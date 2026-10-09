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
// Every check runs inside main, from no event: nothing here waits.

#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript exports

#include <emscripten/em_asm.h>
#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <memory_resource>

namespace trystero = webcpp::trystero;

int main() {
    boost::asio::io_context io;
    trystero::browser::event_loop loop(io);
    trystero::browser::backend backend(loop);
    std::pmr::memory_resource* const memory = std::pmr::get_default_resource();

    // node has no RTCPeerConnection.
    BOOST_TEST(backend.make_peer_connection({}, memory) == nullptr);

    // One whose constructor throws, as a browser's does at its limit of connections.
    EM_ASM({
        globalThis.RTCPeerConnection = class {
            constructor() { throw new DOMException('too many connections', 'OperationError'); }
        };
    });
    BOOST_TEST(backend.make_peer_connection({}, memory) == nullptr);

    // One that is made, whose createDataChannel throws, as a closed connection's does.
    EM_ASM({
        globalThis.RTCPeerConnection = class {
            createDataChannel() { throw new DOMException('closed', 'InvalidStateError'); }
            close() {}
        };
    });
    const auto pc = backend.make_peer_connection({}, memory);
    if (BOOST_TEST(pc != nullptr)) {
        BOOST_TEST(pc->create_data_channel("data") == nullptr);
    }
    return boost::report_errors();
}
