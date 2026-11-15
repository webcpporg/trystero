// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the platform backend's relay WebSockets to relays it cannot reach, on both targets: a URL
// the session does not take (another scheme, http and https among them, a fragment, no URL, a
// relative one) makes no connection and reports a close, as the backend interface says of a relay
// it cannot connect to; one it takes, of a port on 127.0.0.1 or ::1 where nothing listens, is
// attempted and fails to connect, natively with Boost.Beast's TCP connect and on emscripten with
// node's WebSocket, whose close comes before any open, and reports a close too. Each reports
// exactly one, after the call that made it, never inside it, and nothing is thrown; a send before
// the connection opened changes nothing, and so does a close of a refused one. On emscripten the
// test counts the WebSockets made: one for each URL taken, none for a refused one. Before the fix,
// a page's `new WebSocket` threw a SyntaxError through the room's constructor; and the test closed
// every socket in the call that made it, so that no URL was attempted, no WebSocket made in a page
// and no connection natively.
//
// main makes the sockets from a handler of the io_context, as a room does, and runs the io_context
// under a work guard, which the verdict releases once every URL has reported its close and a
// little more time has shown no second one; a step that does not come within 10 s fails by name.

#include <webcpp/trystero/platform/backend.hpp>

#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/post.hpp>
#include <webcpp/asio/steady_timer.hpp>

#ifdef WEBCPP_TARGET_EMSCRIPTEN
#include <emscripten/em_js.h>
#include <emscripten/val.h>
#endif
#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <webcpp/test/exit_status.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string_view>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using namespace std::chrono_literals;

#ifdef WEBCPP_TARGET_EMSCRIPTEN
// Counts the WebSockets the page makes, on globalThis, each still node's own.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, count_websockets, (), {
    globalThis.madeWebSockets = 0;
    const Page = globalThis.WebSocket;
    globalThis.WebSocket = class extends Page {
        constructor(...values) {
            ++globalThis.madeWebSockets;
            super(...values);
        }
    };
});
// clang-format on
#endif

namespace {

/** A relay's URL, and whether the session takes it and attempts a connection. */
struct relay_url {
    std::string_view url;
    bool attempted;
};

/**
 * URLs the session refuses (another scheme, a fragment, no URL, a relative one, and http and https,
 * which a page's WebSocket would take as ws and wss), and ones it takes, to a port where nothing
 * listens.
 */
constexpr std::array<relay_url, 8> urls{{
    {.url = "ftp://relay.invalid", .attempted = false},
    {.url = "wss://relay.invalid/#fragment", .attempted = false},
    {.url = "not a url", .attempted = false},
    {.url = "/relative", .attempted = false},
    {.url = "http://127.0.0.1:9", .attempted = false},
    {.url = "https://127.0.0.1:9", .attempted = false},
    {.url = "ws://127.0.0.1:9", .attempted = true},
    {.url = "ws://[::1]:9", .attempted = true},
}};

struct state {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    trystero::platform::backend backend{io.get_executor()};
    asio::steady_timer settle{io};
    std::array<std::shared_ptr<trystero::backend::websocket>, urls.size()> sockets;
    std::array<std::size_t, urls.size()> closes{};
    std::size_t total = 0;
    bool judged = false;
};

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
            BOOST_TEST_EQ(s.closes.at(j), 1U);
        }
#ifdef WEBCPP_TARGET_EMSCRIPTEN
        BOOST_TEST_EQ(emscripten::val::global("madeWebSockets").as<std::uint32_t>(), 2U);
#endif
        s.sockets = {};  // destroyed after the close: nothing more is reported
        s.judged = true;
        s.guard.reset();
    });
}

/**
 * Makes a WebSocket of each URL and sends on it; closes one the session refuses, and leaves one it
 * attempts to fail on its own.
 */
void connect_all(state& s) {
    for (std::size_t i = 0; i < urls.size(); ++i) {
        s.sockets.at(i) = s.backend.connect(urls.at(i).url,
                                            {
                                                .open = [] { BOOST_ERROR("a URL opened"); },
                                                .message = {},
                                                .close = [&s, i] { closed(s, i); },
                                            },
                                            std::pmr::get_default_resource());
        s.sockets.at(i)->send("ignored");
        if (!urls.at(i).attempted) {
            s.sockets.at(i)->close();
        }
        BOOST_TEST_EQ(s.closes.at(i), 0U);  // not inside the call
    }
}

}  // namespace

int main() {
#ifdef WEBCPP_TARGET_EMSCRIPTEN
    count_websockets();
#endif
    state s;
    asio::post(s.io, [&s] { connect_all(s); });
    s.io.run_for(10s);  // lint-run: the test's event loop, held by its work guard until the verdict
    if (!s.judged) {
        BOOST_ERROR("not every URL reported a close");
    }
    return webcpp::test::exit_status(boost::report_errors());
}
