// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the browser backend's bound on what a relay's WebSocket holds unsent, under node, over a
// stand-in for the page's WebSocket: a relay that stops reading, whose bufferedAmount only grows,
// and whose close never completes. The room sends texts of 64 KiB on it, 6.4 MiB in all; past
// protocol::max_queued_relay_bytes the socket is dropped and reports one close, at once, and holds
// no more than the bound. Without the bound, the stand-in held all 6.4 MiB and no close came.
//
// main sets the test up and returns with node's exit code at 1; the verdict comes once the texts
// are sent and the close is reported, or from a deadline that fails by name.

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

namespace trystero = webcpp::trystero;
using emscripten::val;

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
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    trystero::browser::backend backend{loop};
    std::shared_ptr<trystero::backend::websocket> socket;
    int closes = 0;
    bool sent = false;
    val deadline = val::undefined();
};

state& the_state() {
    static state s;
    return s;
}

void set_exit_code(int code) {
    val::global("process").set("exitCode", val(code));
}

void verdict() {
    auto& s = the_state();
    if (!s.sent || s.closes == 0) {
        return;
    }
    BOOST_TEST_EQ(s.closes, 1);
    const auto held = static_cast<std::size_t>(val::global("stalledHeld").as<double>());
    BOOST_TEST_LE(held, trystero::protocol::max_queued_relay_bytes);
    BOOST_TEST_EQ(val::global("stalledCloses").as<int>(), 1);
    val::global("clearTimeout")(s.deadline);
    s.socket.reset();
    set_exit_code(boost::report_errors());
}

}  // namespace

int main() {
    install_stalled_websocket();
    auto& s = the_state();
    s.socket = s.backend.connect("wss://relay.test",
                                 {
                                     .open =
                                         [] {
                                             auto& s = the_state();
                                             const std::string text(std::size_t{64} * 1024, 't');
                                             for (int i = 0; i < texts; ++i) {
                                                 s.socket->send(text);
                                             }
                                             s.sent = true;
                                             verdict();
                                         },
                                     .message = {},
                                     .close =
                                         [] {
                                             ++the_state().closes;
                                             verdict();
                                         },
                                 },
                                 std::pmr::get_default_resource());
    const int id = s.loop.add([](int /*slot*/, const val& /*event*/) {
        BOOST_ERROR("the stalled relay was never dropped");
        BOOST_TEST_LE(static_cast<std::size_t>(val::global("stalledHeld").as<double>()),
                      trystero::protocol::max_queued_relay_bytes);
        set_exit_code(boost::report_errors());
    });
    s.deadline = val::global("setTimeout")(s.loop.handler(id, 0), val(10'000), val::undefined());
    set_exit_code(1);  // until the verdict
    return 0;
}
