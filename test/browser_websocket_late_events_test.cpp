// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests that a relay's WebSocket the browser backend dropped at protocol::max_queued_relay_bytes
// runs none of its handlers again, under node, over a stand-in for the page's WebSocket that, as a
// browser does, has a message event queued for every text sent (a relay message already in the
// page's event loop) and, after close(), a close event, each dispatched to whatever its handler
// property holds when it runs. Without the detach, the 64 messages queued before the drop reached
// the room after its close was reported. A second socket sends one text larger than the bound
// alone: it is dropped at once, and reports one close.
//
// main sets the test up and returns with node's exit code at 1. The verdict runs on the timer
// queue once both closes are reported, behind every event the stand-in had queued by then, or a
// deadline fails by name.

#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript exports
#include <webcpp/trystero/protocol/relays.hpp>

#include <emscripten/em_js.h>
#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>

namespace trystero = webcpp::trystero;
using emscripten::val;

// The stand-in: it opens on the next turn, adds each text's UTF-8 length to bufferedAmount and
// never drains it, queues a message event for each text sent and a close event after close().
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, install_late_websocket, (), {
    globalThis.lateCloses = 0;
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
            setTimeout(() => {
                if (this.onmessage) this.onmessage({data: "late"});
            }, 0);
        }
        close() {
            this.readyState = 2;
            ++globalThis.lateCloses;
            setTimeout(() => {
                this.readyState = 3;
                if (this.onclose) this.onclose({code: 1006});
            }, 0);
        }
    };
});
// clang-format on

namespace {

struct state {
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    trystero::browser::backend backend{loop};
    std::shared_ptr<trystero::backend::websocket> stalled;
    std::shared_ptr<trystero::backend::websocket> big;
    int stalled_closes = 0;
    int messages_after_close = 0;
    int big_closes = 0;
    int verdict = 0;
    val deadline = val::undefined();
};

state& the_state() {
    static state s;
    return s;
}

void set_exit_code(int code) {
    val::global("process").set("exitCode", val(code));
}

void judge(int /*slot*/, const val& /*event*/) {
    auto& s = the_state();
    BOOST_TEST_EQ(s.stalled_closes, 1);
    BOOST_TEST_EQ(s.messages_after_close, 0);
    BOOST_TEST_EQ(s.big_closes, 1);
    BOOST_TEST_EQ(val::global("lateCloses").as<int>(), 2);
    val::global("clearTimeout")(s.deadline);
    s.stalled.reset();
    s.big.reset();
    set_exit_code(boost::report_errors());
}

/** Once both sockets reported their close, judges behind every event the stand-in queued. */
void closed() {
    const auto& s = the_state();
    if (s.stalled_closes == 0 || s.big_closes == 0) {
        return;
    }
    val::global("setTimeout")(s.loop.handler(s.verdict, 0), val(0), val::undefined());
}

}  // namespace

int main() {
    install_late_websocket();
    auto& s = the_state();
    s.verdict = s.loop.add(judge);
    s.stalled = s.backend.connect("wss://relay.test",
                                  {
                                      .open =
                                          [] {
                                              const auto& s = the_state();
                                              const std::string text(std::size_t{64} * 1024, 't');
                                              for (int i = 0; i < 100; ++i) {
                                                  s.stalled->send(text);
                                              }
                                          },
                                      .message =
                                          [](std::string_view /*text*/) {
                                              auto& s = the_state();
                                              if (s.stalled_closes != 0) {
                                                  ++s.messages_after_close;
                                              }
                                          },
                                      .close =
                                          [] {
                                              ++the_state().stalled_closes;
                                              closed();
                                          },
                                  },
                                  std::pmr::get_default_resource());
    s.big = s.backend.connect(
        "wss://relay.test",
        {
            .open =
                [] {
                    const auto& s = the_state();
                    s.big->send(std::string(trystero::protocol::max_queued_relay_bytes + 1, 'b'));
                },
            .message = {},
            .close =
                [] {
                    ++the_state().big_closes;
                    closed();
                },
        },
        std::pmr::get_default_resource());
    const int id = s.loop.add([](int /*slot*/, const val& /*event*/) {
        BOOST_ERROR("a dropped socket did not report its close");
        set_exit_code(boost::report_errors());
    });
    s.deadline = val::global("setTimeout")(s.loop.handler(id, 0), val(10'000), val::undefined());
    set_exit_code(1);  // until the verdict
    return 0;
}
