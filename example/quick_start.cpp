// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The quick start of the documentation (doc: #trystero-quick-start), which includes the tagged
// regions below: a browser program whose app joins a room, greets every peer that joins, prints
// what it receives, and echoes another action from a coroutine.

// tag::includes[]
#include <webcpp/trystero.hpp>
#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;
// end::includes[]

namespace {

// tag::application[]
/** The room the page joined, and its actions. */
struct lobby {
    trystero::room room;
    trystero::action<std::string> chat = room.make_action<std::string>("chat");
    trystero::action<std::string> echo = room.make_action<std::string>("echo");
};

/** What the page keeps alive: event loop, backend, app, and the room it joined. */
struct application {
    asio::io_context io;
    trystero::browser::event_loop loop{io};
    trystero::browser::backend backend{loop};
    trystero::app app{loop.get_executor(), backend, {.app_id = "webcpp-trystero-quick-start"}};
    std::optional<lobby> joined;

    /** Joins the lobby and makes its actions; null, with the reason printed, when the app refuses
     * (a room of that id joined already, or 64 rooms). */
    lobby* join() {
        auto result = app.join("lobby");
        if (!result) {
            const std::string reason = result.error().message();
            std::printf("not joined: %s\n", reason.c_str());
            return nullptr;
        }
        return &joined.emplace(std::move(*result));
    }
};

// end::application[]

// tag::coroutine[]
/** Answers every message of the "echo" action, one at a time. */
asio::awaitable<void> echo_back(trystero::action<std::string>& echo) {
    for (;;) {
        auto [ec, text, info] = co_await echo.async_receive(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            co_return;  // the room was left
        }
        co_await echo.async_send("echo: " + text,
                                 trystero::send_options{.targets = {info.from}, .metadata = {}},
                                 asio::use_awaitable);
    }
}

// end::coroutine[]

}  // namespace

// tag::main[]
int main() {
    auto* app = new application;  // a page's program lives as long as the tab
    auto* lobby = app->join();
    if (lobby == nullptr) {
        return 1;
    }

    lobby->room.on_peer_join([lobby](std::string_view peer) {
        trystero::send_options to_peer{.targets = {trystero::peer_id(peer)}, .metadata = {}};
        lobby->chat.async_send("hello", std::move(to_peer), [](boost::system::error_code ec) {
            if (ec) {
                std::cerr << "not delivered: " << ec.message() << '\n';
            }
        });
    });
    lobby->chat.on_message([](const std::string& text, const trystero::message_info& info) {
        std::cout << info.from << ": " << text << '\n';
    });
    asio::co_spawn(app->loop.get_executor(), echo_back(lobby->echo), asio::detached);
}

// end::main[]
