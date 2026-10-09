// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the browser backend's data channels, under node, over a plain object in place of the
// page's RTCDataChannel: what a peer can send that is no frame is dropped before it is read. A text
// message "1e300" once threw a RangeError out of `new Uint8Array`, and "300000000" made a 300 MB
// frame; a binary message larger than any room frame is dropped too. Only the frame sent last
// reaches the handler, and nothing larger than it is allocated.
//
// main sets the test up and returns with node's exit code at 1; the verdict comes when the last
// frame arrives, or from a deadline that fails by name, and node exits once nothing is pending.

#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript export

#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <cstddef>
#include <memory>
#include <memory_resource>

namespace trystero = webcpp::trystero;
using emscripten::val;

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
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    watching_resource memory;
    val channel = val::object();
    std::shared_ptr<trystero::browser::data_channel> wrapper;
    int messages = 0;
    std::size_t last_size = 0;
    val deadline = val::undefined();
};

state& the_state() {
    static state s;
    return s;
}

void set_exit_code(int code) {
    val::global("process").set("exitCode", val(code));
}

/** Delivers `data` to the channel's `onmessage` from a timer, as the page delivers a message. */
void deliver(const val& data) {
    val event = val::object();
    event.set("data", data);
    val::global("setTimeout")(the_state().channel["onmessage"], val(0), event);
}

}  // namespace

int main() {
    auto& s = the_state();
    s.channel.set("readyState", val("open"));
    s.wrapper = std::make_shared<trystero::browser::data_channel>(s.loop, s.channel, &s.memory);
    s.wrapper->set_handlers({
        .open = {},
        .message =
            [](const trystero::backend::bytes& frame) {
                auto& s = the_state();
                ++s.messages;
                s.last_size = frame.size();
                BOOST_TEST_EQ(s.messages, 1);
                BOOST_TEST_EQ(s.last_size, 3U);
                BOOST_TEST_LT(s.memory.largest, std::size_t{1024});
                val::global("clearTimeout")(s.deadline);
                set_exit_code(boost::report_errors());
            },
        .close = {},
        .buffered_amount_low = {},
    });

    deliver(val("1e300"));
    deliver(val("300000000"));
    deliver(val::global("ArrayBuffer").new_(val(20'000)));
    deliver(val::global("ArrayBuffer").new_(val(3)));

    const int id = s.loop.add([](int /*slot*/, const val& /*event*/) {
        BOOST_ERROR("the frame sent last never arrived");
        set_exit_code(boost::report_errors());
    });
    s.deadline = val::global("setTimeout")(s.loop.handler(id, 0), val(10'000), val::undefined());
    set_exit_code(1);  // until the verdict
    return 0;
}
