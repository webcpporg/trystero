// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)
//
// What both C++ peers of the interop test do, the browser's (peer.cpp) and the native one
// (native_peer.cpp): one app, one room, its actions and its listeners, configured by named
// parameters and reporting each event as a line of JSON, as js.html does for Trystero 0.27.0.
// Each peer gives its parameters, where the events go and where the app's log goes.
//
// | Parameter | Meaning                                                                       |
// | app, room | the app id and room id                                                        |
// | password  | the app password, optional                                                    |
// | relays    | comma-separated relay URLs, optional (Trystero's default list)                |
// | ice       | a STUN or TURN server's URL, optional (Trystero's default list)               |
// | identity  | a user identity's text (`trystero_uk_...`) the app imports, optional          |
// | export    | any value: the app's user is a new identity, whose text the `identity` event  |
// |           | carries, optional                                                             |
// | maxUsers  | the room's `max_users`, optional                                              |
// | wide      | any value: each peer that joins is also sent 1 MiB on the action `wide`       |
// | offline   | any value: the app's relays are offline before it joins, until `online`       |
// | key       | a label whose SHA-256 is the app's endpoint key, so that the driver knows the |
// |           | peer id before the peer joins, optional                                       |
//
// | Event      | Fields                                                                     |
// | joined     | self, user (`app::self_id`, `app::user_id`), relays                        |
// | identity   | text, user: the exported identity (with `export`)                          |
// | peer-join  | peer, user                                                                 |
// | peer-leave | peer, user                                                                 |
// | user-join  | user, peer                                                                 |
// | user-leave | user, peer                                                                 |
// | chat, drink, file, wide | from, and the text, the JSON value, or size, checksum, metadata |
// | request    | from, text: a request this peer answered                                   |
// | response   | peer, text, error: the answer to this peer's request                       |
// | ping       | peer, ms, error                                                            |
// | sent       | what, peer, error                                                          |
// | error      | code (0.27.0's name, or the code's message), error (the message), peer     |
// | left       | error                                                                      |
#ifndef WEBCPP_TRYSTERO_TEST_INTEROP_PEER_ROOM_HPP
#define WEBCPP_TRYSTERO_TEST_INTEROP_PEER_ROOM_HPP

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/identity.hpp>
#include <webcpp/trystero/protocol/topics.hpp>
#include <webcpp/trystero/room.hpp>
#include <webcpp/trystero/room_error.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/quote.hpp>
#include <webcpp/document/stringify.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace interop {

namespace trystero = webcpp::trystero;

/** Appends `text` as `JSON.stringify` writes a string; a text that is not UTF-8, which no
 * JavaScript string is, as `null`. */
inline void append_string(std::string& out, std::string_view text) {
    if (!webcpp::document::quote(text, out).has_value()) {
        out += "null";
    }
}

/** A field of an event for the driver: its key, and its value as JSON text. */
struct field {
    /** A string. */
    field(std::string_view name, std::string_view text) : key(name) { append_string(json, text); }

    /** An integer, in decimal. */
    template <std::integral I>
        requires(!std::same_as<I, bool>)
    field(std::string_view name, I number) : key(name) {
        std::array<char, 24> digits{};
        const auto written = std::to_chars(digits.data(), digits.data() + digits.size(), number);
        json.assign(digits.data(), written.ptr);
    }

    /** Strings, as an array of them. */
    field(std::string_view name, std::span<const std::pmr::string> texts) : key(name) {
        json += '[';
        for (const auto& text : texts) {
            if (json.size() > 1) {
                json += ',';
            }
            append_string(json, text);
        }
        json += ']';
    }

    /** A JSON value, as `JSON.stringify` writes it. */
    field(std::string_view name, const webcpp::document::document& value)
        : key(name), json(value.stringify()) {}

    /** The key. */
    std::string_view key;
    /** The value, as JSON text. */
    std::string json;
};

/** The parameters of a peer, by name: empty for one not given. */
using parameters = std::function<std::string(std::string_view)>;

/** Where a peer's events go, each one JSON object. */
using event_sink = std::function<void(std::string_view)>;

/** The checksum the driver compares payloads by: the sum of each byte times its position, mod
 * 2^32. */
inline std::uint32_t checksum(std::span<const std::uint8_t> data) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < data.size(); ++i) {
        sum += data[i] * static_cast<std::uint32_t>(i + 1);
    }
    return sum;
}

/** `size` bytes, byte i being (i * step) mod 256: the payloads the driver knows the checksums of.
 */
inline trystero::bytes pattern(std::size_t size, std::size_t step) {
    trystero::bytes out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::uint8_t>((i * step) % 256);
    }
    return out;
}

/** The app's configuration, from the parameters, its log going to `log`. */
inline trystero::app_config config_of(const parameters& param, trystero::detail::logger::sink log) {
    trystero::app_config config;
    config.app_id = param("app");
    config.password = param("password");
    for (std::string relays = param("relays"); !relays.empty();) {
        const auto comma = relays.find(',');
        config.relay_urls.emplace_back(relays.substr(0, comma));
        relays = comma == std::string::npos ? std::string() : relays.substr(comma + 1);
    }
    if (const std::string ice = param("ice"); !ice.empty()) {
        config.ice_servers.push_back(
            {.urls = {std::pmr::string(ice)}, .username = {}, .credential = {}});
    }
    config.log = std::move(log);
    return config;
}

/**
 * A C++ peer of the interop test: its app and its room, which sends each peer that joins a chat
 * message, a JSON value, a 100 KB file (and 1 MiB with `wide`), a request and a ping, and echoes
 * each chat message, as js.html's page does.
 */
class peer {
public:
    /**
     * Makes the app and joins the room; an app or a room that cannot be made reports `error`
     * events, and the peer does nothing more.
     *
     * @param ex The app's executor.
     * @param factory The backend.
     * @param param The parameters.
     * @param emit Where the events go.
     * @param log Where the app's log goes.
     */
    peer(const trystero::app::executor_type& ex, trystero::backend::factory& factory,
         const parameters& param, event_sink emit, trystero::detail::logger::sink log)
        : emit_(std::move(emit)) {
        auto config = config_of(param, std::move(log));
        if (const std::string text = param("identity"); !text.empty()) {
            auto imported = trystero::import_user_identity(text);
            if (!imported) {
                record("error",
                       {{"code", "identity-import"}, {"error", imported.error().message()}});
                return;
            }
            config.identity = *imported;
        } else if (!param("export").empty()) {
            auto created = trystero::create_user_identity();
            if (!created) {
                record("error",
                       {{"code", "identity-create"}, {"error", created.error().message()}});
                return;
            }
            record("identity", {{"text", trystero::export_user_identity(*created)},
                                {"user", created->user_id()}});
            config.identity = *created;
        }
        if (const std::string key = param("key"); !key.empty()) {
            // The app's endpoint key, named by its label (its scalar is the label's SHA-256),
            // through the app's detail seam, which the library keeps for its own tests: 0.27.0 has
            // no option for it. The driver derives the peer id from the label, to order the peers'
            // ids.
            app_.emplace(trystero::detail::app_access::make(ex, factory, std::move(config),
                                                            trystero::protocol::sha256(key)));
        } else {
            app_.emplace(ex, factory, std::move(config));
        }
        app_->on_error([this](const trystero::room_error& e) { record_error(e); });
        if (!param("offline").empty()) {
            app_->set_relay_connectivity(trystero::relay_connectivity::offline);
        }
        trystero::room_options options;
        if (const std::string max = param("maxUsers"); !max.empty()) {
            options.max_users = std::stoul(max);
        }
        auto joined = app_->join(param("room"), options);
        if (!joined) {
            record("error", {{"code", "join"}, {"error", joined.error().message()}});
            return;
        }
        room_.emplace(std::move(*joined));
        wide_ = !param("wide").empty();
        record("joined", {{"self", app_->self_id()},
                          {"user", app_->user_id()},
                          {"relays", room_->relay_urls()}});
        listen();
    }

    /** Leaves the room with Trystero's farewell; `then` runs once it is left. */
    void leave(std::function<void()> then = {}) {
        if (!room_) {
            return;
        }
        room_->async_leave([this, then = std::move(then)](boost::system::error_code ec) {
            record("left", {{"error", ec ? ec.message() : ""}});
            if (then) {
                then();
            }
        });
    }

    /** Sets the app's relay connectivity, `setRelayConnectivity`. */
    void set_connectivity(trystero::relay_connectivity mode) {
        if (app_) {
            app_->set_relay_connectivity(mode);
            record(
                "connectivity",
                {{"mode", mode == trystero::relay_connectivity::offline ? "offline" : "online"}});
        }
    }

private:
    /** An event: a kind and fields, the kind last. */
    void record(std::string_view kind, std::initializer_list<field> fields = {}) const {
        std::string text = "{";
        for (const field& f : fields) {
            append_string(text, f.key);
            text += ':';
            text += f.json;
            text += ',';
        }
        append_string(text, "kind");
        text += ':';
        append_string(text, kind);
        text += '}';
        emit_(text);
    }

    /** An error, its code by 0.27.0's name, or by its message for a code 0.27.0 has not. */
    void record_error(const trystero::room_error& e) const {
        const std::string_view name = trystero::room_error_code_name(e.code);
        record("error", {{"code", name.empty() ? e.code.message() : std::string(name)},
                         {"error", e.message},
                         {"peer", e.peer_id}});
    }

    /** A completion of a send to `peer` that records it as `what`. */
    auto sent(std::string_view what, const trystero::peer_id& peer) const {
        return [this, what = std::string(what), peer](boost::system::error_code ec) {
            record("sent", {{"what", what}, {"peer", peer}, {"error", ec ? ec.message() : ""}});
        };
    }

    void greet(const trystero::peer_id& peer) {
        const auto to = [&peer](std::string_view metadata = {}) {
            return trystero::send_options{.targets = {peer},
                                          .metadata = std::pmr::string(metadata)};
        };
        chat_->async_send("hello from C++", to(), sent("chat", peer));
        auto cafezinho =
            webcpp::document::parse(R"({"drink":"cafezinho","withIce":false,"from":"c++"})");
        if (cafezinho.has_value()) {
            drink_->async_send(std::move(*cafezinho), to(), sent("drink", peer));
        }
        file_->async_send(pattern(100'000, 13), to(R"({"name":"from-cpp.bin"})"),
                          sent("file", peer));
        if (wide_) {
            wide_action_->async_send(pattern(1024 * 1024, 13), to(), sent("wide", peer));
        }
        question_->async_request(
            "question from C++", trystero::request_options{.target = peer, .metadata = {}},
            [this, peer](boost::system::error_code ec, std::string answer) {
                record("response",
                       {{"peer", peer}, {"text", answer}, {"error", ec ? ec.message() : ""}});
            });
        room_->async_ping(
            peer, [this, peer](boost::system::error_code ec, std::chrono::milliseconds rtt) {
                record("ping",
                       {{"peer", peer}, {"error", ec ? ec.message() : ""}, {"ms", rtt.count()}});
            });
    }

    void listen() {
        auto& room = *room_;
        chat_.emplace(room.make_action<std::string>("chat"));
        drink_.emplace(room.make_action<webcpp::document::document>("drink"));
        file_.emplace(room.make_action<trystero::bytes>("file"));
        wide_action_.emplace(room.make_action<trystero::bytes>("wide"));
        question_.emplace(room.make_request_action<std::string, std::string>("question"));
        room.on_peer_join([this](const trystero::peer_event& e) {
            const trystero::peer_id peer(e.peer_id);
            record("peer-join", {{"peer", e.peer_id}, {"user", e.user_id}});
            greet(peer);
        });
        room.on_peer_leave([this](const trystero::peer_event& e) {
            record("peer-leave", {{"peer", e.peer_id}, {"user", e.user_id}});
        });
        room.on_user_join([this](const trystero::user_event& e) {
            record("user-join", {{"user", e.user_id}, {"peer", e.peer_id}});
        });
        room.on_user_leave([this](const trystero::user_event& e) {
            record("user-leave", {{"user", e.user_id}, {"peer", e.peer_id}});
        });
        room.on_error([this](const trystero::room_error& e) { record_error(e); });
        chat_->on_message([this](std::string text, const trystero::message_info& info) {
            record("chat", {{"from", info.from}, {"text", text}});
            if (!text.starts_with("echo: ")) {
                chat_->async_send("echo: " + text,
                                  trystero::send_options{.targets = {info.from}, .metadata = {}},
                                  [](boost::system::error_code) {});
            }
        });
        drink_->on_message(
            [this](const webcpp::document::document& v, const trystero::message_info& info) {
                record("drink", {{"from", info.from}, {"value", v}});
            });
        const auto bytes_of = [this](std::string_view kind) {
            return [this, kind](const trystero::bytes& data, const trystero::message_info& info) {
                record(kind, {{"from", info.from},
                              {"size", data.size()},
                              {"checksum", checksum(data)},
                              {"metadata", info.metadata}});
            };
        };
        file_->on_message(bytes_of("file"));
        wide_action_->on_message(bytes_of("wide"));
        const auto answering =
            question_->on_request([this](std::string text, const trystero::message_info& info) {
                record("request", {{"from", info.from}, {"text", text}});
                return "answer to: " + text;
            });
        if (!answering) {
            record("error", {{"code", "on-request"}, {"error", answering.error().message()}});
        }
    }

    event_sink emit_;
    std::optional<trystero::app> app_;
    std::optional<trystero::room> room_;
    std::optional<trystero::action<std::string>> chat_;
    std::optional<trystero::action<webcpp::document::document>> drink_;
    std::optional<trystero::action<trystero::bytes>> file_;
    std::optional<trystero::action<trystero::bytes>> wide_action_;
    std::optional<trystero::request_action<std::string, std::string>> question_;
    bool wide_ = false;
};

}  // namespace interop

#endif
