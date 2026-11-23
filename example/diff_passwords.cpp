// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences-behaviour): passwords across rooms.
// Trystero JS takes a password with each room, so that one page holds rooms of different
// passwords, each room with its own connection to a peer it meets in several, which the sealed
// negotiation of that room authenticates: 0.27.0 has no password challenge on the connection. A
// password here is an app's, the same for every room of the app and never changed, the app's room
// keys derived from it: rooms of different passwords are rooms of separate apps, each with its own
// self id. This program joins
// a lobby with one password and a kitchen with another, as another peer of the same program does,
// and compares the peers its two rooms meet. Its twin is test/oracle/twins/diff_passwords.mjs.
// Both meet over the relay whose URL is in the environment, TRYSTERO_RELAY_URL, which their lanes
// start on 127.0.0.1.

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
    const auto config = [relay](std::string_view password) {
        trystero::app_config c;
        c.app_id = "webcpp-trystero-differences";
        c.password = password;
        c.relay_urls = {relay};
        c.ice_servers = {{.urls = {"stun:127.0.0.1:9"}, .username = {}, .credential = {}}};
        return c;
    };
    // The other peer, as another page would be, with the same passwords for the same rooms.
    trystero::app other_one(io.get_executor(), backend, config("one"));
    trystero::app other_two(io.get_executor(), backend, config("two"));
    auto other_lobby = other_one.join("lobby");
    auto other_kitchen = other_two.join("kitchen");
    // tag::example[]
    // A password is an app's: a room with another password is a room of another app.
    trystero::app one(io.get_executor(), backend, config("one"));
    trystero::app two(io.get_executor(), backend, config("two"));
    auto lobby = one.join("lobby");
    auto kitchen = two.join("kitchen");
    // What the two rooms meet.
    const auto report = [](std::string_view in_lobby, std::string_view in_kitchen) {
        std::cout << "the lobby, with the password \"one\": joined\n"
                  << "the kitchen, with the password \"two\": joined\n"
                  << "the peer in both rooms: "
                  << (in_lobby == in_kitchen ? "one peer" : "two peers") << '\n';
    };
    // end::example[]
    if (!other_lobby || !other_kitchen || !lobby || !kitchen) {
        std::cerr << "not joined\n";
        return 1;
    }
    // The peer each of this program's rooms meets first; once both have met one, the program
    // reports, and every room leaves.
    const std::array<trystero::room*, 2> rooms{&*lobby, &*kitchen};
    std::array<std::string, 2> met;
    std::size_t meetings = 0;
    for (std::size_t i = 0; i < rooms.size(); ++i) {
        rooms[i]->on_peer_join([&, i](const trystero::peer_event& joined) {
            if (!met[i].empty()) {
                return;
            }
            met[i] = joined.peer_id;
            if (++meetings < met.size()) {
                return;
            }
            report(met[0], met[1]);
            for (trystero::room* room : {&*lobby, &*kitchen, &*other_lobby, &*other_kitchen}) {
                room->async_leave([](boost::system::error_code) {});
            }
        });
    }
    io.run();  // lint-run: the program's network I/O; libdatachannel's threads post to it
}
