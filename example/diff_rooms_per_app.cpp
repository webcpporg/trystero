// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences-behaviour): how many rooms an app holds.
// Trystero joins every room a page asks for, and a peer that advertises a 65th over one
// connection is dropped by the other side; app::join refuses the 65th with
// error::too_many_rooms, counting a room in its farewell, since the 65th would cost every
// connection of the app. Its twin is test/oracle/twins/diff_rooms_per_app.mjs. Both join over the
// relay whose URL is in the environment, TRYSTERO_RELAY_URL, which their lanes start on 127.0.0.1.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/native/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace trystero = webcpp::trystero;

int main() {
    const char* const relay = std::getenv("TRYSTERO_RELAY_URL");
    if (relay == nullptr) {
        std::cerr << "TRYSTERO_RELAY_URL names no relay\n";
        return 1;
    }
    boost::asio::io_context io;
    trystero::native::backend backend(io.get_executor());
    trystero::app app(io.get_executor(), backend,
                      {.app_id = "webcpp-trystero-differences", .relay_urls = {relay}});
    std::vector<trystero::room> rooms;
    // What a join gives, a room kept until the end or the error that refused it.
    const auto join = [&](const std::string& room_id) {
        auto joined = app.join(room_id);
        if (!joined) {
            return joined.error().message();
        }
        rooms.push_back(std::move(*joined));
        return std::string("a room");
    };
    const auto leave_all = [&] {
        for (auto& room : rooms) {
            room.async_leave([](boost::system::error_code) {});
        }
    };
    // tag::example[]
    for (std::size_t n = 1; n <= 64; ++n) {
        join("room-" + std::to_string(n));
    }
    std::cout << "joins 1 to 64: " << rooms.size() << " rooms\n";
    std::cout << "join 65: " << join("room-65") << '\n';
    rooms.front().async_leave([&](boost::system::error_code) {
        std::cout << "join 67, once room 1 has left: " << join("room-67") << '\n';
        leave_all();
    });
    std::cout << "join 66, while room 1 is leaving: " << join("room-66") << '\n';
    // end::example[]
    io.run();  // lint-run: the rooms' announcements and farewells, over the relay
}
