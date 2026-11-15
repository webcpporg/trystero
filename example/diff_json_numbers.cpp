// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences-behaviour): a JSON payload's numbers that
// a double cannot hold. Trystero JS sends a JSON payload as JSON.stringify writes a JavaScript
// value, whose numbers are doubles: an integer past 2^53 and a decimal of more digits than a
// double keeps are sent as the nearest double, and a number past a double's range, Infinity, as
// null. A webcpp::document::document holds each number exactly, and a room sends it as the
// document writes it. This program's room sends the payload a JSON text makes, and another peer of
// the same program prints what it receives. Two apps of one program meet in a room, as two pages
// would. Its twin is test/oracle/twins/diff_json_numbers.mjs. Both meet over the relay whose URL
// is in the environment, TRYSTERO_RELAY_URL, which their lanes start on 127.0.0.1.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/parse.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace trystero = webcpp::trystero;
namespace wd = webcpp::document;

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
    // The numbers of the payload's JSON text, none of which a double holds: an integer past 2^53,
    // a decimal of 34 digits, and a number past a double's range.
    constexpr std::array<std::string_view, 3> numbers{
        "9007199254740993",
        "0.1000000000000000000000000000000001",
        "1e999",
    };
    std::string text = "[";
    for (const std::string_view number : numbers) {
        text += text.size() > 1 ? "," : "";
        text += number;
    }
    text += ']';
    // This room sends the payload its JSON text makes, once it meets the other peer.
    auto payload = room.make_action<wd::document>("payload");
    room.on_peer_join([&](std::string_view) {
        wd::result<wd::document> value = wd::parse(text);
        if (!value) {
            std::cerr << "not parsed: " << value.error().message() << '\n';
            leave();
            return;
        }
        payload.async_send(std::move(*value), [](boost::system::error_code) {});
    });
    // What the other peer receives, number by number.
    auto received = other->make_action<wd::document>("payload");
    received.on_message([&](const wd::document& value, const trystero::message_info&) {
        for (std::size_t i = 0; i < numbers.size(); ++i) {
            std::cout << numbers[i] << " arrives as " << value[i].stringify() << '\n';
        }
        leave();
    });
    // end::example[]
    io.run();  // lint-run: the program's network I/O; libdatachannel's threads post to it
}
