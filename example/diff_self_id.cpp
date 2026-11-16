// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences-behaviour): the scope of the self id.
// Trystero JS draws one selfId for the page, which it announces in every room of every app; an
// app here draws its own, and keeps its connections, so that one program holds several peers, and
// one id across apps is the program's choice, app_config::self_id. This program joins two rooms of
// one app and a room of a second app, and another peer, two apps of the same program, compares the
// ids it meets there. Its twin is test/oracle/twins/diff_self_id.mjs. Both meet over the relay
// whose URL is in the environment, TRYSTERO_RELAY_URL, which their lanes start on 127.0.0.1.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;

int main() {
    const char* const relay = std::getenv("TRYSTERO_RELAY_URL");
    if (relay == nullptr) {
        std::cerr << "TRYSTERO_RELAY_URL names no relay\n";
        return 1;
    }
    asio::io_context io;
    // The peers meet on this machine alone: every connection bound to the loopback, and a STUN
    // server where nothing answers in place of Trystero's public ones.
    trystero::platform::backend backend(io.get_executor(), {.bind_address = "127.0.0.1"});
    const auto config = [relay](std::string_view app_id) {
        trystero::app_config c;
        c.app_id = app_id;
        c.relay_urls = {relay};
        c.ice_servers = {{.urls = {"stun:127.0.0.1:9"}, .username = {}, .credential = {}}};
        // The connection closes once no room uses it, so that the program ends when all leave.
        c.idle_timeout = std::chrono::milliseconds{0};
        return c;
    };
    constexpr std::string_view first_app = "webcpp-trystero-differences";
    constexpr std::string_view second_app = "webcpp-trystero-differences-second";
    // The other peer, as another page would be: an app of each id, in the rooms this program joins.
    trystero::app other_first(io.get_executor(), backend, config(first_app));
    trystero::app other_second(io.get_executor(), backend, config(second_app));
    auto other_lobby = other_first.join("lobby");
    auto other_kitchen = other_first.join("kitchen");
    auto other_second_lobby = other_second.join("lobby");
    // tag::example[]
    // Each app draws a self id of its own, unless app_config::self_id names one for both.
    trystero::app first(io.get_executor(), backend, config(first_app));
    trystero::app second(io.get_executor(), backend, config(second_app));
    auto lobby = first.join("lobby");
    auto kitchen = first.join("kitchen");
    auto second_lobby = second.join("lobby");
    // What the other peer meets in those three rooms.
    const auto report = [](std::string_view in_lobby, std::string_view in_kitchen,
                           std::string_view in_second_lobby) {
        std::cout << "two rooms of one app: " << (in_lobby == in_kitchen ? "one id" : "two ids")
                  << '\n'
                  << "rooms of two apps: " << (in_lobby == in_second_lobby ? "one id" : "two ids")
                  << '\n';
    };
    // end::example[]
    if (!other_lobby || !other_kitchen || !other_second_lobby || !lobby || !kitchen ||
        !second_lobby) {
        std::cerr << "not joined\n";
        return 1;
    }
    // The id each of the other peer's rooms meets first; once all three have met one, the program
    // reports, and every room leaves.
    const std::array observers{&*other_lobby, &*other_kitchen, &*other_second_lobby};
    std::array<std::string, 3> met;
    std::size_t meetings = 0;
    for (std::size_t i = 0; i < observers.size(); ++i) {
        observers[i]->on_peer_join([&, i](std::string_view peer) {
            if (!met[i].empty()) {
                return;
            }
            met[i] = peer;
            if (++meetings < met.size()) {
                return;
            }
            report(met[0], met[1], met[2]);
            for (trystero::room* room : {&*lobby, &*kitchen, &*second_lobby}) {
                room->async_leave([](boost::system::error_code) {});
            }
            for (trystero::room* room : observers) {
                room->async_leave([](boost::system::error_code) {});
            }
        });
    }
    io.run();  // lint-run: the program's network I/O; libdatachannel's threads post to it
}
