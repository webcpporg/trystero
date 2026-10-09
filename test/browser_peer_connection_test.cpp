// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the browser backend's peer connections, under node, which has no WebRTC of its own: when
// the page makes no `RTCPeerConnection` (none exists, or its constructor throws, as for a server it
// cannot use or with as many connections as the browser allows), the backend makes none and says
// so with a null connection, which the room reports, rather than letting the exception through;
// a connection whose `createDataChannel` throws, as a closed one's does, opens no channel.
//
// The page holds at most `backend::max_peer_connections` of them, counted from their construction
// until the garbage collector frees them, as Chromium counts them: beyond, the backend makes none,
// closed or dropped connections still count, and once a collection frees them it makes one again.
//
// main runs the checks that need no event, and starts the collection; the verdict comes from the
// census's release, and a deadline fails by name if it never comes. Node exits once nothing is
// pending.

#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript exports

#include <emscripten/em_asm.h>
#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <memory>
#include <memory_resource>
#include <vector>

namespace trystero = webcpp::trystero;
using emscripten::val;

namespace {

struct state {
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    trystero::browser::backend backend{loop};
    val deadline = val::undefined();
    bool done = false;
};

state& the_state() {
    static state s;
    return s;
}

val census() {
    return val::take_ownership(webcpp_trystero_js_census());
}

/** Calls `setTimeout` with a function that runs `f` through the loop, as the page's events do. */
val after(int milliseconds, trystero::browser::event_loop::callback f) {
    auto& loop = the_state().loop;
    const int id = loop.add(std::move(f));
    return val::global("setTimeout")(loop.handler(id, 0), val(milliseconds), val::undefined());
}

void set_exit_code(int code) {
    val::global("process").set("exitCode", val(code));
}

/** Once a collection released a connection, the backend makes one again. */
void on_release(int /*slot*/, const val& held) {
    auto& s = the_state();
    if (s.done || held.as<double>() >= trystero::browser::backend::max_peer_connections) {
        return;
    }
    s.done = true;
    BOOST_TEST(s.backend.make_peer_connection({}, std::pmr::get_default_resource()) != nullptr);
    val::global("clearTimeout")(s.deadline);
    set_exit_code(boost::report_errors());
}

}  // namespace

int main() {
    auto& s = the_state();
    std::pmr::memory_resource* const memory = std::pmr::get_default_resource();

    // node has no RTCPeerConnection.
    BOOST_TEST(s.backend.make_peer_connection({}, memory) == nullptr);

    // One whose constructor throws, as a browser's does at its limit of connections.
    EM_ASM({
        globalThis.RTCPeerConnection = class {
            constructor() { throw new DOMException('too many connections', 'OperationError'); }
        };
    });
    BOOST_TEST(s.backend.make_peer_connection({}, memory) == nullptr);
    BOOST_TEST_EQ(census()["held"].as<double>(), 0);

    // One that is made, whose createDataChannel throws, as a closed connection's does.
    EM_ASM({
        globalThis.RTCPeerConnection = class {
            createDataChannel() { throw new DOMException('closed', 'InvalidStateError'); }
            close() {}
        };
    });
    {
        const auto pc = s.backend.make_peer_connection({}, memory);
        if (BOOST_TEST(pc != nullptr)) {
            BOOST_TEST(pc->create_data_channel("data") == nullptr);
            pc->close();
        }
    }

    // The page's budget: the one above still counts, closed and dropped.
    constexpr std::size_t budget = trystero::browser::backend::max_peer_connections;
    {
        std::vector<std::shared_ptr<trystero::backend::peer_connection>> made;
        for (std::size_t i = 0; i < budget; ++i) {
            if (auto pc = s.backend.make_peer_connection({}, memory)) {
                made.push_back(std::move(pc));
            }
        }
        BOOST_TEST_EQ(made.size(), budget - 1);
        BOOST_TEST(s.backend.make_peer_connection({}, memory) == nullptr);
        for (const auto& pc : made) {
            pc->close();
        }
    }
    BOOST_TEST_EQ(census()["held"].as<double>(), static_cast<double>(budget));
    BOOST_TEST(s.backend.make_peer_connection({}, memory) == nullptr);

    // A collection frees them: node's gc, exposed for this test, from an event after this one.
    census().set("onrelease", s.loop.handler(s.loop.add(on_release), 0));
    after(0, [](int /*slot*/, const val& /*event*/) {
        EM_ASM({
            require('v8').setFlagsFromString('--expose-gc');
            require('vm').runInNewContext('gc')();
        });
    });
    s.deadline = after(10'000, [](int /*slot*/, const val& /*event*/) {
        BOOST_ERROR("no collection released a connection");
        set_exit_code(boost::report_errors());
    });
    set_exit_code(1);  // until the verdict
    return 0;
}
