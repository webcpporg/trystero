// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests that the platform backend's handlers in a page run outside the page's events, under node:
// a relay socket's open, message and close handlers, over a stand-in for the page's WebSocket, and
// the completion of a password's PBKDF2, through the page's WebCrypto, each find webcpp::asio's
// bridge delivering nothing (`bridge::delivering()` false): the page's event, or the
// settlement of a promise, only posts to the io_context, and the run family runs the handler after
// it. A probe proves that the check sees a delivery: inside a call of a bridge's function, which a
// page's event is, `delivering()` is true.
//
// main runs the io_context under a work guard, as a page's program does; once the socket reported
// its close and the derivation completed, the verdict releases the guard. A step that does
// not come within 10 s fails by name.

#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/protocol/room_keys.hpp>

#include <webcpp/asio/browser/bridge.hpp>
#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>

#include <emscripten/em_js.h>
#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <webcpp/test/exit_status.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using emscripten::val;
using namespace std::chrono_literals;

// The stand-in relay: it opens on the next turn, answers the first text sent with a message event
// that carries it, and on the turn after, closes. Its events are a page's: each names its type, and
// a close its code and reason.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, install_echo_websocket, (), {
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
            setTimeout(() => {
                if (this.onmessage) this.onmessage({type: "message", data: text});
                setTimeout(() => {
                    this.readyState = 3;
                    if (this.onclose) this.onclose({type: "close", code: 1000, reason: "done"});
                }, 0);
            }, 0);
        }
        close() {
            this.readyState = 3;
        }
    };
});
// clang-format on

namespace {

/** A registration of the bridge's that records whether the bridge delivers while it is called. */
class probe final : public asio::browser::registration {
public:
    explicit probe(asio::browser::bridge& bridge) : bridge_(bridge) {}

    bool deliver(const val& /*event*/) override {
        saw_delivering = bridge_.delivering();
        return false;
    }

    void shutdown() noexcept override {}

    bool saw_delivering = false;

private:
    asio::browser::bridge& bridge_;
};

/** Where a handler ran: whether it ran at all, and whether inside a delivery of the bridge's. */
struct seen {
    bool ran = false;
    bool inside_an_event = false;
};

struct state {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    trystero::platform::backend backend{io.get_executor()};
    std::shared_ptr<trystero::backend::websocket> socket;
    std::array<std::uint8_t, 32> salt = trystero::protocol::pbkdf2_salt("my-app", "room-1");
    std::array<std::uint8_t, 32> master{};
    std::string echoed;
    seen open;
    seen message;
    seen close;
    seen derived;
    bool judged = false;
};

/** Records that a handler ran, and whether the bridge was delivering an event while it did. */
void record(state& s, seen& into) {
    into.ran = true;
    into.inside_an_event = asio::use_service<asio::browser::bridge>(s.io).delivering();
}

/** Once the socket closed and the derivation completed: the verdict. */
void judge(state& s) {
    if (!s.close.ran || !s.derived.ran) {
        return;
    }
    s.socket.reset();
    s.judged = true;
    s.guard.reset();
}

/** Derives a password's master, recording where its completion ran. */
void derive(state& s) {
    constexpr std::string_view password = "secret";
    s.backend.derive_password_master(
        std::span(reinterpret_cast<const std::uint8_t*>(password.data()), password.size()), s.salt,
        s.master, [&s](boost::system::error_code ec) {
            record(s, s.derived);
            BOOST_TEST(!ec);
            judge(s);
        });
}

/** Checks that a handler ran outside every event of the page's. */
void expect_outside(const seen& got, const char* name) {
    if (!got.ran) {
        BOOST_ERROR(name);
        return;
    }
    BOOST_TEST(!got.inside_an_event);
}

}  // namespace

int main() {
    install_echo_websocket();
    state s;
    {
        auto& bridge = asio::use_service<asio::browser::bridge>(s.io);
        probe p(bridge);
        const std::uint64_t key = bridge.add(p);
        bridge.function(key)(0);
        bridge.remove(key);
        BOOST_TEST(p.saw_delivering);
        BOOST_TEST(!bridge.delivering());
    }
    s.socket = s.backend.connect("wss://relay.test",
                                 {
                                     .open =
                                         [&s] {
                                             record(s, s.open);
                                             s.socket->send("hello");
                                         },
                                     .message =
                                         [&s](std::string_view text) {
                                             record(s, s.message);
                                             s.echoed = text;
                                         },
                                     .close =
                                         [&s] {
                                             record(s, s.close);
                                             judge(s);
                                         },
                                 },
                                 std::pmr::get_default_resource());
    derive(s);
    s.io.run_for(10s);  // lint-run: the test's event loop, held by its work guard until the verdict
    if (!s.judged) {
        BOOST_ERROR("the socket's close or the derivation's completion never came");
    }
    expect_outside(s.open, "the socket's open handler never ran");
    expect_outside(s.message, "the socket's message handler never ran");
    expect_outside(s.close, "the socket's close handler never ran");
    expect_outside(s.derived, "the derivation never completed");
    BOOST_TEST_EQ(s.echoed, "hello");
    BOOST_TEST(std::ranges::any_of(s.master, [](std::uint8_t b) { return b != 0; }));
    return webcpp::test::exit_status(boost::report_errors());
}
