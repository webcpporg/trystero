// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The native quick start of the documentation (doc: #trystero-quick-start), which includes the
// tagged regions below: the browser quick start's room, as a native program. Trystero JS and
// browser peers in the same room are its peers.

// tag::program[]
#include <webcpp/trystero/native/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>

#include <iostream>
#include <string>

namespace trystero = webcpp::trystero;

int main() {
    boost::asio::io_context io;
    trystero::native::backend backend(io.get_executor());
    trystero::room room(io.get_executor(), backend, {.app_id = "webcpp-trystero-quick-start"},
                        "lobby");
    auto chat = room.make_action<std::string>("chat");

    room.on_peer_join([&](std::string_view peer) {
        trystero::send_options to_peer{.targets = {trystero::peer_id(peer)}, .metadata = {}};
        chat.async_send("hello from a native peer", std::move(to_peer),
                        [](boost::system::error_code) {});
    });
    chat.on_message([](const std::string& text, const trystero::message_info& info) {
        // flushed: into a pipe too, since the program runs until it is stopped
        std::cout << info.from << ": " << text << '\n' << std::flush;
    });

    io.run();  // lint-run: the program's network I/O; libdatachannel's threads post to it
}

// end::program[]
