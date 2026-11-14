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
// main makes the sockets from a handler of the io_context, as a room does, and runs the io_context
// under a work guard, which the verdict releases once every URL has reported its close and a
// little more time has shown no second one; a step that does not come within 10 s fails by name.

#include <webcpp/trystero/browser/backend.hpp>

#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/post.hpp>
#include <webcpp/asio/steady_timer.hpp>

#include <emscripten/em_asm.h>
#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <webcpp/test/exit_status.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <string_view>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using namespace std::chrono_literals;

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
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    trystero::browser::backend backend{io.get_executor()};
    asio::steady_timer settle{io};
    std::array<std::shared_ptr<trystero::backend::websocket>, urls.size()> sockets;
    std::array<int, urls.size()> closes{};
    std::size_t total = 0;
    bool judged = false;
};

/** Whether node's WebSocket makes a socket of `url`, as the page's own code would see it. */
bool node_accepts(std::string_view url) {
    // clang-format off
    return EM_ASM_INT(({
        try {
            new WebSocket(UTF8ToString($0, $1)).close();
            return 1;
        } catch (_) {
            return 0;
        }
    }), url.data(), url.size()) != 0;
    // clang-format on
}

/** Judges once every URL has reported its close, after 100 ms more, in which a second one of any
 * would come. */
void closed(state& s, std::size_t i) {
    ++s.closes.at(i);
    if (++s.total != urls.size()) {
        return;
    }
    s.settle.expires_after(100ms);
    s.settle.async_wait([&s](boost::system::error_code) {
        for (std::size_t j = 0; j < urls.size(); ++j) {
            BOOST_TEST_EQ(s.closes.at(j), 1);
        }
        s.sockets = {};  // destroyed after the close: nothing more is reported
        s.judged = true;
        s.guard.reset();
    });
}

/** Makes a WebSocket of each URL, then sends on it and closes it. */
void connect_all(state& s) {
    for (std::size_t i = 0; i < urls.size(); ++i) {
        std::cout << urls.at(i) << (node_accepts(urls.at(i)) ? ": a socket\n" : ": refused\n");
        s.sockets.at(i) = s.backend.connect(urls.at(i),
                                            {
                                                .open = [] { BOOST_ERROR("a URL opened"); },
                                                .message = {},
                                                .close = [&s, i] { closed(s, i); },
                                            },
                                            std::pmr::get_default_resource());
        s.sockets.at(i)->send("ignored");
        s.sockets.at(i)->close();
        BOOST_TEST_EQ(s.closes.at(i), 0);  // not inside the call
    }
}

}  // namespace

int main() {
    state s;
    asio::post(s.io, [&s] { connect_all(s); });
    s.io.run_for(10s);  // lint-run: the test's event loop, held by its work guard until the verdict
    if (!s.judged) {
        BOOST_ERROR("not every URL reported a close");
    }
    return webcpp::test::exit_status(boost::report_errors());
}
