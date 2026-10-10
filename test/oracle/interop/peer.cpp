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
// | Query parameter | Meaning                                                          |
// | app, room       | the app id and room id                                           |
// | password        | the room password, optional                                      |
// | self            | this peer's id, optional                                         |
// | relays          | comma-separated relay URLs, optional (Trystero's default list)   |
// | ice             | a STUN or TURN server's URL, optional (Trystero's default list)  |
// | room2           | a second room of the same app to join too, optional              |
// | program         | the program's JavaScript, which cpp.html loads; not read here    |
#include <webcpp/trystero.hpp>
#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript export

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <memory>
#include <string>

using emscripten::val;
namespace trystero = webcpp::trystero;

namespace {

/** Everything the page keeps alive: the io_context, the event loop, the backend, the room and its
 * actions. */
struct page {
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    trystero::browser::backend backend{loop};
    std::unique_ptr<trystero::room> room;
    std::unique_ptr<trystero::action<std::string>> chat;
    std::unique_ptr<trystero::action<boost::json::value>> drink;
    std::unique_ptr<trystero::action<trystero::bytes>> file;
    // The second room, with `room2`, and its actions.
    std::unique_ptr<trystero::room> second;
    std::unique_ptr<trystero::action<std::string>> second_chat;
    std::unique_ptr<trystero::action<trystero::bytes>> second_file;
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

void log(std::string_view category, std::string_view text) {
    val::global("window").call<void>("trysteroLog", the_page().loop.text(category),
                                     the_page().loop.text(text));
}

/** An event for the driver: a kind and fields, pushed to `window.events`. */
void record(std::string_view kind, boost::json::object fields = {}) {
    fields["kind"] = kind;
    val::global("window")["events"].call<void>(
        "push", val::global("JSON").call<val>("parse", val(boost::json::serialize(fields))));
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

/** The page's configuration, from the URL. */
trystero::room_config config_of_page() {
    trystero::room_config config;
    config.app_id = param("app");
    config.password = param("password");
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
    config.log = log;
    return config;
}

/**
 * Joins `room2`, a second room of the same app, with the configuration of the first but its room
 * id: it sends each peer that joins a chat message and a 100 KB file, and records what it sees with
 * `room: second`.
 */
void join_second() {
    page& app = the_page();
    app.second = std::make_unique<trystero::room>(app.loop.get_executor(), app.backend,
                                                  config_of_page(), param("room2"));
    app.second_chat = std::make_unique<trystero::action<std::string>>(
        app.second->make_action<std::string>("chat"));
    app.second_file = std::make_unique<trystero::action<trystero::bytes>>(
        app.second->make_action<trystero::bytes>("file"));
    record("joined", {{"room", "second"}, {"self", app.second->self_id()}});
    app.second->on_peer_join([](std::string_view peer_view) {
        const trystero::peer_id peer(peer_view);
        record("peer-join", {{"room", "second"}, {"peer", peer}});
        the_page().second_chat->async_send(
            "hello from C++", trystero::send_options{.targets = {peer}, .metadata = {}},
            [](boost::system::error_code) {});
        trystero::bytes big(100'000);
        for (std::size_t i = 0; i < big.size(); ++i) {
            big[i] = static_cast<std::uint8_t>((i * 13) % 256);
        }
        the_page().second_file->async_send(
            std::move(big), trystero::send_options{.targets = {peer}, .metadata = {}},
            [](boost::system::error_code) {});
    });
    app.second->on_peer_leave(
        [](std::string_view peer) { record("peer-leave", {{"room", "second"}, {"peer", peer}}); });
    app.second->on_error(
        [](std::string_view peer, boost::system::error_code ec, std::string_view what) {
            record("error",
                   {{"room", "second"}, {"peer", peer}, {"error", ec.message()}, {"what", what}});
        });
    app.second_chat->on_message([](std::string text, const trystero::message_info& info) {
        record("chat", {{"room", "second"}, {"from", info.from}, {"text", text}});
    });
    app.second_file->on_message(
        [](const trystero::bytes& data, const trystero::message_info& info) {
            record("file", {
                               {"room", "second"},
                               {"from", info.from},
                               {"size", data.size()},
                               {"checksum", checksum(data)},
                           });
        });
}

void start() {
    page& app = the_page();
    app.room = std::make_unique<trystero::room>(app.loop.get_executor(), app.backend,
                                                config_of_page(), param("room"));
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
                record("ping",
                       {{"peer", peer}, {"error", ec ? ec.message() : ""}, {"ms", rtt.count()}});
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
        join_second();
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
