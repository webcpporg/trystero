// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// One room's connection to one peer alone, over a memory pair: the frames it keeps before its room
// binds it and 0.27.0's bounds on them (`peer.ts`'s `failData`), the bound on a frame once bound,
// the drain waiters, and a disconnection's grace.

#include <webcpp/trystero/detail/peer_link.hpp>

#include "support/memory_backend.hpp"

#include <webcpp/asio/io_context.hpp>

#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cases.hpp"
#include "require.hpp"

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
namespace detail = webcpp::trystero::detail;
using namespace std::chrono_literals;

namespace {

bool run_until(asio::io_context& io, const std::function<bool()>& done,
               std::chrono::milliseconds limit = 5s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < end) {
        io.run_for(10ms);  // lint-run: the memory network posts to it
        if (io.stopped()) {
            io.restart();
        }
    }
    return done();
}

/** Runs the ready work of `io` until there is none: the memory network posts every frame. */
void settle(asio::io_context& io) {
    io.restart();
    while (io.poll() > 0) {
        io.restart();
    }
}

/** A link to "B" over one end of a memory pair, on the virtual clock, and what its room heard: the
 * frames handed over and the reason of its end. */
struct link_pair {
    asio::io_context io;
    trystero::test::memory_network net{io};
    std::size_t local_id = 0;  // the memory network's id of the link's connection
    std::shared_ptr<trystero::backend::peer_connection> remote_pc;
    std::shared_ptr<trystero::backend::data_channel> remote;
    std::shared_ptr<detail::peer_link> link;
    std::vector<std::vector<std::uint8_t>> got;
    std::string ended;
    std::size_t ends = 0;

    link_pair();

    /** Binds the link, recording what it hands over. */
    void bind() {
        const std::pmr::polymorphic_allocator<> alloc(std::pmr::get_default_resource());
        link->bind({
            .frame = {std::allocator_arg, alloc,
                      [this](trystero::backend::bytes frame) {
                          got.emplace_back(frame.begin(), frame.end());
                      }},
            .ended = {std::allocator_arg, alloc,
                      [this](std::string_view why) {
                          ended = why;
                          ++ends;
                      }},
        });
    }

    /** Has the other end send a frame of `size` bytes, all `fill`, and delivers it. */
    void receive(std::size_t size, std::uint8_t fill = 0) {
        remote->send(std::vector<std::uint8_t>(size, fill));
        settle(io);
    }
};

link_pair::link_pair() {
    net.virtual_clock = true;  // the link's timers wait for advance()
    auto* m = std::pmr::get_default_resource();
    auto pc = net.make_peer_connection({}, m);
    remote_pc = net.make_peer_connection({}, m);
    local_id = std::static_pointer_cast<trystero::test::memory_peer_connection>(pc)->id();
    auto local = pc->create_data_channel("data");
    remote_pc->set_handlers({
        .ice_candidate = {},
        .data_channel =
            [this](std::shared_ptr<trystero::backend::data_channel> c) { remote = std::move(c); },
        .state = {},
    });
    std::pmr::string offer(m);
    std::pmr::string answer(m);
    pc->set_local_description([&offer](trystero::backend::error_code, std::string_view,
                                       std::string_view sdp) { offer = sdp; });
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return !offer.empty(); })));
    remote_pc->set_remote_description("offer", offer, [](trystero::backend::error_code) {});
    remote_pc->set_local_description([&answer](trystero::backend::error_code, std::string_view,
                                               std::string_view sdp) { answer = sdp; });
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return !answer.empty(); })));
    pc->set_remote_description("answer", answer, [](trystero::backend::error_code) {});
    webcpp::test::require(
        BOOST_TEST(run_until(io, [&] { return local->is_open() && remote && remote->is_open(); })));
    remote_pc->set_handlers({});
    link = detail::peer_link::make("B", std::move(pc), std::move(local), net, m);
}

// Frames that arrive before the room binds the link are kept, and handed over in their order when
// it binds; a frame after the bind goes straight through.
void frames_before_bind_are_kept_in_order() {
    link_pair p;
    p.receive(3, 1);
    p.receive(4, 2);
    p.receive(5, 3);
    BOOST_TEST_EQ(p.link->pending_count(), 3U);
    BOOST_TEST(p.got.empty());
    p.bind();
    BOOST_TEST_EQ(p.link->pending_count(), 0U);
    webcpp::test::require(BOOST_TEST_EQ(p.got.size(), 3U));
    BOOST_TEST(p.got[0] == (std::vector<std::uint8_t>(3, 1)));
    BOOST_TEST(p.got[1] == (std::vector<std::uint8_t>(4, 2)));
    BOOST_TEST(p.got[2] == (std::vector<std::uint8_t>(5, 3)));
    p.receive(6, 4);
    BOOST_TEST_EQ(p.got.size(), 4U);
    BOOST_TEST(p.link->live());
    BOOST_TEST_EQ(p.ends, 0U);
}

// A frame whose call ends the link stops the replay of the frames kept: none after it is handed
// over, and the end is reported once.
void a_frame_that_ends_the_link_stops_the_replay() {
    link_pair p;
    p.receive(1, 1);
    p.receive(1, 2);
    p.receive(1, 3);
    const std::pmr::polymorphic_allocator<> alloc(std::pmr::get_default_resource());
    p.link->bind({
        .frame = {std::allocator_arg, alloc,
                  [&p](trystero::backend::bytes frame) {
                      p.got.emplace_back(frame.begin(), frame.end());
                      if (p.got.size() == 2) {
                          p.link->end("ended by the room");
                      }
                  }},
        .ended = {std::allocator_arg, alloc,
                  [&p](std::string_view why) {
                      p.ended = why;
                      ++p.ends;
                  }},
    });
    BOOST_TEST_EQ(p.got.size(), 2U);
    BOOST_TEST_EQ(p.ends, 1U);
    BOOST_TEST_EQ(p.ended, "ended by the room");
    BOOST_TEST(!p.link->live());
}

// The link keeps 64 frames before its bind, 0.27.0's maxQueuedDataFrames; a 65th ends it, and a
// room that binds it then hears of its end at once.
void a_65th_pending_frame_ends_the_link() {
    link_pair p;
    for (std::size_t i = 0; i < detail::peer_link::max_pending_frames; ++i) {
        p.receive(1);
    }
    BOOST_TEST_EQ(p.link->pending_count(), 64U);
    BOOST_TEST(p.link->live());
    p.receive(1);
    BOOST_TEST(!p.link->live());
    BOOST_TEST_EQ(p.link->pending_count(), 0U);
    BOOST_TEST(!p.remote->is_open());  // the other end sees the close
    p.bind();
    BOOST_TEST_EQ(p.ends, 1U);
    BOOST_TEST_EQ(p.ended, "excessive pre-handler data");
    BOOST_TEST(p.got.empty());
}

// The first frame kept starts 10 s, 0.27.0's pendingDataTimeoutMs, after which the link ends
// unless its room has bound it.
void pending_frames_end_the_link_after_10_s() {
    {
        link_pair p;
        p.receive(1);
        BOOST_TEST(p.net.advance(9'999ms));
        BOOST_TEST(p.link->live());
        BOOST_TEST(p.net.advance(1ms));
        BOOST_TEST(!p.link->live());
    }
    {
        link_pair p;
        p.receive(1);
        BOOST_TEST(p.net.advance(5'000ms));
        p.bind();
        BOOST_TEST(p.net.advance(10'000ms));
        BOOST_TEST(p.link->live());
        BOOST_TEST_EQ(p.got.size(), 1U);
        BOOST_TEST_EQ(p.ends, 0U);
    }
}

// Before its bind the link keeps a frame of at most 17,408 bytes, 0.27.0's maxRoomFrameBytes; one
// byte more ends it.
void an_unbound_frame_above_17_kib_ends_the_link() {
    {
        link_pair p;
        p.receive(17'408);
        BOOST_TEST(p.link->live());
        BOOST_TEST_EQ(p.link->pending_count(), 1U);
    }
    link_pair p;
    p.receive(17'409);
    BOOST_TEST(!p.link->live());
    p.bind();
    BOOST_TEST_EQ(p.ended, "excessive pre-handler data");
}

// Once bound, a frame of 65,536 bytes, 0.27.0's maxBulkFrameBytes, goes through, and one byte more
// ends the link.
void a_bound_frame_above_64_kib_ends_the_link() {
    link_pair p;
    p.bind();
    p.receive(65'536);
    BOOST_TEST_EQ(p.got.size(), 1U);
    BOOST_TEST(p.link->live());
    p.receive(65'537);
    BOOST_TEST_EQ(p.got.size(), 1U);
    BOOST_TEST(!p.link->live());
    BOOST_TEST_EQ(p.ends, 1U);
    BOOST_TEST_EQ(p.ended, "excessive pre-handler data");
}

// A disconnection ends the link after 5 s, as 0.27.0's grace; a reconnection before then cancels
// it.
void a_disconnected_state_ends_after_its_grace() {
    {
        link_pair p;
        p.bind();
        p.net.disconnect(p.local_id);
        BOOST_TEST(p.net.advance(4'999ms));
        BOOST_TEST_EQ(p.ends, 0U);
        BOOST_TEST(p.net.advance(1ms));
        BOOST_TEST_EQ(p.ended, "connection lost");
    }
    link_pair p;
    p.bind();
    p.net.disconnect(p.local_id);
    BOOST_TEST(p.net.advance(4'000ms));
    p.net.reconnect(p.local_id);
    BOOST_TEST(p.net.advance(10'000ms));
    BOOST_TEST_EQ(p.ends, 0U);
    BOOST_TEST(p.link->live());
}

// A failed connection, and a closed channel, end the link at once, once.
void the_transport_ends_the_link_once() {
    {
        link_pair p;
        p.bind();
        p.net.fail(p.local_id);
        settle(p.io);
        BOOST_TEST_EQ(p.ends, 1U);
        BOOST_TEST_EQ(p.ended, "connection failed");
    }
    link_pair p;
    p.bind();
    p.remote->close();
    settle(p.io);
    BOOST_TEST_EQ(p.ends, 1U);
    BOOST_TEST_EQ(p.ended, "data channel closed");
    p.link->end("again");
    BOOST_TEST_EQ(p.ends, 1U);
}

/** A waiter that records `name`, or `!name` when told the channel closed. */
trystero::handler<void(bool)> record(std::vector<std::string>& turns, std::string name) {
    return {std::allocator_arg, std::pmr::polymorphic_allocator<>(std::pmr::get_default_resource()),
            [&turns, name = std::move(name)](bool open) {
                turns.emplace_back(open ? name : "!" + name);
            }};
}

// The drain waiters complete in the order they began to wait once the channel drains; the end
// fails those still waiting, and every later wait at once.
void drain_waiters_complete_in_order_and_fail_at_the_end() {
    link_pair p;
    p.bind();
    std::vector<std::string> turns;
    p.link->wait_drained(record(turns, "empty"));
    BOOST_TEST(turns == (std::vector<std::string>{"empty"}));
    p.net.channel_capacity = 1;
    const std::vector<std::uint8_t> frame(16'000, 0);
    const auto fill = [&] {
        for (std::size_t i = 0; i < 5; ++i) {
            BOOST_TEST(p.link->send(frame));
        }
        webcpp::test::require(BOOST_TEST_GT(p.link->buffered_amount(),
                                            trystero::backend::data_channel::low_threshold));
    };
    fill();
    p.link->wait_drained(record(turns, "a"));
    p.link->wait_drained(record(turns, "b"));
    BOOST_TEST(run_until(p.io, [&] { return turns.size() == 3; }));
    BOOST_TEST(turns == (std::vector<std::string>{"empty", "a", "b"}));
    fill();
    p.link->wait_drained(record(turns, "c"));
    p.link->end("test");
    BOOST_TEST(turns == (std::vector<std::string>{"empty", "a", "b", "!c"}));
    BOOST_TEST_EQ(p.ended, "test");
    BOOST_TEST(!p.link->send(frame));
    BOOST_TEST_EQ(p.link->buffered_amount(), 0U);
    p.link->wait_drained(record(turns, "late"));
    BOOST_TEST_EQ(turns.back(), "!late");
}

}  // namespace

int main() {
    using webcpp::test::run_case;
    run_case(frames_before_bind_are_kept_in_order);
    run_case(a_frame_that_ends_the_link_stops_the_replay);
    run_case(a_65th_pending_frame_ends_the_link);
    run_case(pending_frames_end_the_link_after_10_s);
    run_case(an_unbound_frame_above_17_kib_ends_the_link);
    run_case(a_bound_frame_above_64_kib_ends_the_link);
    run_case(a_disconnected_state_ends_after_its_grace);
    run_case(the_transport_ends_the_link_once);
    run_case(drain_waiters_complete_in_order_and_fail_at_the_end);
    // Under Asyncify, a main that suspended loses what it returns: the status goes first.
    return webcpp::test::exit_status(boost::report_errors());
}
