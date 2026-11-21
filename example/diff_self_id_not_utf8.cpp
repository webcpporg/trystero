// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences-behaviour): a self id that is not UTF-8.
// An app given one sets it aside for 20 random characters and reports error::invalid_configuration,
// since a signal writes the id as a JSON string, which carries UTF-8 alone. Trystero JS draws its
// selfId when its module loads and takes none from a program, so it has no such id, and this
// program no twin (test/oracle/twins/without-twin.txt). The app joins no room, so it needs no
// relay.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/platform/backend.hpp>

#include <boost/asio/io_context.hpp>

#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace trystero = webcpp::trystero;

int main() {
    boost::asio::io_context io;
    trystero::platform::backend backend(io.get_executor());
    // tag::example[]
    const std::string given = "caf\xE9";  // ISO 8859-1's "café": its last byte is no UTF-8
    trystero::app_config config;
    config.app_id = "webcpp-trystero-differences";
    config.self_id = given;
    trystero::app app(io.get_executor(), backend, std::move(config));
    app.on_error([](const trystero::room_error& e) {
        std::cout << "reported: " << e.code.message() << ": " << e.message << '\n';
    });
    std::cout << "the self id given: " << given.size() << " bytes, not UTF-8\n"
              << "the app's self id: "
              << (app.self_id() == given
                      ? std::string("the one given")
                      : std::to_string(app.self_id().size()) + " random characters")
              << '\n';
    // end::example[]
    io.run();  // lint-run: delivers the report the app's constructor posted
}
