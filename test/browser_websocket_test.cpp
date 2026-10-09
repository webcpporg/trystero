// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the browser backend's WebSockets, under node, whose WebSocket refuses the URLs a browser's
// does: a URL the page's WebSocket cannot be made with reports a close, as the backend interface
// says of a relay it cannot connect to. It reports it later, never inside the call that made the
// WebSocket, and nothing is thrown. Before the fix, `new WebSocket` threw a SyntaxError through the
// room's constructor.
//
// main sets the test up and returns with node's exit code at 1; the verdict comes from a timer,
// which sets the exit code to `boost::report_errors()`, and node exits once nothing is pending.

#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript export

#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <array>
#include <memory>
#include <memory_resource>
#include <string_view>

namespace trystero = webcpp::trystero;
using emscripten::val;

namespace {

/** URLs no WebSocket can be made to: another scheme, a fragment, and no URL at all. */
constexpr std::array<std::string_view, 3> refused{
    "ftp://relay.invalid",
    "wss://relay.invalid/#fragment",
    "not a url",
};

struct state {
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    trystero::browser::backend backend{loop};
    std::array<std::shared_ptr<trystero::backend::websocket>, refused.size()> sockets;
    std::array<int, refused.size()> closes{};
};

state& the_state() {
    static state s;
    return s;
}

void set_exit_code(int code) {
    val::global("process").set("exitCode", val(code));
}

/** Calls `setTimeout` with a function that runs `f` through the loop, as the page's events do. */
void after(int milliseconds, trystero::browser::event_loop::callback f) {
    auto& loop = the_state().loop;
    const int id = loop.add(std::move(f));
    // The handler takes one argument, which setTimeout passes when given one.
    val::global("setTimeout")(loop.handler(id, 0), val(milliseconds), val::undefined());
}

}  // namespace

int main() {
    auto& s = the_state();
    for (std::size_t i = 0; i < refused.size(); ++i) {
        s.sockets.at(i) = s.backend.connect(refused.at(i),
                                            {
                                                .open = [] { BOOST_ERROR("a refused URL opened"); },
                                                .message = {},
                                                .close = [i] { ++the_state().closes.at(i); },
                                            },
                                            std::pmr::get_default_resource());
        // Sending on it, and closing it, do nothing either.
        s.sockets.at(i)->send("ignored");
        s.sockets.at(i)->close();
        BOOST_TEST_EQ(s.closes.at(i), 0);  // not inside the call
    }

    after(200, [](int /*slot*/, const val& /*event*/) {
        for (std::size_t i = 0; i < refused.size(); ++i) {
            BOOST_TEST_EQ(the_state().closes.at(i), 1);
        }
        the_state().sockets = {};  // destroyed after the close: nothing more is reported
        set_exit_code(boost::report_errors());
    });
    set_exit_code(1);  // until the verdict
    return 0;
}
