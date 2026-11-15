// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests what the platform backend's relay WebSocket takes of what a relay sends in a page, under
// node, over a stand-in for the page's WebSocket whose events are a page's:
//
// - A text longer than protocol::max_relay_message_bytes that the relay sends in the same task of
//   the page as its open, as a relay that answers the handshake and writes at once can have the
//   page deliver them, closes the connection, and the room never sees it: the stand-in fires its
//   open and then a text of 1 MiB and one byte from one callback. Before the fix the session set
//   its bound only once the handshake had completed on the executor, after both events had entered
//   webcpp::beast's page stream, so the text was checked against beast's default of 16 MiB, copied
//   into WebAssembly and handed to the room whole.
// - A binary message is dropped, as the page's own backend that this replaced dropped what was not
//   a string, and the text after it reaches the room: the stand-in sends an ArrayBuffer, then a
//   text. Before the fix the room was handed the binary message's bytes as a text.
//
// Each socket reports one open and one close. main runs the io_context under a work guard, which
// the verdict releases once both closes are reported; a step that does not come within 10 s fails
// by name.

#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/protocol/relays.hpp>

#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>

#include <emscripten/em_js.h>
#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using emscripten::val;
using namespace std::chrono_literals;

// The stand-in: on the next turn it opens and, from the same callback, sends what its URL's path
// names: /oversized, a text of $0 bytes; /binary, an ArrayBuffer of six bytes and then a text. A
// close() closes it on the next turn, once. Its constructions are counted on globalThis.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, install_sending_websocket, (double oversized), {
    globalThis.sendingSockets = 0;
    globalThis.WebSocket = class {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;
        constructor(url) {
            ++globalThis.sendingSockets;
            this.url = url;
            this.readyState = 0;
            this.bufferedAmount = 0;
            setTimeout(() => {
                this.readyState = 1;
                if (this.onopen) this.onopen({type: "open"});
                if (url.endsWith("/oversized")) {
                    if (this.onmessage) this.onmessage({type: "message", data: "o".repeat(oversized)});
                } else if (url.endsWith("/binary")) {
                    const bytes = new TextEncoder().encode("binary");
                    if (this.onmessage) this.onmessage({type: "message", data: bytes.buffer});
                    if (this.onmessage) this.onmessage({type: "message", data: "after"});
                }
            }, 0);
        }
        send(text) {}
        close() {
            if (this.readyState >= 2) return;
            this.readyState = 2;
            setTimeout(() => {
                this.readyState = 3;
                if (this.onclose) this.onclose({type: "close", code: 1006, reason: ""});
            }, 0);
        }
    };
});
// clang-format on

namespace {

/** What one socket reported. */
struct reports {
    std::size_t opens = 0;
    std::size_t closes = 0;
    std::vector<std::string> messages;
};

struct state {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    trystero::platform::backend backend{io.get_executor()};
    std::shared_ptr<trystero::backend::websocket> oversized;
    std::shared_ptr<trystero::backend::websocket> binary;
    reports oversized_reports;
    reports binary_reports;
    bool judged = false;
};

void verdict(state& s) {
    if (s.oversized_reports.closes == 0 || s.binary_reports.closes == 0) {
        return;
    }
    BOOST_TEST_EQ(s.oversized_reports.opens, 1U);
    BOOST_TEST_EQ(s.oversized_reports.closes, 1U);
    BOOST_TEST_EQ(s.oversized_reports.messages.size(), 0U);
    for (const auto& text : s.oversized_reports.messages) {
        BOOST_ERROR("the oversized text reached the room");
        BOOST_TEST_EQ(text.size(), trystero::protocol::max_relay_message_bytes + 1);
    }
    BOOST_TEST_EQ(s.binary_reports.opens, 1U);
    BOOST_TEST_EQ(s.binary_reports.closes, 1U);
    if (BOOST_TEST_EQ(s.binary_reports.messages.size(), 1U)) {
        BOOST_TEST_EQ(s.binary_reports.messages.front(), "after");
    }
    BOOST_TEST_EQ(val::global("sendingSockets").as<std::uint32_t>(), 2U);
    s.oversized.reset();
    s.binary.reset();
    s.judged = true;
    s.guard.reset();
}

/** The handlers of a socket that records into `into`, and runs `on_message` after each message. */
trystero::backend::websocket::handlers recording(state& s, reports& into,
                                                 void (*on_message)(state&)) {
    return {
        .open = [&into] { ++into.opens; },
        .message =
            [&s, &into, on_message](std::string_view text) {
                into.messages.emplace_back(text);
                on_message(s);
            },
        .close =
            [&s, &into] {
                ++into.closes;
                verdict(s);
            },
    };
}

/** A text reached the binary socket, the one after its binary message: it is closed. */
void close_binary(state& s) {
    s.binary->close();
}

/** A text reached the oversized socket, which none should: it is closed, so that the verdict runs.
 */
void close_oversized(state& s) {
    s.oversized->close();
}

}  // namespace

int main() {
    install_sending_websocket(static_cast<double>(trystero::protocol::max_relay_message_bytes + 1));
    state s;
    s.oversized = s.backend.connect("wss://relay.test/oversized",
                                    recording(s, s.oversized_reports, close_oversized),
                                    std::pmr::get_default_resource());
    s.binary =
        s.backend.connect("wss://relay.test/binary", recording(s, s.binary_reports, close_binary),
                          std::pmr::get_default_resource());
    s.io.run_for(10s);  // lint-run: the test's event loop, held by its work guard until the verdict
    if (!s.judged) {
        BOOST_ERROR("a socket did not report its close");
    }
    return webcpp::test::exit_status(boost::report_errors());
}
