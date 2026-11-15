// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the platform backend's bound on what a relay's WebSocket holds unsent in a page, under
// node, over a stand-in for the page's WebSocket: a relay that stops reading, whose bufferedAmount
// only grows, and whose close never completes. The room sends texts of 64 KiB on it, one a turn of
// the io_context, so that each write reaches the page before the next and the session's own queue
// stays empty, 6.4 MiB in all: past protocol::max_queued_relay_bytes of bufferedAmount,
// webcpp::beast's send limit fails the write, and the socket is dropped and reports one close,
// never inside a send, holding no more than the bound. Without a bound, the page's own backend
// that this replaced held all 6.4 MiB and no close came.
//
// main runs the io_context under a work guard, which the verdict releases once the texts are sent
// and the close is reported; a step that does not come within 10 s fails by name.

#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/protocol/relays.hpp>

#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/post.hpp>

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
// Its events are a page's: each names its type.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, install_stalled_websocket, (), {
    globalThis.stalledHeld = 0;
    globalThis.stalledCloses = 0;
    globalThis.WebSocket = class {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;
        constructor(url) {
            this.url = url;
            this.readyState = 0;
            this.bufferedAmount = 0;
            setTimeout(() => {
                this.readyState = 1;
                if (this.onopen) this.onopen({type: "open"});
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
    trystero::platform::backend backend{io.get_executor()};
    std::shared_ptr<trystero::backend::websocket> socket;
    const std::string text = std::string(std::size_t{64} * 1024, 't');
    int sending = 0;
    int closes = 0;
    bool in_send = false;
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
    BOOST_TEST_GE(val::global("stalledCloses").as<int>(), 1);
    s.socket.reset();
    s.judged = true;
    s.guard.reset();
}

/**
 * Sends the next text, and posts the one after it, so that one text is sent a turn.
 *
 * A loop of posted calls, not a recursion: each runs from the io_context once the one that posted
 * it has returned, so the stack never grows; hence its exemption, and its lambda's, from
 * misc-no-recursion, which sees the cycle through Asio's post.
 */
void send_next(state& s) {  // NOLINT(misc-no-recursion)
    if (s.sending == texts) {
        s.sent = true;
        verdict(s);
        return;
    }
    ++s.sending;
    s.in_send = true;
    s.socket->send(s.text);
    s.in_send = false;
    asio::post(s.io, [&s] { send_next(s); });  // NOLINT(misc-no-recursion)
}

}  // namespace

int main() {
    install_stalled_websocket();
    state s;
    s.socket = s.backend.connect("wss://relay.test",
                                 {
                                     .open = [&s] { send_next(s); },
                                     .message = {},
                                     .close =
                                         [&s] {
                                             BOOST_TEST(!s.in_send);  // not inside a send
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
