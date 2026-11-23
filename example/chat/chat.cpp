// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A chat room in the browser: the app and its room are C++ (webcpp::trystero), the page only draws.
// It joins over Trystero's public Nostr relays, so Trystero JS peers with the same app id and room
// id are in the same chat.
#include <webcpp/trystero.hpp>
#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/platform/page.hpp>

#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>

#include <emscripten/val.h>

#include <memory>
#include <string>
#include <utility>

using emscripten::val;
namespace trystero = webcpp::trystero;

namespace {

// tag::state[]
struct chat {
    webcpp::asio::io_context io;
    trystero::platform::backend backend{io.get_executor()};
    std::unique_ptr<trystero::app> app;  // made at the first join, with that join's app id
    std::unique_ptr<trystero::room> room;
    // The action "chat" twice: received as views of the frame, and sent by moving the text.
    std::unique_ptr<trystero::action<std::string_view>> messages;
    std::unique_ptr<trystero::action<std::string>> outgoing;
};

// end::state[]

val js(std::string_view text) {
    return trystero::platform::js_string(text);
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
void join_room(chat& c, const val& form) {
    // The page's value is checked before it is read: a page_call's handler lets no JavaScript
    // error out of the run, which reading a field of null or undefined (a TypeError) or converting
    // what is not a string (embind's BindingError) would.
    if (c.room || form.isNull() || form.isUndefined()) {
        return;
    }
    const val app_field = form["app"];
    const val room_field = form["room"];
    if (!app_field.isString() || !room_field.isString()) {
        return;
    }
    const auto room_id = room_field.as<std::string>();
    if (!c.app) {
        trystero::app_config config;
        config.app_id = app_field.as<std::string>();
        config.log = [](std::string_view category, std::string_view text) {
            ui("log", category, text);
        };
        c.app = std::make_unique<trystero::app>(c.io.get_executor(), c.backend, std::move(config));
    }
    auto joined = c.app->join(room_id);
    if (!joined) {
        ui("log", std::string_view("chat"),
           std::string_view("not joined: " + joined.error().message()));
        return;
    }
    c.room = std::make_unique<trystero::room>(std::move(*joined));
    c.messages = std::make_unique<trystero::action<std::string_view>>(
        c.room->make_action<std::string_view>("chat"));
    c.outgoing =
        std::make_unique<trystero::action<std::string>>(c.room->make_action<std::string>("chat"));
    c.room->on_peer_join([](const trystero::peer_event& e) { ui("peerJoined", e.peer_id); });
    c.room->on_peer_leave([](const trystero::peer_event& e) { ui("peerLeft", e.peer_id); });
    // A view of the received frame: the text goes from the frame straight into a JavaScript string.
    c.messages->on_message([](std::string_view text, const trystero::message_info& info) {
        ui("message", std::string_view(info.from), text, false);
    });
    ui("joined", c.room->self_id(), std::string_view(room_id));
}

// end::join[]

// tag::send[]
void send_message(chat& c, const val& message) {
    if (!c.room || !message.isString()) {
        return;
    }
    auto text = message.as<std::string>();
    if (text.empty()) {
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

// tag::main[]
int main() {
    chat c;  // the page's one chat, which lives as long as the tab, in main's run
    // The page's two calls, functions of webcpp::asio's bridge that post their work to the
    // io_context: joinRoom with the form's app and room, sendText with the text its user wrote.
    const trystero::platform::page_call join(c.io.get_executor(),
                                             [&c](const val& form) { join_room(c, form); });
    const trystero::platform::page_call send(c.io.get_executor(),
                                             [&c](const val& text) { send_message(c, text); });
    val::global("chatUi").call<void>("ready", join.function(), send.function());
    // The page waits for its user with no work at all: the guard keeps the run going.
    const auto guard = webcpp::asio::make_work_guard(c.io);
    c.io.run();  // lint-run: the page's program, suspended into the browser's event loop
}

// end::main[]
