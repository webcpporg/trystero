// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)
//
// The C++ side of the interop test: a page that joins a room with the library, built for the
// browser. The driver (run.mjs) configures it through the URL and reads what happened from
// `window.events`.
//
// | Query parameter | Meaning                                                            |
// | app, room       | the app id and room id                                             |
// | password        | the room password, optional                                        |
// | self            | this peer's id, optional                                           |
// | relays          | comma-separated relay URLs, optional (Trystero's default list)     |
// | ice             | a STUN or TURN server's URL, optional (Trystero's default list)    |
// | room2           | a second room of the same app id to join too, optional             |
// | room3           | a third room of the same app, optional                             |
// | room2Delay      | the ms before joining room2, optional (0)                          |
// | room3Delay      | the ms before joining room3, optional (0)                          |
// | password2       | room2's password, optional (password's)                            |
// | password3       | room3's password, optional (none)                                  |
// | idle            | the app's idle_timeout in ms, optional (123,333)                   |
// | handshake       | every room's handshake_timeout in ms, optional (10,000)            |
// | ping2           | any value: ping each peer of room2 every second, optional          |
// | program         | the program's JavaScript, which cpp.html loads; not read here      |
#include <webcpp/trystero.hpp>
#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript export

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <system_error>

using emscripten::val;
namespace trystero = webcpp::trystero;

namespace {

/** A further room of the app, room2 or room3, and its actions; its events carry `room: <which>`. */
struct extra_room {
    std::string which;
    std::unique_ptr<trystero::room> room;
    std::unique_ptr<trystero::action<std::string>> chat;
    std::unique_ptr<trystero::action<trystero::bytes>> file;
};

/** Everything the page keeps alive: the io_context, the event loop, the backend, the app, the room
 * and its actions. */
struct page {
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    trystero::browser::backend backend{loop};
    std::unique_ptr<trystero::app> app;
    std::unique_ptr<trystero::room> room;
    std::unique_ptr<trystero::action<std::string>> chat;
    std::unique_ptr<trystero::action<boost::json::value>> drink;
    std::unique_ptr<trystero::action<trystero::bytes>> file;
    extra_room second{.which = "second", .room = {}, .chat = {}, .file = {}};
    extra_room third{.which = "third", .room = {}, .chat = {}, .file = {}};
    std::shared_ptr<trystero::backend::timer> second_delay;
    std::shared_ptr<trystero::backend::timer> third_delay;
    std::shared_ptr<trystero::backend::timer> pinger;
    std::uint64_t next_ping = 0;
};

/** The page's one state, made when the driver starts the peer, kept as long as the tab. */
page& the_page() {
    static page state;
    return state;
}

std::string param(std::string_view name) {
    const val value = val::global("URLSearchParams")
                          .new_(val::global("location")["search"])
                          .call<val>("get", val(std::string(name)));
    return value.isNull() ? std::string() : value.as<std::string>();
}

/** An event for the driver: a kind and fields, pushed to `window.events`. */
void record(std::string_view kind, boost::json::object fields = {}) {
    fields["kind"] = kind;
    val::global("window")["events"].call<void>(
        "push", val::global("JSON").call<val>("parse", val(boost::json::serialize(fields))));
}

/** Milliseconds from a parameter's digits, or `fallback` when it has none. */
std::chrono::milliseconds millis(std::string_view text, std::chrono::milliseconds fallback) {
    std::int64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [at, ec] = std::from_chars(begin, end, value);
    return text.empty() || ec != std::errc{} || at != end || value < 0
               ? fallback
               : std::chrono::milliseconds(value);
}

/** The events the driver counts connections by, from the app's log lines
 * `connection with <peer> adopted` and `connection with <peer> ended: <reason>`. */
void record_connection(std::string_view category, std::string_view text) {
    constexpr std::string_view prefix = "connection with ";
    constexpr std::string_view adopted = " adopted";
    constexpr std::string_view ended = " ended: ";
    if (category != "webrtc" || !text.starts_with(prefix)) {
        return;
    }
    const auto rest = text.substr(prefix.size());
    if (rest.ends_with(adopted)) {
        record("adopted", {{"peer", rest.substr(0, rest.size() - adopted.size())}});
    } else if (const auto at = rest.find(ended); at != std::string_view::npos) {
        record("ended", {{"peer", rest.substr(0, at)}, {"reason", rest.substr(at + ended.size())}});
    }
}

void log(std::string_view category, std::string_view text) {
    record_connection(category, text);
    val::global("window").call<void>("trysteroLog", the_page().loop.text(category),
                                     the_page().loop.text(text));
}

/** A checksum the driver can compare without moving the payload: sum of bytes times their position,
 * mod 2^32. */
std::uint32_t checksum(const trystero::bytes& data) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < data.size(); ++i) {
        sum += data[i] * static_cast<std::uint32_t>(i + 1);
    }
    return sum;
}

/** The app's configuration, from the URL. */
trystero::app_config config_of_page() {
    trystero::app_config config;
    config.app_id = param("app");
    config.self_id = param("self");
    for (std::string relays = param("relays"); !relays.empty();) {
        const auto comma = relays.find(',');
        config.relay_urls.emplace_back(relays.substr(0, comma));
        relays = comma == std::string::npos ? std::string() : relays.substr(comma + 1);
    }
    if (const std::string ice = param("ice"); !ice.empty()) {
        config.ice_servers.push_back(
            {.urls = {std::pmr::string(ice)}, .username = {}, .credential = {}});
    }
    config.idle_timeout = millis(param("idle"), std::chrono::milliseconds(123'333));
    config.log = log;
    return config;
}

/** A room's options: `password`, and the page's handshake timeout. */
trystero::room_options options_of_page(std::string_view password) {
    return {
        .password = std::pmr::string(password),
        .handshake_timeout = millis(param("handshake"), std::chrono::milliseconds(10'000)),
    };
}

/** Joins `room_id` through `app`; null, with an event, when the app refuses. */
std::unique_ptr<trystero::room> join(trystero::app& app, const std::string& room_id,
                                     std::string_view which) {
    auto joined = app.join(room_id, options_of_page(param("password")));
    if (!joined) {
        record("not-joined", {{"room", which}, {"error", joined.error().message()}});
        return nullptr;
    }
    return std::make_unique<trystero::room>(std::move(*joined));
}

/**
 * Joins `room_id`, a further room of the app, as `extra`: it sends each peer that joins a chat
 * message and a 100 KB file, echoes each chat message it receives, and records what it sees with
 * `room: <which>`.
 */
void join_extra(extra_room& extra, std::string_view room_id, std::string_view password) {
    page& app = the_page();
    auto joined = app.app->join(room_id, options_of_page(password));
    if (!joined) {
        record("error", {
                            {"room", extra.which},
                            {"peer", ""},
                            {"error", joined.error().message()},
                            {"what", "join"},
                        });
        return;
    }
    extra.room = std::make_unique<trystero::room>(std::move(*joined));
    extra.chat = std::make_unique<trystero::action<std::string>>(
        extra.room->make_action<std::string>("chat"));
    extra.file = std::make_unique<trystero::action<trystero::bytes>>(
        extra.room->make_action<trystero::bytes>("file"));
    record("joined", {{"room", extra.which}, {"self", extra.room->self_id()}});
    extra.room->on_peer_join([&extra](std::string_view peer_view) {
        const trystero::peer_id peer(peer_view);
        record("peer-join", {{"room", extra.which}, {"peer", peer}});
        extra.chat->async_send("hello from C++",
                               trystero::send_options{.targets = {peer}, .metadata = {}},
                               [](boost::system::error_code) {});
        trystero::bytes big(100'000);
        for (std::size_t i = 0; i < big.size(); ++i) {
            big[i] = static_cast<std::uint8_t>((i * 13) % 256);
        }
        extra.file->async_send(std::move(big),
                               trystero::send_options{.targets = {peer}, .metadata = {}},
                               [](boost::system::error_code) {});
    });
    extra.room->on_peer_leave([&extra](std::string_view peer) {
        record("peer-leave", {{"room", extra.which}, {"peer", peer}});
    });
    extra.room->on_error([&extra](std::string_view peer, boost::system::error_code ec,
                                  std::string_view what) {
        record("error",
               {{"room", extra.which}, {"peer", peer}, {"error", ec.message()}, {"what", what}});
    });
    extra.chat->on_message([&extra](std::string text, const trystero::message_info& info) {
        record("chat", {{"room", extra.which}, {"from", info.from}, {"text", text}});
        if (!text.starts_with("echo: ")) {
            extra.chat->async_send("echo: " + text,
                                   trystero::send_options{.targets = {info.from}, .metadata = {}},
                                   [](boost::system::error_code) {});
        }
    });
    extra.file->on_message(
        [&extra](const trystero::bytes& data, const trystero::message_info& info) {
            record("file", {
                               {"room", extra.which},
                               {"from", info.from},
                               {"size", data.size()},
                               {"checksum", checksum(data)},
                           });
        });
}

/** Pings every peer of room2, then again a second later, recording each with `room: second`. */
void ping_second() {
    page& app = the_page();
    if (app.second.room) {
        for (const auto& peer : app.second.room->peers()) {
            const auto id = ++app.next_ping;
            record("ping-started", {{"room", "second"}, {"peer", peer}, {"id", id}});
            app.second.room->async_ping(
                peer, [peer = trystero::peer_id(peer), id](boost::system::error_code ec,
                                                           std::chrono::milliseconds rtt) {
                    record("ping", {
                                       {"room", "second"},
                                       {"peer", peer},
                                       {"id", id},
                                       {"error", ec ? ec.message() : ""},
                                       {"ms", rtt.count()},
                                   });
                });
        }
    }
    app.pinger->start(std::chrono::seconds(1), [] { ping_second(); });
}

void start() {
    page& app = the_page();
    app.app =
        std::make_unique<trystero::app>(app.loop.get_executor(), app.backend, config_of_page());
    app.app->on_error(
        [](std::string_view peer, boost::system::error_code ec, std::string_view what) {
            record("error", {{"peer", peer}, {"error", ec.message()}, {"what", what}});
        });
    app.room = join(*app.app, param("room"), "first");
    if (!app.room) {
        return;
    }
    auto& room = *app.room;
    boost::json::array relays;
    for (const auto& url : room.relay_urls()) {
        relays.emplace_back(url);
    }
    record("joined", {{"self", room.self_id()}, {"relays", relays}});

    app.chat =
        std::make_unique<trystero::action<std::string>>(room.make_action<std::string>("chat"));
    app.drink = std::make_unique<trystero::action<boost::json::value>>(
        room.make_action<boost::json::value>("drink"));
    app.file = std::make_unique<trystero::action<trystero::bytes>>(
        room.make_action<trystero::bytes>("file"));

    room.on_peer_join([](std::string_view peer_view) {
        const trystero::peer_id peer(peer_view);
        record("peer-join", {{"peer", peer}});
        // Each handler holds a copy of its own, not const, so that moving the handler moves it.
        const auto done = [peer](const char* what) {
            return [peer = trystero::peer_id(peer), what](boost::system::error_code ec) {
                record("sent", {{"what", what}, {"peer", peer}, {"error", ec ? ec.message() : ""}});
            };
        };
        const trystero::send_options to_peer{.targets = {peer}, .metadata = {}};
        the_page().chat->async_send("hello from C++", to_peer, done("chat"));
        the_page().drink->async_send(
            boost::json::object{{"drink", "cafezinho"}, {"withIce", false}, {"from", "c++"}},
            to_peer, done("drink"));
        trystero::bytes big(100'000);
        for (std::size_t i = 0; i < big.size(); ++i) {
            big[i] = static_cast<std::uint8_t>((i * 13) % 256);
        }
        the_page().file->async_send(
            big,
            trystero::send_options{.targets = {peer}, .metadata = R"({"name":"from-cpp.bin"})"},
            done("file"));
        the_page().room->async_ping(
            peer, [peer = trystero::peer_id(peer)](boost::system::error_code ec,
                                                   std::chrono::milliseconds rtt) {
                record("ping", {
                                   {"peer", peer},
                                   {"error", ec ? ec.message() : ""},
                                   {"ms", rtt.count()},
                               });
            });
    });
    room.on_peer_leave([](std::string_view peer) { record("peer-leave", {{"peer", peer}}); });
    room.on_error([](std::string_view peer, boost::system::error_code ec, std::string_view what) {
        record("error", {{"peer", peer}, {"error", ec.message()}, {"what", what}});
    });
    app.chat->on_message([](std::string text, const trystero::message_info& info) {
        record("chat", {{"from", info.from}, {"text", text}});
        if (!text.starts_with("echo: ")) {
            the_page().chat->async_send(
                "echo: " + text, trystero::send_options{.targets = {info.from}, .metadata = {}},
                [](boost::system::error_code) {});
        }
    });
    app.drink->on_message([](boost::json::value v, const trystero::message_info& info) {
        record("drink", {{"from", info.from}, {"value", v}});
    });
    app.file->on_message([](const trystero::bytes& data, const trystero::message_info& info) {
        record("file", {
                           {"from", info.from},
                           {"size", data.size()},
                           {"checksum", checksum(data)},
                           {"metadata", info.metadata},
                       });
    });
    if (!param("room2").empty()) {
        app.second_delay = app.backend.make_timer(std::pmr::get_default_resource());
        app.second_delay->start(millis(param("room2Delay"), std::chrono::milliseconds(0)), [] {
            const std::string password2 = param("password2");
            join_extra(the_page().second, param("room2"),
                       password2.empty() ? param("password") : password2);
        });
    }
    if (!param("room3").empty()) {
        app.third_delay = app.backend.make_timer(std::pmr::get_default_resource());
        app.third_delay->start(millis(param("room3Delay"), std::chrono::milliseconds(0)), [] {
            join_extra(the_page().third, param("room3"), param("password3"));
        });
    }
    if (!param("ping2").empty()) {
        app.pinger = app.backend.make_timer(std::pmr::get_default_resource());
        ping_second();
    }
}

/** Called by the driver: leave the room with Trystero's farewell. */
void leave() {
    the_page().room->async_leave(
        [](boost::system::error_code ec) { record("left", {{"error", ec ? ec.message() : ""}}); });
}

}  // namespace

EMSCRIPTEN_BINDINGS(interop_peer) {
    emscripten::function("start", &start);
    emscripten::function("leave", &leave);
}
