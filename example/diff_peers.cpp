// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences): what a room lists of its peers.
// Trystero's getPeers maps each joined peer's id to its RTCPeerConnection; room::peers lists the
// ids, since a connection is the backend's and the room does not hand it out. Two apps of one
// program meet in a room, as two pages would. Its twin is test/oracle/twins/diff_peers.mjs. Both
// meet over the relay whose URL is in the environment, TRYSTERO_RELAY_URL, which their lanes start
// on 127.0.0.1.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace trystero = webcpp::trystero;

int main() {
    const char* const relay = std::getenv("TRYSTERO_RELAY_URL");
    if (relay == nullptr) {
        std::cerr << "TRYSTERO_RELAY_URL names no relay\n";
        return 1;
    }
    boost::asio::io_context io;
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
    room.on_peer_join([&](std::string_view peer_id) {
        const auto peers = room.peers();
        std::cout << "peers: " << peers.size() << '\n'
                  << "the joined peer's entry: "
                  << (peers.size() == 1 && peers.front() == peer_id ? "its id" : "not its id")
                  << '\n';
        leave();
    });
    // end::example[]
    io.run();  // lint-run: the program's network I/O; libdatachannel's threads post to it
}
