// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests that a relay's WebSocket the platform backend dropped at protocol::max_queued_relay_bytes
// in a page runs none of its handlers again, under node, over a stand-in for the page's WebSocket
// that, as a browser does, has a message event queued for every text sent (a relay message already
// in the page's event loop) and, after close(), a close event, each dispatched to whatever its
// handler property holds when it runs. The room sends 100 texts of 64 KiB in one call: the first
// reaches the page and the rest wait in the session's queue, which drops the socket past the bound;
// the message the page queued for the first finds no handler. In the page's own backend that this
// replaced, before its socket was detached, the 64 messages queued before the drop reached the room
// after its close was reported. A second socket sends one text larger than the bound alone: it is
// dropped at once, and reports one close.
//
// main runs the io_context under a work guard. Once both closes are reported, the verdict waits
// for a promise that a JavaScript timer settles, behind every event the stand-in had queued by
// then, and releases the guard; a step that does not come within 10 s fails by name.

#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/protocol/relays.hpp>

#include <webcpp/asio/browser/async_await.hpp>
#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>

#include <emscripten/em_js.h>
#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <webcpp/test/browser_values.hpp>  // once per program: its EM_JS
#include <webcpp/test/exit_status.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using emscripten::val;
using namespace std::chrono_literals;

// The stand-in: it opens on the next turn, adds each text's UTF-8 length to bufferedAmount and
// never drains it, queues a message event for each text sent and a close event after close(). Its
// events are a page's: each names its type, and a close its code and reason.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, install_late_websocket, (), {
    globalThis.lateCloses = 0;
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
            setTimeout(() => {
                if (this.onmessage) this.onmessage({type: "message", data: "late"});
            }, 0);
        }
        close() {
            this.readyState = 2;
            ++globalThis.lateCloses;
            setTimeout(() => {
                this.readyState = 3;
                if (this.onclose) this.onclose({type: "close", code: 1006, reason: ""});
            }, 0);
        }
    };
});
// clang-format on

namespace {

struct state {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    trystero::platform::backend backend{io.get_executor()};
    std::shared_ptr<trystero::backend::websocket> stalled;
    std::shared_ptr<trystero::backend::websocket> big;
    std::size_t stalled_closes = 0;
    std::size_t messages_after_close = 0;
    std::size_t big_closes = 0;
    bool judged = false;
};

void judge(state& s) {
    BOOST_TEST_EQ(s.stalled_closes, 1U);
    BOOST_TEST_EQ(s.messages_after_close, 0U);
    BOOST_TEST_EQ(s.big_closes, 1U);
    BOOST_TEST_EQ(val::global("lateCloses").as<std::uint32_t>(), 2U);
    s.stalled.reset();
    s.big.reset();
    s.judged = true;
    s.guard.reset();
}

/** Once both sockets reported their close, judges behind every event the stand-in queued. */
void closed(state& s) {
    if (s.stalled_closes == 0 || s.big_closes == 0) {
        return;
    }
    asio::browser::async_await(s.io.get_executor(),
                               val::take_ownership(test_promise_after(0, 0, false)),
                               [&s](boost::system::error_code ec, const val& /*value*/) {
                                   BOOST_TEST(!ec);
                                   judge(s);
                               });
}

}  // namespace

int main() {
    install_late_websocket();
    state s;
    s.stalled = s.backend.connect("wss://relay.test",
                                  {
                                      .open =
                                          [&s] {
                                              const std::string text(std::size_t{64} * 1024, 't');
                                              for (std::size_t i = 0; i < 100; ++i) {
                                                  s.stalled->send(text);
                                              }
                                          },
                                      .message =
                                          [&s](std::string_view /*text*/) {
                                              if (s.stalled_closes != 0) {
                                                  ++s.messages_after_close;
                                              }
                                          },
                                      .close =
                                          [&s] {
                                              ++s.stalled_closes;
                                              closed(s);
                                          },
                                  },
                                  std::pmr::get_default_resource());
    s.big = s.backend.connect(
        "wss://relay.test",
        {
            .open =
                [&s] {
                    s.big->send(std::string(trystero::protocol::max_queued_relay_bytes + 1, 'b'));
                },
            .message = {},
            .close =
                [&s] {
                    ++s.big_closes;
                    closed(s);
                },
        },
        std::pmr::get_default_resource());
    s.io.run_for(10s);  // lint-run: the test's event loop, held by its work guard until the verdict
    if (!s.judged) {
        BOOST_ERROR("a dropped socket did not report its close");
    }
    return webcpp::test::exit_status(boost::report_errors());
}
