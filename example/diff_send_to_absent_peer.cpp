// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences): a send to a peer that has not joined.
// Trystero 0.27.0's send rejects with an Error whose message names the peer; async_send completes
// with error::not_connected, since an operation that fails says so with its error code. Its twin is
// test/oracle/twins/diff_send_to_absent_peer.mjs. Both join over the relay whose URL is in the
// environment, TRYSTERO_RELAY_URL, which their lanes start on 127.0.0.1.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace trystero = webcpp::trystero;

int main() {
    const char* const relay = std::getenv("TRYSTERO_RELAY_URL");
    if (relay == nullptr) {
        std::cerr << "TRYSTERO_RELAY_URL names no relay\n";
        return 1;
    }
    boost::asio::io_context io;
    trystero::platform::backend backend(io.get_executor());
    trystero::app app(io.get_executor(), backend,
                      {.app_id = "webcpp-trystero-differences", .relay_urls = {relay}});
    auto joined = app.join("lobby");
    if (!joined) {
        std::cerr << "not joined: " << joined.error().message() << '\n';
        return 1;
    }
    trystero::room room = std::move(*joined);
    // tag::example[]
    auto greeting = room.make_action<std::string>("greeting");
    greeting.async_send("hello", {.targets = {"a-peer-that-has-not-joined"}},
                        [&room](boost::system::error_code ec) {
                            std::cout << "the send to a peer that has not joined: "
                                      << (ec ? "failed: " + ec.message() : "succeeded") << '\n';
                            room.async_leave([](boost::system::error_code) {});
                        });
    // end::example[]
    io.run();  // lint-run: the room's announcement and farewell, over the relay
}
