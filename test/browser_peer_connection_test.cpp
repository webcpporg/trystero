// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the browser backend's peer connections, webcpp::webrtc's, under node, which has no WebRTC
// of its own: when the page makes no `RTCPeerConnection` (none exists, or its constructor throws,
// as for a server it cannot use or with as many connections as the browser allows), the backend
// makes none and says so with a null connection, which the room reports, rather than letting the
// exception through; a connection whose `createDataChannel` throws, as a closed one's does, opens
// no channel.
//
// The page holds at most `backend::max_peer_connections` of them, the cap of the backend's
// webcpp::webrtc runtime, counted by webcpp::webrtc's census of the page from their construction
// until the garbage collector frees them, as Chromium counts them: beyond, the backend makes none,
// closed or dropped connections still count, and once a collection frees them it makes one again.
//
// main runs the checks that need no event, and runs the io_context, where a timer collects and
// looks every 10 ms for the census to count a connection less, the verdict; a verdict that does
// not come within 10 s fails by name.

#include <webcpp/trystero/browser/backend.hpp>

#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/post.hpp>
#include <webcpp/asio/steady_timer.hpp>

#include <emscripten/em_asm.h>
#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <webcpp/test/exit_status.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <vector>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using emscripten::val;
using namespace std::chrono_literals;

namespace {

struct state {
    asio::io_context io;
    trystero::browser::backend backend{io.get_executor()};
    asio::steady_timer look{io};
    bool done = false;
};

/** The connections webcpp::webrtc's census of the page counts. */
double held() {
    const val census = val::global(
        "globalThis")[val::global("Symbol").call<val>("for", val("webcpp.webrtc.census"))];
    return census.isUndefined() ? 0 : census["held"].as<double>();
}

/** Runs node's garbage collector, exposed for this test. */
void collect() {
    EM_ASM({
        require('v8').setFlagsFromString('--expose-gc');
        require('vm').runInNewContext('gc')();
    });
}

/** Once a collection released a connection, the backend makes one again; until then, it collects
 * and looks again 10 ms later, while node runs the collection's finalizers. */
void look_for_release(state& s) {
    if (held() >= trystero::browser::backend::max_peer_connections) {
        collect();
        s.look.expires_after(10ms);
        s.look.async_wait([&s](boost::system::error_code ec) {
            if (!ec) {
                look_for_release(s);
            }
        });
        return;
    }
    s.done = true;
    BOOST_TEST(s.backend.make_peer_connection({}, std::pmr::get_default_resource()) != nullptr);
    s.io.stop();
}

}  // namespace

int main() {
    state s;
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
    BOOST_TEST_EQ(held(), 0);

    // One that is made, whose createDataChannel throws, as a closed connection's does.
    EM_ASM({
        globalThis.RTCPeerConnection = class {
            constructor() {
                this.connectionState = 'new';
                this.iceConnectionState = 'new';
                this.localDescription = null;
                this.remoteDescription = null;
            }
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
    BOOST_TEST_EQ(held(), static_cast<double>(budget));
    BOOST_TEST(s.backend.make_peer_connection({}, memory) == nullptr);

    // A collection frees them: node's gc, from a handler of the io_context, after the
    // connections' own posted work; the census counts the release once the FinalizationRegistry
    // has seen it, from node's event loop, while the run waits.
    asio::post(s.io, [&s] { look_for_release(s); });
    s.io.run_for(10s);  // lint-run: the test's event loop, held by its timer until the verdict
    if (!s.done) {
        BOOST_ERROR("no collection released a connection");
    }
    return webcpp::test::exit_status(boost::report_errors());
}
