// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences-behaviour): joining a room the app has
// joined already. Trystero's joinRoom returns the room it joined before; app::join refuses with
// error::already_joined, since a room is a handle whose destruction leaves. Its twin is
// test/oracle/twins/diff_already_joined.mjs. Both join over the relay whose URL is in the
// environment, TRYSTERO_RELAY_URL, which their lanes start on 127.0.0.1.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

namespace trystero = webcpp::trystero;

int main() {
    const char* const relay = std::getenv("TRYSTERO_RELAY_URL");
    if (relay == nullptr) {
        std::cerr << "TRYSTERO_RELAY_URL names no relay\n";
        return 1;
    }
    boost::asio::io_context io;
    trystero::platform::backend backend(io.get_executor());
    // tag::example[]
    trystero::app app(io.get_executor(), backend,
                      {.app_id = "webcpp-trystero-differences", .relay_urls = {relay}});
    auto first = app.join("lobby");
    const auto second = app.join("lobby");
    std::cout << "the first join: " << (first ? std::string("a room") : first.error().message())
              << '\n'
              << "the second join: "
              << (second ? std::string("another room") : second.error().message()) << '\n';
    // end::example[]
    if (first) {
        first->async_leave([](boost::system::error_code) {});
    }
    io.run();  // lint-run: the room's farewell, over the relay
}
