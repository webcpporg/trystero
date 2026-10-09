// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the browser backend's WebSockets, under node, whose WebSocket refuses URLs as a browser's
// does: what `new WebSocket` makes of each URL is the browser's to decide. A URL it throws for
// makes no socket and reports a close, as the backend interface says of a relay it cannot connect
// to; one it accepts makes a socket, whose failed connection to a port where nothing listens, or
// its close, reports a close too. Each reports exactly one, after the call that made it, never
// inside it, and nothing is thrown. Before the fix, `new WebSocket` threw a SyntaxError through the
// room's constructor.
//
// main sets the test up and returns with node's exit code at 1; the verdict comes when every URL
// has reported its close, or from a deadline that fails by name, and node exits once nothing is
// pending.

#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript exports

#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <array>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <string_view>

namespace trystero = webcpp::trystero;
using emscripten::val;

namespace {

/**
 * URLs node's WebSocket throws for (another scheme, a fragment, no URL, a relative one without a
 * page to resolve it against) and ones the WHATWG constructor accepts, mapping http and https to
 * ws and wss, to a port where nothing listens.
 */
constexpr std::array<std::string_view, 6> urls{
    "ftp://relay.invalid", "wss://relay.invalid/#fragment", "not a url", "/relative",
    "http://127.0.0.1:9",  "https://127.0.0.1:9",
};

struct state {
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    trystero::browser::backend backend{loop};
    std::array<std::shared_ptr<trystero::backend::websocket>, urls.size()> sockets;
    std::array<int, urls.size()> closes{};
    std::size_t total = 0;
    val deadline = val::undefined();
};

state& the_state() {
    static state s;
    return s;
}

void set_exit_code(int code) {
    val::global("process").set("exitCode", val(code));
}

/** Ends the test once every URL has reported its close. */
void closed(std::size_t i) {
    auto& s = the_state();
    ++s.closes.at(i);
    if (++s.total < urls.size()) {
        return;
    }
    for (std::size_t j = 0; j < urls.size(); ++j) {
        BOOST_TEST_EQ(s.closes.at(j), 1);
    }
    val::global("clearTimeout")(s.deadline);
    s.sockets = {};  // destroyed after the close: nothing more is reported
    set_exit_code(boost::report_errors());
}

/** Makes a WebSocket of each URL, then sends on it and closes it, from an event of the page, as a
 * room does from its handlers. */
void connect_all(int /*slot*/, const val& /*event*/) {
    auto& s = the_state();
    for (std::size_t i = 0; i < urls.size(); ++i) {
        // What node makes of the URL, as the page's own code would see it.
        const auto probe =
            trystero::browser::detail::js_new(val::global("WebSocket"), s.loop.text(urls.at(i)));
        if (probe.ok) {
            probe.value.call<void>("close");
        }
        std::cout << urls.at(i) << (probe.ok ? ": a socket\n" : ": refused\n");

        s.sockets.at(i) = s.backend.connect(urls.at(i),
                                            {
                                                .open = [] { BOOST_ERROR("a URL opened"); },
                                                .message = {},
                                                .close = [i] { closed(i); },
                                            },
                                            std::pmr::get_default_resource());
        s.sockets.at(i)->send("ignored");
        s.sockets.at(i)->close();
        BOOST_TEST_EQ(s.closes.at(i), 0);  // not inside the call
    }
}

}  // namespace

int main() {
    auto& s = the_state();
    const int start = s.loop.add(connect_all);
    val::global("setTimeout")(s.loop.handler(start, 0), val(0), val::undefined());
    const int id = s.loop.add([](int /*slot*/, const val& /*event*/) {
        BOOST_ERROR("not every URL reported a close");
        set_exit_code(boost::report_errors());
    });
    s.deadline = val::global("setTimeout")(s.loop.handler(id, 0), val(10'000), val::undefined());
    set_exit_code(1);  // until the verdict
    return 0;
}
