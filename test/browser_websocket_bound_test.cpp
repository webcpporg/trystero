// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the browser backend's bound on what a relay's WebSocket holds unsent, under node, over a
// stand-in for the page's WebSocket: a relay that stops reading, whose bufferedAmount only grows,
// and whose close never completes. The room sends texts of 64 KiB on it, 6.4 MiB in all; past
// protocol::max_queued_relay_bytes the socket is dropped and reports one close, after the call,
// and holds no more than the bound. Without the bound, the stand-in held all 6.4 MiB and no close
// came.
//
// main runs the io_context under a work guard, which the verdict releases once the texts are sent
// and the close is reported; a step that does not come within 10 s fails by name.

#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/protocol/relays.hpp>

#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>

#include <emscripten/em_js.h>
#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <string>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using emscripten::val;
using namespace std::chrono_literals;

// The stand-in: it opens on the next turn, adds each text's UTF-8 length to bufferedAmount and
// never drains it, and records close() without ever firing onclose. Its counts are on globalThis.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, install_stalled_websocket, (), {
    globalThis.stalledHeld = 0;
    globalThis.stalledCloses = 0;
    globalThis.WebSocket = class {
        constructor(url) {
            this.url = url;
            this.readyState = 0;
            this.bufferedAmount = 0;
            setTimeout(() => {
                this.readyState = 1;
                if (this.onopen) this.onopen({});
            }, 0);
        }
        send(text) {
            this.bufferedAmount += new TextEncoder().encode(text).length;
            globalThis.stalledHeld = Math.max(globalThis.stalledHeld, this.bufferedAmount);
        }
        close() {
            this.readyState = 2;
            ++globalThis.stalledCloses;
        }
    };
});
// clang-format on

namespace {

constexpr int texts = 100;

struct state {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    trystero::browser::backend backend{io.get_executor()};
    std::shared_ptr<trystero::backend::websocket> socket;
    int closes = 0;
    bool sent = false;
    bool judged = false;
};

std::size_t held() {
    return static_cast<std::size_t>(val::global("stalledHeld").as<double>());
}

void verdict(state& s) {
    if (!s.sent || s.closes == 0) {
        return;
    }
    BOOST_TEST_EQ(s.closes, 1);
    BOOST_TEST_LE(held(), trystero::protocol::max_queued_relay_bytes);
    BOOST_TEST_EQ(val::global("stalledCloses").as<int>(), 1);
    s.socket.reset();
    s.judged = true;
    s.guard.reset();
}

}  // namespace

int main() {
    install_stalled_websocket();
    state s;
    s.socket = s.backend.connect("wss://relay.test",
                                 {
                                     .open =
                                         [&s] {
                                             const std::string text(std::size_t{64} * 1024, 't');
                                             for (int i = 0; i < texts; ++i) {
                                                 s.socket->send(text);
                                             }
                                             BOOST_TEST_EQ(s.closes, 0);  // not inside the call
                                             s.sent = true;
                                             verdict(s);
                                         },
                                     .message = {},
                                     .close =
                                         [&s] {
                                             ++s.closes;
                                             verdict(s);
                                         },
                                 },
                                 std::pmr::get_default_resource());
    s.io.run_for(10s);  // lint-run: the test's event loop, held by its work guard until the verdict
    if (!s.judged) {
        BOOST_ERROR("the stalled relay was never dropped");
        BOOST_TEST_LE(held(), trystero::protocol::max_queued_relay_bytes);
    }
    return webcpp::test::exit_status(boost::report_errors());
}
