// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The shared connection alone, over a memory pair: routing by token, kept frames, presence, the
// bound on advertised rooms, malformed frames, and the order of the drain waiters.

#include <webcpp/trystero/detail/connection.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>

#include "support/memory_backend.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "require.hpp"

namespace asio = boost::asio;
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

/** What one side's connection reported. */
struct side {
    std::shared_ptr<detail::connection> c;
    std::vector<std::string> presence;  // "+T" or "-T"
    std::vector<std::string> kept;
    std::string ended;
};

detail::connection_callbacks callbacks(side& s) {
    auto* m = std::pmr::get_default_resource();
    const std::pmr::polymorphic_allocator<> a(m);
    return {
        .presence =
            {
                std::allocator_arg,
                a,
                [&s](std::string_view, std::string_view t, bool present) {
                    s.presence.push_back((present ? "+" : "-") + std::string(t));
                },
            },
        .keeps =
            {
                std::allocator_arg,
                a,
                [](std::string_view, std::string_view t) { return t != "nobody"; },
            },
        .kept =
            {
                std::allocator_arg,
                a,
                [&s](std::string_view, std::string_view t) { s.kept.emplace_back(t); },
            },
        .ended =
            {
                std::allocator_arg,
                a,
                [&s](std::string_view, std::string_view why) { s.ended = why; },
            },
    };
}

/** Two connections over a memory pair, each started with the tokens given. */
struct link_pair {
    asio::io_context io;
    trystero::test::memory_network net{io};
    side a;
    side b;

    link_pair(std::span<const std::string_view> a_tokens,
              std::span<const std::string_view> b_tokens);
};

link_pair::link_pair(std::span<const std::string_view> a_tokens,
                     std::span<const std::string_view> b_tokens) {
    net.virtual_clock = true;  // the keep timer waits for advance()
    auto* m = std::pmr::get_default_resource();
    const std::pmr::polymorphic_allocator<> alloc(m);
    const auto offerer = net.make_peer_connection({}, m);
    const auto answerer = net.make_peer_connection({}, m);
    auto offer_end = offerer->create_data_channel("data");
    std::shared_ptr<trystero::backend::data_channel> answer_end;
    answerer->set_handlers({
        .ice_candidate = {},
        .data_channel =
            [&answer_end](std::shared_ptr<trystero::backend::data_channel> c) {
                answer_end = std::move(c);
            },
        .state = {},
    });
    std::pmr::string offer(m);
    std::pmr::string answer(m);
    offerer->set_local_description([&offer](trystero::backend::error_code, std::string_view,
                                            std::string_view sdp) { offer = sdp; });
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return !offer.empty(); })));
    answerer->set_remote_description("offer", offer, [](trystero::backend::error_code) {});
    answerer->set_local_description([&answer](trystero::backend::error_code, std::string_view,
                                              std::string_view sdp) { answer = sdp; });
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return !answer.empty(); })));
    offerer->set_remote_description("answer", answer, [](trystero::backend::error_code) {});
    webcpp::test::require(BOOST_TEST(run_until(
        io, [&] { return offer_end->is_open() && answer_end && answer_end->is_open(); })));
    a.c = std::allocate_shared<detail::connection>(alloc, net, "B", offerer, offer_end,
                                                   callbacks(a), detail::logger{}, m);
    b.c = std::allocate_shared<detail::connection>(alloc, net, "A", answerer, answer_end,
                                                   callbacks(b), detail::logger{}, m);
    a.c->start(a_tokens);
    b.c->start(b_tokens);
}

/** A binding of `token` on `c`, which records the action of every frame delivered to it. */
std::shared_ptr<detail::binding> make_binding(const std::shared_ptr<detail::connection>& c,
                                              std::string_view token,
                                              std::vector<std::vector<std::uint8_t>>& got) {
    auto* m = std::pmr::get_default_resource();
    const std::pmr::polymorphic_allocator<> alloc(m);
    return std::allocate_shared<detail::binding>(
        alloc, c, token,
        trystero::handler<void(trystero::backend::bytes&&, std::span<const std::uint8_t>)>(
            std::allocator_arg, alloc,
            [&got](trystero::backend::bytes&&, std::span<const std::uint8_t> action) {
                got.emplace_back(action.begin(), action.end());
            }),
        m);
}

void start_advertises_every_token() {
    const std::array<std::string_view, 2> a_tokens{"T1", "T2"};
    const std::array<std::string_view, 1> b_tokens{"T1"};
    link_pair p(a_tokens, b_tokens);
    BOOST_TEST(
        run_until(p.io, [&] { return p.b.presence.size() == 2 && p.a.presence.size() == 1; }));
    BOOST_TEST(p.b.presence == (std::vector<std::string>{"+T1", "+T2"}));
    BOOST_TEST(p.a.presence == (std::vector<std::string>{"+T1"}));
    BOOST_TEST(p.b.c->advertised("T1") && p.b.c->advertised("T2") && !p.b.c->advertised("T3"));
    BOOST_TEST(p.a.c->advertised("T1") && !p.a.c->advertised("T2"));
}

void a_withdrawn_token_is_forgotten() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    webcpp::test::require(BOOST_TEST(run_until(p.io, [&] { return p.b.c->advertised("T1"); })));
    p.a.c->send_presence("T1", false);
    BOOST_TEST(run_until(p.io, [&] { return !p.b.c->advertised("T1"); }));
    BOOST_TEST(p.b.presence == (std::vector<std::string>{"+T1", "-T1"}));
}

void a_bound_token_reaches_its_binding_and_another_is_kept() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    std::vector<std::vector<std::uint8_t>> got;
    const auto bound = make_binding(p.b.c, "T1", got);
    auto* m = std::pmr::get_default_resource();
    const std::array<std::uint8_t, 2> action{2, 0};
    p.a.c->send(trystero::protocol::room_frame("T1", action, m));
    p.a.c->send(trystero::protocol::room_frame("T2", action, m));
    p.a.c->send(trystero::protocol::room_frame("nobody", action, m));
    BOOST_TEST(run_until(p.io, [&] { return got.size() == 1 && p.b.kept.size() == 1; }));
    BOOST_TEST(got.at(0) == (std::vector<std::uint8_t>{2, 0}));
    BOOST_TEST_EQ(p.b.kept.at(0), "T2");
    BOOST_TEST_EQ(p.b.c->kept_count(), 1U);
    BOOST_TEST_EQ(p.b.c->take_kept("T2").size(), 1U);
    BOOST_TEST_EQ(p.b.c->kept_count(), 0U);
    BOOST_TEST(p.b.c->take_kept("nobody").empty());
}

void a_withdrawn_token_drops_its_kept_frames() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    auto* m = std::pmr::get_default_resource();
    const std::array<std::uint8_t, 2> action{2, 0};
    p.a.c->send(trystero::protocol::room_frame("T2", action, m));
    p.a.c->send(trystero::protocol::room_frame("T3", action, m));
    webcpp::test::require(BOOST_TEST(run_until(p.io, [&] { return p.b.kept.size() == 2; })));
    p.a.c->send_presence("T2", false);
    BOOST_TEST(run_until(p.io, [&] { return p.b.presence.size() == 2; }));
    BOOST_TEST_EQ(p.b.c->kept_count(), 1U);
    BOOST_TEST(p.b.c->take_kept("T2").empty());
    BOOST_TEST_EQ(p.b.c->take_kept("T3").size(), 1U);
}

void a_65th_advertised_token_ends_the_connection() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    auto* m = std::pmr::get_default_resource();
    // T1 and 63 more: 64 tokens, the most a remote may advertise.
    for (int i = 0; i < 63; ++i) {
        p.a.c->send(trystero::protocol::presence_frame("X" + std::to_string(i), true, m));
    }
    webcpp::test::require(BOOST_TEST(run_until(p.io, [&] { return p.b.presence.size() == 64; })));
    BOOST_TEST(p.b.ended.empty());
    p.a.c->send(trystero::protocol::presence_frame("X0", true, m));  // known: not a new token
    p.a.c->send(trystero::protocol::presence_frame("X63", true, m));
    BOOST_TEST(run_until(p.io, [&] { return !p.b.ended.empty(); }));
    BOOST_TEST_EQ(p.b.ended, "too many advertised rooms");
    BOOST_TEST_EQ(p.b.presence.size(), 65U);
    BOOST_TEST(!p.b.c->live());
    BOOST_TEST(run_until(p.io, [&] { return p.a.ended == "data channel closed"; }));
}

void a_malformed_frame_ends_the_connection() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    const std::array<std::uint8_t, 4> bad{3, 0, 1, 'x'};
    p.a.c->send(bad);
    BOOST_TEST(run_until(p.io, [&] { return !p.b.ended.empty(); }));
    BOOST_TEST_EQ(p.b.ended, "invalid or incompatible room frame");
}

void kept_frames_are_bounded_and_dropped_after_10_s() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    auto* m = std::pmr::get_default_resource();
    const std::array<std::uint8_t, 2> action{2, 0};
    for (int i = 0; i < 65; ++i) {
        p.a.c->send(trystero::protocol::room_frame("T2", action, m));
    }
    BOOST_TEST(run_until(p.io, [&] { return p.b.kept.size() == 64; }));
    BOOST_TEST(p.net.advance(9'999ms));
    BOOST_TEST_EQ(p.b.kept.size(), 64U);  // the 65th, beyond the bound, was not kept
    BOOST_TEST_EQ(p.b.c->kept_count(), 64U);
    BOOST_TEST(p.net.advance(1ms));  // 10 s after the first was kept
    BOOST_TEST_EQ(p.b.c->kept_count(), 0U);
    BOOST_TEST(p.b.c->take_kept("T2").empty());
}

// A connection whose channel is closing or closed is not live, so the app does not reuse it
// (review M6).
void a_closing_channel_is_not_live() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    BOOST_TEST(p.a.c->live());
    p.net.fail(p.net.peer_of(0));
    BOOST_TEST(run_until(p.io, [&] { return !p.a.c->live(); }));
    BOOST_TEST(run_until(p.io, [&] { return !p.a.ended.empty() && !p.b.ended.empty(); }));
    BOOST_TEST_EQ(p.b.ended, "connection failed");
}

// The bindings waiting at one drain each get their turn, in the order they began to wait, and a
// binding destroyed by an earlier turn gets none (review M5).
void drain_turns_follow_the_order_of_waiting() {
    const std::array<std::string_view, 3> tokens{"T1", "T2", "T3"};
    link_pair p(tokens, tokens);
    webcpp::test::require(BOOST_TEST(run_until(p.io, [&] { return p.b.presence.size() == 3; })));
    std::vector<std::vector<std::uint8_t>> got;
    const auto t1 = make_binding(p.a.c, "T1", got);
    const auto t2 = make_binding(p.a.c, "T2", got);
    auto t3 = make_binding(p.a.c, "T3", got);
    p.net.channel_capacity = 1;
    auto* m = std::pmr::get_default_resource();
    // Frames the other side drops, past the threshold: 5 x 16,000 bytes.
    const std::vector<std::uint8_t> payload(16'000, 0);
    for (int i = 0; i < 5; ++i) {
        BOOST_TEST(t1->send(trystero::protocol::room_frame("nobody", payload, m)));
    }
    webcpp::test::require(
        BOOST_TEST_GT(t1->buffered_amount(), trystero::backend::data_channel::low_threshold));
    std::vector<std::string> turns;
    const std::pmr::polymorphic_allocator<> alloc(m);
    t2->wait_drained({std::allocator_arg, alloc,
                      [&turns](bool open) { turns.emplace_back(open ? "T2" : "!T2"); }});
    t1->wait_drained({std::allocator_arg, alloc, [&turns, &t3](bool open) {
                          turns.emplace_back(open ? "T1" : "!T1");
                          t3.reset();
                      }});
    t3->wait_drained({std::allocator_arg, alloc,
                      [&turns](bool open) { turns.emplace_back(open ? "T3" : "!T3"); }});
    t2->wait_drained({std::allocator_arg, alloc,
                      [&turns](bool open) { turns.emplace_back(open ? "T2'" : "!T2'"); }});
    BOOST_TEST(run_until(p.io, [&] { return turns.size() >= 3; }));
    run_until(p.io, [] { return false; }, 50ms);
    BOOST_TEST(turns == (std::vector<std::string>{"T2", "T2'", "T1"}));
    BOOST_TEST(!t3);
    BOOST_TEST(p.a.ended.empty() && p.b.ended.empty());
}

// A connection's end fails every waiting binding's drain, once.
void the_end_fails_every_drain_waiter() {
    const std::array<std::string_view, 2> tokens{"T1", "T2"};
    link_pair p(tokens, tokens);
    std::vector<std::vector<std::uint8_t>> got;
    const auto t1 = make_binding(p.a.c, "T1", got);
    const auto t2 = make_binding(p.a.c, "T2", got);
    p.net.channel_capacity = 1;
    auto* m = std::pmr::get_default_resource();
    const std::vector<std::uint8_t> payload(16'000, 0);
    for (int i = 0; i < 5; ++i) {
        t1->send(trystero::protocol::room_frame("nobody", payload, m));
    }
    std::vector<std::string> turns;
    const std::pmr::polymorphic_allocator<> alloc(m);
    t1->wait_drained({std::allocator_arg, alloc,
                      [&turns](bool open) { turns.emplace_back(open ? "T1" : "!T1"); }});
    t2->wait_drained({std::allocator_arg, alloc,
                      [&turns](bool open) { turns.emplace_back(open ? "T2" : "!T2"); }});
    p.a.c->end("test");
    BOOST_TEST(turns == (std::vector<std::string>{"!T1", "!T2"}));
    BOOST_TEST_EQ(p.a.ended, "test");
    BOOST_TEST(!t1->send(trystero::protocol::room_frame("T1", payload, m)));
    t1->wait_drained({std::allocator_arg, alloc,
                      [&turns](bool open) { turns.emplace_back(open ? "T1" : "!T1"); }});
    BOOST_TEST_EQ(turns.size(), 3U);
    BOOST_TEST_EQ(turns.back(), "!T1");
}

// The last binding to detach ends the connection after a kick, or with no idle time; otherwise
// after the idle time, which a new binding cancels.
void the_last_binding_ends_or_idles_the_connection() {
    const std::array<std::string_view, 1> tokens{"T1"};
    std::vector<std::vector<std::uint8_t>> got;
    {
        const link_pair p(tokens, tokens);
        const auto t1 = make_binding(p.a.c, "T1", got);
        const auto t2 = make_binding(p.a.c, "T2", got);
        t1->detach(true, 1000ms);
        BOOST_TEST(p.a.ended.empty());  // T2 is still bound
        t2->detach(true, 1000ms);
        BOOST_TEST_EQ(p.a.ended, "its last room kicked the peer");
    }
    {
        const link_pair p(tokens, tokens);
        const auto t1 = make_binding(p.a.c, "T1", got);
        t1->detach(false, 0ms);
        BOOST_TEST_EQ(p.a.ended, "no room left");
    }
    {
        link_pair p(tokens, tokens);
        const auto t1 = make_binding(p.a.c, "T1", got);
        t1->detach(false, 1000ms);
        BOOST_TEST(p.net.advance(999ms));
        const auto t2 = make_binding(p.a.c, "T2", got);  // cancels the idle timer
        BOOST_TEST(p.net.advance(10'000ms));
        BOOST_TEST(p.a.ended.empty());
        t2->detach(false, 1000ms);
        BOOST_TEST(p.net.advance(999ms));
        BOOST_TEST(p.a.ended.empty());
        BOOST_TEST(p.net.advance(1ms));
        BOOST_TEST_EQ(p.a.ended, "idle");
    }
}

}  // namespace

int main() {
    start_advertises_every_token();
    a_withdrawn_token_is_forgotten();
    a_bound_token_reaches_its_binding_and_another_is_kept();
    a_withdrawn_token_drops_its_kept_frames();
    a_65th_advertised_token_ends_the_connection();
    a_malformed_frame_ends_the_connection();
    kept_frames_are_bounded_and_dropped_after_10_s();
    a_closing_channel_is_not_live();
    drain_turns_follow_the_order_of_waiting();
    the_end_fails_every_drain_waiter();
    the_last_binding_ends_or_idles_the_connection();
    return boost::report_errors();
}
