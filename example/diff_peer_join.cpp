// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences-behaviour): a join handler set once a
// peer has joined. Trystero calls a newly set onPeerJoin with each peer already joined; a room
// reports a join from its executor once, to the handler set then, and peers() lists the peers
// joined. Two apps of one program meet in a room, as two pages would. Its twin is
// test/oracle/twins/diff_peer_join.mjs. Both meet over the relay whose URL is in the environment,
// TRYSTERO_RELAY_URL, which their lanes start on 127.0.0.1.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

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
    const auto config = [relay] {
        trystero::app_config c;
        c.app_id = "webcpp-trystero-differences";
        c.relay_urls = {relay};
        c.ice_servers = {{.urls = {"stun:127.0.0.1:9"}, .username = {}, .credential = {}}};
        // The connection closes once no room uses it, so that the program ends when both leave.
        c.idle_timeout = std::chrono::milliseconds{0};
        return c;
    };
    trystero::app other_app(io.get_executor(), backend, config());
    auto other = other_app.join("lobby");
    trystero::app app(io.get_executor(), backend, config());
    auto joined = app.join("lobby");
    if (!other || !joined) {
        std::cerr << "not joined\n";
        return 1;
    }
    trystero::room room = std::move(*joined);
    const auto leave = [&] {
        room.async_leave([](boost::system::error_code) {});
        other->async_leave([](boost::system::error_code) {});
    };
    // tag::example[]
    int late_calls = 0;
    room.on_peer_join([&](std::string_view) {
        std::cout << "the handler set before the join: called\n";
        // Once the join is reported, another handler replaces this one.
        asio::post(io, [&] {
            room.on_peer_join([&late_calls](std::string_view) { ++late_calls; });
            asio::post(io, [&] {
                std::cout << "the handler set after the join: "
                          << (late_calls == 0 ? "not called" : "called") << '\n'
                          << "peers: " << room.peers().size() << '\n';
                leave();
            });
        });
    });
    // end::example[]
    io.run();  // lint-run: the program's network I/O; libdatachannel's threads post to it
}
