// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A chat room in the browser: the room is C++ (webcpp::trystero), the page only draws. It joins
// over Trystero's public Nostr relays, so Trystero JS peers with the same app id and room id are in
// the same chat.
#include <webcpp/trystero.hpp>
#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript export

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <string>

using emscripten::val;
namespace trystero = webcpp::trystero;

namespace {

// tag::state[]
struct chat {
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    trystero::browser::backend backend{loop};
    std::unique_ptr<trystero::room> room;
    // The action "chat" twice: received as views of the frame, and sent by moving the text.
    std::unique_ptr<trystero::action<std::string_view>> messages;
    std::unique_ptr<trystero::action<std::string>> outgoing;
};

// end::state[]

chat& the_chat() {
    static chat c;  // the page's one chat; it lives as long as the tab
    return c;
}

val js(std::string_view text) {
    return the_chat().loop.text(text);
}

val js(bool value) {
    return val(value);
}

/** Calls `window.chatUi[name](args...)`, the page's drawing functions. */
template <class... Args>
void ui(const char* name, const Args&... args) {
    val::global("chatUi").call<void>(name, js(args)...);
}

// tag::join[]
void join_room(const std::string& app_id, const std::string& room_id) {
    auto& c = the_chat();
    if (c.room) {
        return;
    }
    trystero::room_config config;
    config.app_id = app_id;
    config.log = [](std::string_view category, std::string_view text) {
        ui("log", category, text);
    };
    c.room = std::make_unique<trystero::room>(c.loop.get_executor(), c.backend, std::move(config),
                                              room_id);
    c.messages = std::make_unique<trystero::action<std::string_view>>(
        c.room->make_action<std::string_view>("chat"));
    c.outgoing =
        std::make_unique<trystero::action<std::string>>(c.room->make_action<std::string>("chat"));
    c.room->on_peer_join([](std::string_view peer) { ui("peerJoined", peer); });
    c.room->on_peer_leave([](std::string_view peer) { ui("peerLeft", peer); });
    // A view of the received frame: the text goes from the frame straight into a JavaScript string.
    c.messages->on_message([](std::string_view text, const trystero::message_info& info) {
        ui("message", std::string_view(info.from), text, false);
    });
    ui("joined", c.room->self_id(), std::string_view(room_id));
}

// end::join[]

// tag::send[]
void send_message(std::string text) {
    auto& c = the_chat();
    if (!c.room || text.empty()) {
        return;
    }
    ui("message", c.room->self_id(), std::string_view(text), true);
    c.outgoing->async_send(std::move(text), [](boost::system::error_code ec) {
        if (ec) {
            ui("log", std::string_view("chat"), std::string_view("not delivered: " + ec.message()));
        }
    });
}

// end::send[]

}  // namespace

// tag::bindings[]
EMSCRIPTEN_BINDINGS(chat_example) {
    emscripten::function("join", &join_room);
    emscripten::function("send", &send_message);
}

// end::bindings[]
