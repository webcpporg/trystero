// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the platform backend's peer connections in a page, webcpp::webrtc's, under node, which has
// no WebRTC of its own: when the page makes no `RTCPeerConnection` (none exists, or its constructor
// throws, as for a server it cannot use or with as many connections as the browser allows), the
// backend makes none and says so with a null connection, which the room reports, rather than
// letting the exception through; a connection whose `createDataChannel` throws, as a closed one's
// does, opens no channel. A connection's largest message is its SCTP transport's, unknown while it
// has none (max_message_size_is_the_transports).
//
// The page holds at most `runtime_options::max_peer_connections` of them, 256 by default, the cap
// of the backend's webcpp::webrtc runtime, counted by webcpp::webrtc's census of the page from
// their construction until the garbage collector frees them, as Chromium counts them: beyond, the
// backend makes none, closed or dropped connections still count, and once a collection frees them
// it makes one again. The backend's window, `backend_config::max_peer_connections_per_minute`,
// holds in a page as natively: a backend whose window is two makes no third connection within the
// minute, however many the census allows. The census's backend gets a window wider than all it
// makes here.
//
// main runs the checks that need no event, and runs the io_context, where a timer collects and
// looks every 10 ms for the census to count a connection less, the verdict; a verdict that does
// not come within 10 s fails by name.

#include <webcpp/trystero/platform/backend.hpp>

#include <webcpp/webrtc/options.hpp>

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
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <vector>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using emscripten::val;
using namespace std::chrono_literals;

namespace {

/** The page's cap on its connections alive at once: the default of the backend's runtime. */
constexpr std::size_t budget = webcpp::webrtc::runtime_options{}.max_peer_connections;

struct state {
    asio::io_context io;
    trystero::platform::backend backend{io.get_executor(),
                                        {.max_peer_connections_per_minute = 4 * budget}};
    asio::steady_timer look{io};
    bool done = false;
};

/** The connections webcpp::webrtc's census of the page counts. */
std::size_t held() {
    const val census = val::global(
        "globalThis")[val::global("Symbol").call<val>("for", val("webcpp.webrtc.census"))];
    return census.isUndefined() ? 0 : static_cast<std::size_t>(census["held"].as<double>());
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
    if (held() >= budget) {
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

/** The `sctp` of the page's last connection, the stand-in's: `null`, or a transport whose
 * `maxMessageSize` is `size`. */
void set_sctp(std::optional<double> size) {
    if (size) {
        EM_ASM({ globalThis.lastConnection.sctp = {maxMessageSize : $0}; }, *size);
    } else {
        EM_ASM({ globalThis.lastConnection.sctp = null; });
    }
}

// The connection's largest message is the page's own `sctp.maxMessageSize`, as webcpp::webrtc's
// max_message_size() reads it, within webcpp::webrtc's 256 KiB: nothing while the connection has
// no SCTP transport, then what the transport says, at most 256 KiB for a larger size or none.
void max_message_size_is_the_transports(trystero::backend::peer_connection& pc) {
    BOOST_TEST(!pc.max_message_size().has_value());
    set_sctp(16384);
    BOOST_TEST(pc.max_message_size() == std::optional<std::size_t>(16384));
    set_sctp(65536);
    BOOST_TEST(pc.max_message_size() == std::optional<std::size_t>(65536));
    set_sctp(1073741823);
    BOOST_TEST(pc.max_message_size() == std::optional<std::size_t>(262144));
    set_sctp(std::numeric_limits<double>::infinity());
    BOOST_TEST(pc.max_message_size() == std::optional<std::size_t>(262144));
    set_sctp(std::nullopt);
    BOOST_TEST(!pc.max_message_size().has_value());
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

    // One that is made, whose createDataChannel throws, as a closed connection's does, and whose
    // SCTP transport is the test's to set.
    EM_ASM({
        globalThis.RTCPeerConnection = class {
            constructor() {
                this.connectionState = 'new';
                this.iceConnectionState = 'new';
                this.localDescription = null;
                this.remoteDescription = null;
                this.sctp = null;
                globalThis.lastConnection = this;
            }
            createDataChannel() { throw new DOMException('closed', 'InvalidStateError'); }
            close() {}
        };
    });
    {
        const auto pc = s.backend.make_peer_connection({}, memory);
        if (BOOST_TEST(pc != nullptr)) {
            BOOST_TEST(pc->create_data_channel("data") == nullptr);
            max_message_size_is_the_transports(*pc);
            pc->close();
        }
    }

    // The backend's window: two in a minute, while the census has room for more.
    {
        trystero::platform::backend windowed(s.io.get_executor(),
                                             {.max_peer_connections_per_minute = 2});
        const auto first = windowed.make_peer_connection({}, memory);
        const auto second = windowed.make_peer_connection({}, memory);
        BOOST_TEST(first != nullptr);
        BOOST_TEST(second != nullptr);
        BOOST_TEST(windowed.make_peer_connection({}, memory) == nullptr);
        BOOST_TEST_EQ(held(), 3);
        for (const auto& pc : {first, second}) {
            if (pc) {
                pc->close();
            }
        }
    }

    // The page's budget: the three above still count, closed and dropped.
    {
        std::vector<std::shared_ptr<trystero::backend::peer_connection>> made;
        for (std::size_t i = 0; i < budget; ++i) {
            if (auto pc = s.backend.make_peer_connection({}, memory)) {
                made.push_back(std::move(pc));
            }
        }
        BOOST_TEST_EQ(made.size(), budget - 3);
        BOOST_TEST(s.backend.make_peer_connection({}, memory) == nullptr);
        for (const auto& pc : made) {
            pc->close();
        }
    }
    BOOST_TEST_EQ(held(), budget);
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
