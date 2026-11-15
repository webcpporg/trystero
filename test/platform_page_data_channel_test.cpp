// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the platform backend's data channels in a page, webcpp::webrtc's, under node, over plain
// objects in
// place of the page's RTCPeerConnection and RTCDataChannel: what a peer can send that is no frame
// is dropped. A text message "1e300" once threw a RangeError out of `new Uint8Array`, and
// "300000000" made a 300 MB frame: webcpp::webrtc delivers a text as the few bytes it holds, which
// the channel drops; a binary message larger than any room frame is dropped too. Only the frame
// sent last reaches the handler, and nothing larger than the largest message sent is allocated.
//
// main runs the io_context under a work guard, which the verdict releases when the last frame
// arrives; a frame that does not come within 10 s fails by name.

#include <webcpp/trystero/platform/backend.hpp>

#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>

#include <emscripten/em_asm.h>
#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>

#include "require.hpp"

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using emscripten::val;
using namespace std::chrono_literals;

namespace {

/** A resource that remembers the largest allocation it made. */
class watching_resource final : public std::pmr::memory_resource {
public:
    std::size_t largest = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        largest = std::max(largest, bytes);
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct state {
    watching_resource memory;
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    trystero::platform::backend backend{io.get_executor()};
    std::shared_ptr<trystero::backend::peer_connection> connection;
    std::shared_ptr<trystero::backend::data_channel> wrapper;
    int messages = 0;
    std::size_t last_size = 0;
};

/** Delivers `data` to the channel's `onmessage` from a timer, as the page delivers a message. */
void deliver(const val& data) {
    val event = val::object();
    event.set("data", data);
    val::global("setTimeout")(val::global("lastChannel")["onmessage"], val(0), event);
}

}  // namespace

int main() {
    state s;
    // A connection whose channel is open from the start, and which the test reaches as
    // lastChannel.
    EM_ASM({
        globalThis.RTCPeerConnection = class {
            constructor() {
                this.connectionState = 'new';
                this.iceConnectionState = 'new';
                this.localDescription = null;
                this.remoteDescription = null;
            }
            createDataChannel(label) {
                const channel = {};
                channel.label = label;
                channel.readyState = 'open';
                channel.bufferedAmount = 0;
                channel.send = function(){};
                channel.close = function(){};
                globalThis.lastChannel = channel;
                return channel;
            }
            close() {}
        };
    });
    s.connection = s.backend.make_peer_connection({}, &s.memory);
    webcpp::test::require(BOOST_TEST(s.connection != nullptr));
    s.wrapper = s.connection->create_data_channel("data");
    webcpp::test::require(BOOST_TEST(s.wrapper != nullptr));
    s.wrapper->set_handlers({
        .open = {},
        .message =
            [&s](const trystero::backend::bytes& frame) {
                ++s.messages;
                s.last_size = frame.size();
                s.guard.reset();
                s.io.stop();
            },
        .close = {},
        .buffered_amount_low = {},
    });

    deliver(val("1e300"));
    deliver(val("300000000"));
    deliver(val::global("ArrayBuffer").new_(val(20'000)));
    deliver(val::global("ArrayBuffer").new_(val(3)));

    s.io.run_for(10s);  // lint-run: the test's event loop, held by its work guard until the verdict
    if (s.messages == 0) {
        BOOST_ERROR("the frame sent last never arrived");
    }
    // The first frame to arrive is the last one sent: the run stopped there.
    BOOST_TEST_EQ(s.messages, 1);
    BOOST_TEST_EQ(s.last_size, 3U);
    BOOST_TEST_LE(s.memory.largest, std::size_t{20'000});
    return webcpp::test::exit_status(boost::report_errors());
}
