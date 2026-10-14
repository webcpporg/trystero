// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The memory backend's hooks, which the tests of shared connections drive: a filter that delivers,
// drops or holds frames in the channel's order, deferred opens, injected frames, a forced failure,
// and a channel that buffers and drains.

#include "support/memory_backend.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <vector>

#include "require.hpp"

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;
using trystero::test::memory_network;
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

/** Two ends of one pair, their channels and what each received. */
struct pair {
    std::shared_ptr<trystero::backend::peer_connection> offerer;
    std::shared_ptr<trystero::backend::peer_connection> answerer;
    std::shared_ptr<trystero::backend::data_channel> offer_end;
    std::shared_ptr<trystero::backend::data_channel> answer_end;
    std::vector<std::vector<std::uint8_t>> at_offerer;
    std::vector<std::vector<std::uint8_t>> at_answerer;
    std::vector<trystero::backend::connection_state> offerer_states;
    std::size_t offerer_id = 0;
    std::size_t answerer_id = 0;
};

/** Makes and pairs two peer connections, as two rooms' negotiations do. */
void connect(memory_network& net, pair& p) {
    std::vector<std::size_t> made;
    net.on_made = [&made](std::size_t pc, std::span<const trystero::backend::ice_server>) {
        made.push_back(pc);
    };
    auto* memory = std::pmr::get_default_resource();
    p.offerer = net.make_peer_connection({}, memory);
    p.answerer = net.make_peer_connection({}, memory);
    net.on_made = {};
    p.offerer_id = made.at(0);
    p.answerer_id = made.at(1);
    p.offer_end = p.offerer->create_data_channel("data");
    p.offerer->set_handlers({
        .ice_candidate = {},
        .data_channel = {},
        .state = [&p](trystero::backend::connection_state s) { p.offerer_states.push_back(s); },
    });
    p.answerer->set_handlers({
        .ice_candidate = {},
        .data_channel =
            [&p](std::shared_ptr<trystero::backend::data_channel> c) {
                p.answer_end = std::move(c);
                p.answer_end->set_handlers({
                    .open = {},
                    .message =
                        [&p](trystero::backend::bytes b) {
                            p.at_answerer.emplace_back(b.begin(), b.end());
                        },
                    .close = {},
                    .buffered_amount_low = {},
                });
            },
        .state = {},
    });
    p.offer_end->set_handlers({
        .open = {},
        .message =
            [&p](trystero::backend::bytes b) { p.at_offerer.emplace_back(b.begin(), b.end()); },
        .close = {},
        .buffered_amount_low = {},
    });
    std::pmr::string offer(memory);
    p.offerer->set_local_description([&offer](trystero::backend::error_code, std::string_view,
                                              std::string_view sdp) { offer = sdp; });
    // Each side's description is the other's remote one, as the signaling hands them over.
    webcpp::test::require(BOOST_TEST(run_until(net.io(), [&] { return !offer.empty(); })));
    p.answerer->set_remote_description("offer", offer, [](trystero::backend::error_code) {});
    std::pmr::string answer(memory);
    p.answerer->set_local_description([&answer](trystero::backend::error_code, std::string_view,
                                                std::string_view sdp) { answer = sdp; });
    webcpp::test::require(BOOST_TEST(run_until(net.io(), [&] { return !answer.empty(); })));
    p.offerer->set_remote_description("answer", answer, [](trystero::backend::error_code) {});
}

void ids_are_sizes_and_peer_of_pairs_them() {
    asio::io_context io;
    memory_network net{io};
    pair p;
    connect(net, p);
    BOOST_TEST(run_until(io, [&] { return p.answer_end && p.answer_end->is_open(); }));
    BOOST_TEST_EQ(net.peer_of(p.offerer_id), p.answerer_id);
    BOOST_TEST_EQ(net.peer_of(p.answerer_id), p.offerer_id);
}

void a_held_frame_holds_every_later_frame_of_its_end() {
    asio::io_context io;
    memory_network net{io};
    pair p;
    connect(net, p);
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); })));
    net.filter = [](std::size_t, std::size_t, std::span<const std::uint8_t> frame) {
        return frame[0] == 'A' ? memory_network::verdict::hold : memory_network::verdict::deliver;
    };
    const std::array<std::uint8_t, 1> a{'A'};
    const std::array<std::uint8_t, 1> b{'B'};
    p.offer_end->send(a);
    p.offer_end->send(b);
    run_until(io, [] { return false; }, 100ms);
    BOOST_TEST(p.at_answerer.empty());  // B waits behind A, as on an ordered channel
    net.filter = {};
    net.release(p.answerer_id);
    BOOST_TEST(run_until(io, [&] { return p.at_answerer.size() == 2; }));
    BOOST_TEST(p.at_answerer.at(0).at(0) == 'A' && p.at_answerer.at(1).at(0) == 'B');
}

void a_dropped_frame_is_gone() {
    asio::io_context io;
    memory_network net{io};
    pair p;
    connect(net, p);
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); })));
    net.filter = [](std::size_t, std::size_t, std::span<const std::uint8_t> frame) {
        return frame[0] == 'A' ? memory_network::verdict::drop : memory_network::verdict::deliver;
    };
    const std::array<std::uint8_t, 1> a{'A'};
    const std::array<std::uint8_t, 1> b{'B'};
    p.offer_end->send(a);
    p.offer_end->send(b);
    BOOST_TEST(run_until(io, [&] { return p.at_answerer.size() == 1; }));
    BOOST_TEST(p.at_answerer.at(0).at(0) == 'B');
}

void deferred_opens_open_one_end_at_a_time() {
    asio::io_context io;
    memory_network net{io};
    net.defer_opens = true;
    pair p;
    connect(net, p);
    run_until(io, [] { return false; }, 100ms);
    BOOST_TEST(!p.offer_end->is_open());
    net.open(p.offerer_id);
    BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); }));
    BOOST_TEST(!p.answer_end || !p.answer_end->is_open());
    net.open(p.answerer_id);
    BOOST_TEST(run_until(io, [&] { return p.answer_end && p.answer_end->is_open(); }));
}

void an_injected_frame_reaches_the_end_named() {
    asio::io_context io;
    memory_network net{io};
    pair p;
    connect(net, p);
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); })));
    const std::array<std::uint8_t, 1> x{'X'};
    net.inject(p.offerer_id, x);
    BOOST_TEST(run_until(io, [&] { return p.at_offerer.size() == 1; }));
}

// `failed` is reported while the channel is still open, and the channel's close after it.
void a_failed_connection_reports_failed_then_closes() {
    asio::io_context io;
    memory_network net{io};
    pair p;
    connect(net, p);
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); })));
    std::vector<std::string_view> events;
    p.offerer->set_handlers({
        .ice_candidate = {},
        .data_channel = {},
        .state =
            [&](trystero::backend::connection_state s) {
                if (s == trystero::backend::connection_state::failed) {
                    events.emplace_back(p.offer_end->is_open() ? "failed while open"
                                                               : "failed once closed");
                }
            },
    });
    p.offer_end->set_handlers({
        .open = {},
        .message = {},
        .close = [&events] { events.emplace_back("closed"); },
        .buffered_amount_low = {},
    });
    net.fail(p.offerer_id);
    BOOST_TEST(run_until(io, [&] { return events.size() == 2; }));
    BOOST_TEST((events == std::vector<std::string_view>{"failed while open", "closed"}));
    BOOST_TEST(!p.offer_end->is_open());
}

// A disconnection and a reconnection are reported as states, and the channel stays open: only a
// failure closes it.
void a_disconnection_and_a_reconnection_are_reported() {
    asio::io_context io;
    memory_network net{io};
    pair p;
    connect(net, p);
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); })));
    p.offerer_states.clear();
    net.disconnect(p.offerer_id);
    net.reconnect(p.offerer_id);
    BOOST_TEST(run_until(io, [&] { return p.offerer_states.size() == 2; }));
    BOOST_TEST((p.offerer_states == std::vector<trystero::backend::connection_state>{
                                        trystero::backend::connection_state::disconnected,
                                        trystero::backend::connection_state::connected,
                                    }));
    BOOST_TEST(p.offer_end->is_open());
}

// A frame larger than drain_per_post drains byte by byte and is delivered with its last byte.
void a_frame_is_delivered_once_its_last_byte_drained() {
    asio::io_context io;
    memory_network net{io};
    net.channel_capacity = 1;
    net.drain_per_post = 1024;
    pair p;
    connect(net, p);
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); })));
    const std::vector<std::uint8_t> frame(std::size_t{16} * 1024, 5);
    p.offer_end->send(frame);
    BOOST_TEST_EQ(net.buffered(p.offerer_id), frame.size());
    // Each poll_one runs one drain step, the only handler queued, which posts the next.
    io.poll_one();
    BOOST_TEST_EQ(net.buffered(p.offerer_id), frame.size() - 1024);
    for (int step = 1; step < 15; ++step) {
        io.poll_one();
    }
    BOOST_TEST_EQ(net.buffered(p.offerer_id), 1024U);
    BOOST_TEST(p.at_answerer.empty());  // its last 1024 bytes have not drained yet
    BOOST_TEST(run_until(io, [&] { return p.at_answerer.size() == 1; }));
    BOOST_TEST_EQ(net.buffered(p.offerer_id), 0U);
}

// A channel closed while it drains transmits nothing more: its next step drops what it buffered.
void a_closed_channel_drains_nothing_more() {
    asio::io_context io;
    memory_network net{io};
    net.channel_capacity = 1;
    net.drain_per_post = 1024;
    pair p;
    connect(net, p);
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); })));
    const std::vector<std::uint8_t> frame(std::size_t{16} * 1024, 5);
    p.offer_end->send(frame);
    io.poll_one();
    BOOST_TEST_EQ(net.buffered(p.offerer_id), frame.size() - 1024);
    p.offer_end->close();
    io.poll_one();  // the next drain step, queued before the close's handlers
    BOOST_TEST_EQ(net.buffered(p.offerer_id), 0U);
    run_until(io, [] { return false; }, 100ms);
    BOOST_TEST(p.at_answerer.empty());
}

// Release filters each frame again: a frame the filter still holds stays held, with what is behind
// it, until the next release.
void a_release_filters_each_frame_again() {
    asio::io_context io;
    memory_network net{io};
    pair p;
    connect(net, p);
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); })));
    net.filter = [](std::size_t, std::size_t, std::span<const std::uint8_t>) {
        return memory_network::verdict::hold;
    };
    const std::array<std::uint8_t, 1> a{'A'};
    const std::array<std::uint8_t, 1> b{'B'};
    p.offer_end->send(a);
    p.offer_end->send(b);
    net.filter = [](std::size_t, std::size_t, std::span<const std::uint8_t> frame) {
        return frame[0] == 'B' ? memory_network::verdict::hold : memory_network::verdict::deliver;
    };
    net.release(p.answerer_id);
    BOOST_TEST(run_until(io, [&] { return p.at_answerer.size() == 1; }));
    run_until(io, [] { return false; }, 100ms);
    BOOST_TEST_EQ(p.at_answerer.size(), 1U);
    BOOST_TEST(p.at_answerer.at(0).at(0) == 'A');
    net.filter = {};
    net.release(p.answerer_id);
    BOOST_TEST(run_until(io, [&] { return p.at_answerer.size() == 2; }));
    BOOST_TEST(p.at_answerer.at(1).at(0) == 'B');
}

// A frame held for an end whose sending connection no longer has the channel that sent it, which
// the connection replaced by one never paired, is dropped on release.
void a_release_after_the_sending_channel_was_replaced_drops_its_frames() {
    asio::io_context io;
    memory_network net{io};
    pair p;
    connect(net, p);
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); })));
    net.filter = [](std::size_t, std::size_t, std::span<const std::uint8_t>) {
        return memory_network::verdict::hold;
    };
    const std::array<std::uint8_t, 1> a{'A'};
    p.offer_end->send(a);
    net.filter = {};
    const auto replacement = p.offerer->create_data_channel("again");
    BOOST_TEST(replacement != p.offer_end);
    net.release(p.answerer_id);
    run_until(io, [] { return false; }, 100ms);
    BOOST_TEST(p.at_answerer.empty());
}

// A frame held for an end whose sender is gone is dropped on release.
void a_release_after_the_sender_went_drops_its_frames() {
    asio::io_context io;
    memory_network net{io};
    pair p;
    connect(net, p);
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); })));
    net.filter = [](std::size_t, std::size_t, std::span<const std::uint8_t>) {
        return memory_network::verdict::hold;
    };
    const std::array<std::uint8_t, 1> a{'A'};
    p.offer_end->send(a);
    net.filter = {};
    p.offer_end.reset();
    p.offerer.reset();  // the sending end is destroyed
    net.release(p.answerer_id);
    run_until(io, [] { return false; }, 100ms);
    BOOST_TEST(p.at_answerer.empty());
}

// The virtual clock: a timer fires only when the test advances the clock to it, in order, and
// now_ms is the clock's.
void the_virtual_clock_moves_only_when_advanced() {
    asio::io_context io;
    memory_network net{io};
    net.virtual_clock = true;
    auto* m = std::pmr::get_default_resource();
    std::vector<int> fired;
    const auto t10 = net.make_timer(m);
    const auto t5 = net.make_timer(m);
    const auto cancelled = net.make_timer(m);
    t10->start(10s, [&fired] { fired.push_back(10); });
    t5->start(5s, [&fired] { fired.push_back(5); });
    cancelled->start(1s, [&fired] { fired.push_back(1); });
    cancelled->cancel();
    io.poll();
    BOOST_TEST(fired.empty());
    BOOST_TEST_EQ(net.now_ms(), 0.0);
    net.advance(7s);
    BOOST_TEST(fired == std::vector<int>{5});
    BOOST_TEST_EQ(net.now_ms(), 7000.0);
    net.advance(3s);
    BOOST_TEST((fired == std::vector<int>{5, 10}));
    BOOST_TEST_EQ(net.now_ms(), 10000.0);
    // Two timers due at the same instant both fire, in the order they were started, not made.
    const auto made_first = net.make_timer(m);
    const auto made_second = net.make_timer(m);
    made_second->start(2s, [&fired] { fired.push_back(21); });
    made_first->start(2s, [&fired] { fired.push_back(22); });
    net.advance(2s);
    BOOST_TEST((fired == std::vector<int>{5, 10, 21, 22}));
}

// The clock moves from the first advance, before any timer is made, and a timer made then starts
// from there.
void the_virtual_clock_moves_before_any_timer() {
    asio::io_context io;
    memory_network net{io};
    net.virtual_clock = true;
    BOOST_TEST(net.advance(5s));
    BOOST_TEST_EQ(net.now_ms(), 5000.0);
    const auto t = net.make_timer(std::pmr::get_default_resource());
    bool fired = false;
    t->start(1s, [&fired] { fired = true; });
    net.advance(999ms);
    BOOST_TEST(!fired);
    net.advance(1ms);
    BOOST_TEST(fired);
    BOOST_TEST_EQ(net.now_ms(), 6000.0);
}

/** Counts the bytes it has out, from the default resource. */
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t in_use = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        in_use += bytes;
        return std::pmr::get_default_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        in_use -= bytes;
        std::pmr::get_default_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

// A virtual timer restarted, cancelled or destroyed leaves the clock at once, so its
// memory, in the resource of the room or connection that made it, is back at once, not at its due
// time.
void a_virtual_timer_leaves_the_clock_at_once() {
    asio::io_context io;
    memory_network net{io};
    net.virtual_clock = true;
    counting_resource memory;
    auto t = net.make_timer(&memory);
    t->start(10s, [] {});
    t->start(5s, [] {});  // a restart replaces the first expiry
    BOOST_TEST_EQ(net.scheduled(), 1U);
    t->cancel();
    BOOST_TEST_EQ(net.scheduled(), 0U);
    t->start(10s, [] {});
    t.reset();  // destroyed while scheduled
    BOOST_TEST_EQ(net.scheduled(), 0U);
    BOOST_TEST_EQ(memory.in_use, 0U);  // before any advance
    BOOST_TEST(net.advance(20s));
}

// A timer that re-arms itself without delay would never let the clock move; advance
// stops after max_steps timers and says so, and the test fails instead of hanging.
void a_timer_rearmed_without_delay_stops_the_clock() {
    asio::io_context io;
    memory_network net{io};
    net.virtual_clock = true;
    net.max_steps = 1000;
    const auto t = net.make_timer(std::pmr::get_default_resource());
    std::size_t fired = 0;
    std::function<void()> again = [&] {
        ++fired;
        t->start(0ms, again);
    };
    t->start(0ms, again);
    BOOST_TEST(!net.advance(1s));
    BOOST_TEST_EQ(fired, 1000U);
    t->cancel();
}

// A run of exactly max_steps timers is finite: advance fires them all, reaches its end and says so.
void a_run_of_max_steps_timers_reaches_the_end() {
    asio::io_context io;
    memory_network net{io};
    net.virtual_clock = true;
    net.max_steps = 3;
    auto* m = std::pmr::get_default_resource();
    std::size_t fired = 0;
    const auto t1 = net.make_timer(m);
    const auto t2 = net.make_timer(m);
    const auto t3 = net.make_timer(m);
    t1->start(1s, [&fired] { ++fired; });
    t2->start(2s, [&fired] { ++fired; });
    t3->start(3s, [&fired] { ++fired; });
    BOOST_TEST(net.advance(5s));
    BOOST_TEST_EQ(fired, 3U);
    BOOST_TEST_EQ(net.now_ms(), 5000.0);
}

void a_buffering_channel_drains_and_says_so() {
    asio::io_context io;
    memory_network net{io};
    net.channel_capacity = 1;
    net.drain_per_post = 1024;
    pair p;
    connect(net, p);
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return p.offer_end->is_open(); })));
    bool low = false;
    p.offer_end->set_handlers({
        .open = {},
        .message = {},
        .close = {},
        .buffered_amount_low = [&low] { low = true; },
    });
    const std::vector<std::uint8_t> big(100'000, 7);
    p.offer_end->send(big);
    BOOST_TEST_EQ(p.offer_end->buffered_amount(), big.size());
    BOOST_TEST(run_until(io, [&] { return low; }));
    BOOST_TEST_LE(p.offer_end->buffered_amount(), trystero::backend::data_channel::low_threshold);
    BOOST_TEST(run_until(io, [&] { return p.at_answerer.size() == 1; }));
}

// A ready handler that reposts forever must stop the virtual harness as a zero-delay timer does.
void ready_work_is_bounded() {
    asio::io_context io;
    memory_network net(io);
    net.virtual_clock = true;
    net.max_steps = 8;
    std::size_t calls = 0;
    std::function<void()> again;
    again = [&] {
        ++calls;
        asio::post(io, again);
    };
    asio::post(io, again);
    BOOST_TEST(!net.poll_ready());
    BOOST_TEST(net.handlers_exhausted);
    BOOST_TEST_EQ(calls, net.max_steps);
}

}  // namespace

int main() {
    ids_are_sizes_and_peer_of_pairs_them();
    a_held_frame_holds_every_later_frame_of_its_end();
    a_dropped_frame_is_gone();
    deferred_opens_open_one_end_at_a_time();
    an_injected_frame_reaches_the_end_named();
    a_failed_connection_reports_failed_then_closes();
    a_disconnection_and_a_reconnection_are_reported();
    a_buffering_channel_drains_and_says_so();
    a_frame_is_delivered_once_its_last_byte_drained();
    a_closed_channel_drains_nothing_more();
    a_release_filters_each_frame_again();
    a_release_after_the_sender_went_drops_its_frames();
    a_release_after_the_sending_channel_was_replaced_drops_its_frames();
    the_virtual_clock_moves_only_when_advanced();
    the_virtual_clock_moves_before_any_timer();
    a_virtual_timer_leaves_the_clock_at_once();
    a_timer_rearmed_without_delay_stops_the_clock();
    a_run_of_max_steps_timers_reaches_the_end();
    ready_work_is_bounded();
    return boost::report_errors();
}
