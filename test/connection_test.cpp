// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The shared connection alone, over a memory pair: routing by token, kept frames, presence, the
// bound on advertised rooms, malformed frames, the drain waiters, the end, the idle time and a
// disconnection, and calls back into the connection or a binding from inside its own calls.

#include <webcpp/trystero/detail/connection.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>

#include "support/memory_backend.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
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

/** Runs the ready work of `io` until there is none: the memory network's drains are posted, not
 * timed. */
void settle(asio::io_context& io) {
    io.restart();
    while (io.poll() > 0) {
        io.restart();
    }
}

/** What one side's connection reported, and what its app does from inside the calls. */
struct side {
    std::shared_ptr<detail::connection> c;
    std::vector<std::string> presence;  // "+T" or "-T"
    std::vector<std::string> kept;
    std::string ended;
    /** `keeps` ends the connection, then says yes. */
    bool end_in_keeps = false;
    /** `kept` takes the kept frames of its token, then reads the token. */
    bool take_in_kept = false;
    std::size_t taken = 0;
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
                [&s](std::string_view, std::string_view t) {
                    if (s.end_in_keeps) {
                        s.c->end("ended in keeps");
                    }
                    return t != "nobody";
                },
            },
        .kept =
            {
                std::allocator_arg,
                a,
                [&s](std::string_view, std::string_view t) {
                    if (s.take_in_kept) {
                        s.taken += s.c->take_kept(t).size();
                    }
                    s.kept.emplace_back(t);  // after the take: the token must outlive it
                },
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
    std::shared_ptr<trystero::backend::peer_connection> offerer;   // a's, id 0
    std::shared_ptr<trystero::backend::peer_connection> answerer;  // b's, id 1
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
    offerer = net.make_peer_connection({}, m);
    answerer = net.make_peer_connection({}, m);
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

/** A binding of `token` on `c`, made in `pool`, which reuses a freed block at once. */
std::shared_ptr<detail::binding> make_pooled(std::pmr::memory_resource& pool,
                                             const std::shared_ptr<detail::connection>& c,
                                             std::string_view token) {
    const std::pmr::polymorphic_allocator<> alloc(&pool);
    return std::allocate_shared<detail::binding>(
        alloc, c, token,
        trystero::handler<void(trystero::backend::bytes&&, std::span<const std::uint8_t>)>(
            std::allocator_arg, alloc,
            [](trystero::backend::bytes&&, std::span<const std::uint8_t>) {}),
        &pool);
}

/** Sends through `b` room frames the other side drops, of `payloads` bytes of action each. */
void send_dropped(detail::binding& b, std::span<const std::size_t> payloads) {
    auto* m = std::pmr::get_default_resource();
    for (const auto size : payloads) {
        const std::vector<std::uint8_t> payload(size, 0);
        BOOST_TEST(b.send(trystero::protocol::room_frame("nobody", payload, m)));
    }
}

/** Fills a buffering channel past Trystero's threshold through `b`: 5 frames of 16,009 bytes. */
void fill(detail::binding& b) {
    const std::array<std::size_t, 5> payloads{16'000, 16'000, 16'000, 16'000, 16'000};
    send_dropped(b, payloads);
}

/** A waiter that records `name`, or `!name` when told the channel closed. */
trystero::handler<void(bool)> record(std::vector<std::string>& turns, std::string name) {
    return {std::allocator_arg, std::pmr::polymorphic_allocator<>(std::pmr::get_default_resource()),
            [&turns, name = std::move(name)](bool open) {
                turns.emplace_back(open ? name : "!" + name);
            }};
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
    const std::string token(trystero::protocol::max_room_token_bytes, 'T');
    const std::vector<std::uint8_t> action(trystero::protocol::max_action_frame_bytes, 0);
    const auto frame = trystero::protocol::room_frame(token, action, m);
    BOOST_TEST_EQ(frame.size(), 16'515U);
    for (int i = 0; i < 65; ++i) {
        p.a.c->send(trystero::protocol::room_frame(token, action, m));
    }
    BOOST_TEST(run_until(p.io, [&] { return p.b.kept.size() == 64; }));
    BOOST_TEST(p.net.advance(9'999ms));
    BOOST_TEST_EQ(p.b.kept.size(), 64U);  // the 65th, beyond the bound, was not kept
    BOOST_TEST_EQ(p.b.c->kept_count(), 64U);
    BOOST_TEST(p.net.advance(1ms));  // 10 s after the first was kept
    BOOST_TEST_EQ(p.b.c->kept_count(), 0U);
    BOOST_TEST(p.b.c->take_kept(token).empty());
}

// A connection whose channel is closing or closed is not live, so the app does not reuse it.
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
// binding destroyed by an earlier turn gets none.
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
    settle(p.io);
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

// The end fails the waiters in the order they began to wait, not in the order their bindings
// attached.
void the_end_fails_the_waiters_in_the_order_of_waiting() {
    const std::array<std::string_view, 2> tokens{"T1", "T2"};
    link_pair p(tokens, tokens);
    std::vector<std::vector<std::uint8_t>> got;
    const auto t1 = make_binding(p.a.c, "T1", got);
    const auto t2 = make_binding(p.a.c, "T2", got);
    p.net.channel_capacity = 1;
    fill(*t1);
    std::vector<std::string> turns;
    t2->wait_drained(record(turns, "T2"));
    t1->wait_drained(record(turns, "T1"));
    BOOST_TEST(turns.empty());
    p.a.c->end("test");
    BOOST_TEST(turns == (std::vector<std::string>{"!T2", "!T1"}));
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

// A turn destroys a binding that waits after it and makes another in the same resource, which a
// pool gives the freed block: the new binding began to wait during this drain, so its turn is the
// next drain, once the channel has drained, never the old one's.
void a_binding_made_during_a_drain_waits_for_the_next() {
    const std::array<std::string_view, 3> tokens{"T1", "T2", "T3"};
    link_pair p(tokens, tokens);
    std::pmr::unsynchronized_pool_resource pool;
    const auto t1 = make_pooled(pool, p.a.c, "T1");
    auto t2 = make_pooled(pool, p.a.c, "T2");
    std::shared_ptr<detail::binding> t3;
    p.net.channel_capacity = 1;
    fill(*t1);
    std::vector<std::string> turns;
    std::size_t t3_buffered_at_its_turn = 0;
    const void* const t2_address = t2.get();
    bool same_address = false;
    const std::pmr::polymorphic_allocator<> alloc(std::pmr::get_default_resource());
    t1->wait_drained({std::allocator_arg, alloc, [&](bool) {
                          turns.emplace_back("T1");
                          fill(*t1);  // above the threshold again
                          t2.reset();
                          t3 = make_pooled(pool, p.a.c, "T3");
                          same_address = t3.get() == t2_address;
                          t3->wait_drained({std::allocator_arg, alloc, [&](bool) {
                                                turns.emplace_back("T3");
                                                t3_buffered_at_its_turn = t3->buffered_amount();
                                            }});
                      }});
    t2->wait_drained(record(turns, "T2"));
    BOOST_TEST(run_until(p.io, [&] { return turns.size() >= 2; }));
    BOOST_TEST(same_address);
    BOOST_TEST(turns == (std::vector<std::string>{"T1", "T3"}));
    BOOST_TEST_LE(t3_buffered_at_its_turn, trystero::backend::data_channel::low_threshold);
}

// A turn that ends the connection: every binding still waiting after it is told `false`.
void an_end_inside_a_drain_fails_the_waiters_after_it() {
    const std::array<std::string_view, 2> tokens{"T1", "T2"};
    link_pair p(tokens, tokens);
    std::vector<std::vector<std::uint8_t>> got;
    const auto t1 = make_binding(p.a.c, "T1", got);
    const auto t2 = make_binding(p.a.c, "T2", got);
    p.net.channel_capacity = 1;
    fill(*t1);
    std::vector<std::string> turns;
    const std::pmr::polymorphic_allocator<> alloc(std::pmr::get_default_resource());
    t1->wait_drained({std::allocator_arg, alloc, [&](bool open) {
                          turns.emplace_back(open ? "T1" : "!T1");
                          p.a.c->end("ended in a turn");
                      }});
    t2->wait_drained(record(turns, "T2"));
    BOOST_TEST(run_until(p.io, [&] { return !p.a.ended.empty(); }));
    settle(p.io);
    BOOST_TEST_EQ(p.a.ended, "ended in a turn");
    BOOST_TEST(turns == (std::vector<std::string>{"T1", "!T2"}));
}

// A waiter may end the connection or detach its own binding. Waiters already selected for
// that same drain must fail before the ending call returns, not receive a stale success later.
void an_end_inside_one_binding_fails_its_remaining_waiters() {
    for (const bool detach : {false, true}) {
        const std::array<std::string_view, 1> tokens{"T1"};
        link_pair p(tokens, tokens);
        std::vector<std::vector<std::uint8_t>> got;
        const auto t1 = make_binding(p.a.c, "T1", got);
        p.net.channel_capacity = 1;
        fill(*t1);
        std::vector<std::string> turns;
        const std::pmr::polymorphic_allocator<> alloc(std::pmr::get_default_resource());
        t1->wait_drained({std::allocator_arg, alloc, [&](bool open) {
                              BOOST_TEST(open);
                              turns.emplace_back("first");
                              if (detach) {
                                  t1->detach(false, 1000ms);
                              } else {
                                  p.a.c->end("ended in its own drain");
                              }
                              BOOST_TEST(turns == (std::vector<std::string>{"first", "!second"}));
                          }});
        t1->wait_drained(record(turns, "second"));
        BOOST_TEST(run_until(p.io, [&] { return turns.size() == 2; }));
        BOOST_TEST(turns == (std::vector<std::string>{"first", "!second"}));
    }
}

/** A binding of T1 on `p`'s b side whose delivery kicks the peer (`destroy` false) or drops the
 * binding (`destroy` true), then reads its own capture, as a room's delivery passes its `peer`
 * on after a kick: a use-after-free here would fail under the sanitizers. */
void a_delivery_that_lets_its_binding_go(bool destroy) {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    auto* m = std::pmr::get_default_resource();
    const std::pmr::polymorphic_allocator<> alloc(m);
    std::shared_ptr<detail::binding> bound;
    std::string seen;
    bound = std::allocate_shared<detail::binding>(
        alloc, p.b.c, "T1",
        trystero::handler<void(trystero::backend::bytes&&, std::span<const std::uint8_t>)>(
            std::allocator_arg, alloc,
            [&bound, &seen, destroy,
             peer = std::pmr::string("a peer id longer than any short string", m)](
                trystero::backend::bytes&&, std::span<const std::uint8_t>) {
                if (destroy) {
                    bound.reset();  // the room's peer state goes, and its binding with it
                } else {
                    bound->detach(true, 0ms);  // the room kicks the peer
                }
                seen = peer;
            }),
        m);
    const std::array<std::uint8_t, 2> action{2, 0};
    p.a.c->send(trystero::protocol::room_frame("T1", action, m));
    BOOST_TEST(run_until(p.io, [&] { return !seen.empty(); }));
    BOOST_TEST_EQ(seen, "a peer id longer than any short string");
    BOOST_TEST_EQ(bound == nullptr, destroy);
    BOOST_TEST_EQ(p.b.ended, destroy ? "" : "its last room kicked the peer");
}

void a_delivery_that_kicks_its_binding() {
    a_delivery_that_lets_its_binding_go(false);
}

void a_delivery_that_drops_its_binding() {
    a_delivery_that_lets_its_binding_go(true);
}

// A waiter told `false` by `detach` releases the room's last reference to the binding, as a
// transfer's failure may.
void a_waiter_that_releases_its_binding_during_detach() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    std::vector<std::vector<std::uint8_t>> got;
    auto t1 = make_binding(p.a.c, "T1", got);
    p.net.channel_capacity = 1;
    fill(*t1);
    bool told = false;
    t1->wait_drained({std::allocator_arg,
                      std::pmr::polymorphic_allocator<>(std::pmr::get_default_resource()),
                      [&](bool open) {
                          told = !open;
                          t1.reset();
                      }});
    auto* const raw = t1.get();
    raw->detach(false, 1000ms);
    BOOST_TEST(told);
    BOOST_TEST(!t1);
    BOOST_TEST(p.a.ended.empty());  // idling: the idle time is 1 s
}

// Detaching tells each of the binding's waiters `false` before it returns, and no other binding's.
void detach_fails_the_waiters_of_its_binding_alone() {
    const std::array<std::string_view, 2> tokens{"T1", "T2"};
    link_pair p(tokens, tokens);
    std::vector<std::vector<std::uint8_t>> got;
    const auto t1 = make_binding(p.a.c, "T1", got);
    const auto t2 = make_binding(p.a.c, "T2", got);
    p.net.channel_capacity = 1;
    fill(*t1);
    std::vector<std::string> turns;
    t1->wait_drained(record(turns, "a"));
    t2->wait_drained(record(turns, "T2"));
    t1->wait_drained(record(turns, "b"));
    t1->detach(false, 1000ms);
    BOOST_TEST(turns == (std::vector<std::string>{"!a", "!b"}));
    BOOST_TEST(run_until(p.io, [&] { return turns.size() == 3; }));
    BOOST_TEST_EQ(turns.back(), "T2");
}

// A detached binding is cut off: it sends nothing and its waits fail at once, while its connection
// goes on for the others.
void a_detached_binding_neither_sends_nor_waits() {
    const std::array<std::string_view, 2> tokens{"T1", "T2"};
    const link_pair p(tokens, tokens);
    std::vector<std::vector<std::uint8_t>> got;
    const auto t1 = make_binding(p.a.c, "T1", got);
    const auto t2 = make_binding(p.a.c, "T2", got);
    t1->detach(false, 1000ms);
    auto* m = std::pmr::get_default_resource();
    const std::array<std::uint8_t, 2> action{2, 0};
    BOOST_TEST(!t1->send(trystero::protocol::room_frame("T1", action, m)));
    BOOST_TEST(!t1->shared_connection());
    std::vector<std::string> turns;
    t1->wait_drained(record(turns, "T1"));
    BOOST_TEST(turns == (std::vector<std::string>{"!T1"}));
    BOOST_TEST(t2->send(trystero::protocol::room_frame("T2", action, m)));
    BOOST_TEST(p.a.c->live());
}

// A binding names its connection by identity, whether it detached and whether the connection
// ended: never another connection, nor none.
void a_binding_is_on_its_connection_alone() {
    const std::array<std::string_view, 1> tokens{"T1"};
    const link_pair p(tokens, tokens);
    std::vector<std::vector<std::uint8_t>> got;
    const auto t1 = make_binding(p.a.c, "T1", got);
    BOOST_TEST(t1->is_on(p.a.c));
    BOOST_TEST(!t1->is_on(p.b.c));
    BOOST_TEST(!t1->is_on(nullptr));
    t1->detach(false, 1000ms);
    BOOST_TEST(t1->is_on(p.a.c));
    p.a.c->end("test");
    BOOST_TEST(t1->is_on(p.a.c));
    BOOST_TEST(!t1->is_on(p.b.c));
}

// A wait with the channel at or below Trystero's threshold completes at once; one byte above
// waits for the drain.
void a_wait_at_the_threshold_completes_at_once() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    std::vector<std::vector<std::uint8_t>> got;
    const auto t1 = make_binding(p.a.c, "T1", got);
    p.net.channel_capacity = 1;
    std::vector<std::string> turns;
    t1->wait_drained(record(turns, "empty"));
    BOOST_TEST(turns == (std::vector<std::string>{"empty"}));
    // 4 x 16,009 + 1,499: 65,535 bytes, the threshold itself.
    const std::array<std::size_t, 5> payloads{16'000, 16'000, 16'000, 16'000, 1'490};
    send_dropped(*t1, payloads);
    webcpp::test::require(
        BOOST_TEST_EQ(t1->buffered_amount(), trystero::backend::data_channel::low_threshold));
    t1->wait_drained(record(turns, "at"));
    BOOST_TEST(turns == (std::vector<std::string>{"empty", "at"}));
    const std::array<std::size_t, 1> one_more{0};  // a frame of 9 bytes
    send_dropped(*t1, one_more);
    t1->wait_drained(record(turns, "above"));
    BOOST_TEST_EQ(turns.size(), 2U);
    BOOST_TEST(run_until(p.io, [&] { return turns.size() == 3; }));
    BOOST_TEST_EQ(turns.back(), "above");
}

// A binding has one place in a drain however many times it waits: a waiter that waits again from
// its turn completes at the next drain, not in this one.
void a_binding_has_one_turn_per_drain() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    std::vector<std::vector<std::uint8_t>> got;
    const auto t1 = make_binding(p.a.c, "T1", got);
    p.net.channel_capacity = 1;
    fill(*t1);
    std::vector<std::string> turns;
    std::size_t buffered_at_rewait = 0;
    const std::pmr::polymorphic_allocator<> alloc(std::pmr::get_default_resource());
    t1->wait_drained({std::allocator_arg, alloc, [&](bool) {
                          turns.emplace_back("a");
                          fill(*t1);
                          t1->wait_drained({std::allocator_arg, alloc, [&](bool) {
                                                turns.emplace_back("again");
                                                buffered_at_rewait = t1->buffered_amount();
                                            }});
                      }});
    t1->wait_drained(record(turns, "b"));
    BOOST_TEST(run_until(p.io, [&] { return turns.size() == 3; }));
    BOOST_TEST(turns == (std::vector<std::string>{"a", "b", "again"}));
    BOOST_TEST_LE(buffered_at_rewait, trystero::backend::data_channel::low_threshold);
}

// Taking the last kept frame stops the window: a frame kept later has 10 s of its own.
void taking_every_kept_frame_restarts_the_window() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    auto* m = std::pmr::get_default_resource();
    const std::array<std::uint8_t, 2> action{2, 0};
    p.a.c->send(trystero::protocol::room_frame("T2", action, m));
    webcpp::test::require(BOOST_TEST(run_until(p.io, [&] { return p.b.kept.size() == 1; })));
    BOOST_TEST(p.net.advance(5'000ms));
    BOOST_TEST_EQ(p.b.c->take_kept("T2").size(), 1U);
    BOOST_TEST(p.net.advance(1'000ms));
    p.a.c->send(trystero::protocol::room_frame("T2", action, m));
    webcpp::test::require(BOOST_TEST(run_until(p.io, [&] { return p.b.kept.size() == 2; })));
    BOOST_TEST(p.net.advance(9'999ms));  // 15.999 s
    BOOST_TEST_EQ(p.b.c->kept_count(), 1U);
    BOOST_TEST(p.net.advance(1ms));  // 16 s, 10 s after the second was kept
    BOOST_TEST_EQ(p.b.c->kept_count(), 0U);
}

// A withdrawal that leaves nothing kept stops the window too, as Trystero's syncDataTimers does
// (C/shared-peer.mjs:417-422, 468-472).
void a_withdrawal_that_empties_the_kept_frames_restarts_the_window() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    auto* m = std::pmr::get_default_resource();
    const std::array<std::uint8_t, 2> action{2, 0};
    p.a.c->send(trystero::protocol::room_frame("T2", action, m));
    webcpp::test::require(BOOST_TEST(run_until(p.io, [&] { return p.b.kept.size() == 1; })));
    BOOST_TEST(p.net.advance(5'000ms));
    p.a.c->send_presence("T2", false);
    webcpp::test::require(BOOST_TEST(run_until(p.io, [&] { return p.b.presence.size() == 2; })));
    BOOST_TEST_EQ(p.b.c->kept_count(), 0U);
    BOOST_TEST(p.net.advance(1'000ms));
    p.a.c->send(trystero::protocol::room_frame("T3", action, m));
    webcpp::test::require(BOOST_TEST(run_until(p.io, [&] { return p.b.kept.size() == 2; })));
    BOOST_TEST(p.net.advance(9'999ms));  // 15.999 s
    BOOST_TEST_EQ(p.b.c->kept_count(), 1U);
    BOOST_TEST(p.net.advance(1ms));
    BOOST_TEST_EQ(p.b.c->kept_count(), 0U);
}

// `keeps` that ends the connection: nothing is kept, and no timer is made.
void keeps_that_ends_the_connection_keeps_nothing() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    p.b.end_in_keeps = true;
    auto* m = std::pmr::get_default_resource();
    const std::array<std::uint8_t, 2> action{2, 0};
    p.a.c->send(trystero::protocol::room_frame("T2", action, m));
    BOOST_TEST(run_until(p.io, [&] { return !p.b.ended.empty(); }));
    BOOST_TEST_EQ(p.b.ended, "ended in keeps");
    BOOST_TEST(p.b.kept.empty());
    BOOST_TEST_EQ(p.b.c->kept_count(), 0U);
    BOOST_TEST_EQ(p.net.scheduled(), 0U);
}

// `kept` may take the frame it is told of: the token it was given outlives the take (under the
// sanitizers).
void kept_may_take_its_frame() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    p.b.take_in_kept = true;
    auto* m = std::pmr::get_default_resource();
    const std::array<std::uint8_t, 2> action{2, 0};
    const std::string token = "a room token longer than any short string";
    p.a.c->send(trystero::protocol::room_frame(token, action, m));
    BOOST_TEST(run_until(p.io, [&] { return p.b.kept.size() == 1; }));
    BOOST_TEST_EQ(p.b.kept.at(0), token);
    BOOST_TEST_EQ(p.b.taken, 1U);
    BOOST_TEST_EQ(p.b.c->kept_count(), 0U);
}

// The end closes the peer connection as well as the channel, and frees what was kept.
void the_end_closes_the_connection_and_frees_the_kept_frames() {
    const std::array<std::string_view, 1> tokens{"T1"};
    link_pair p(tokens, tokens);
    auto* m = std::pmr::get_default_resource();
    const std::array<std::uint8_t, 2> action{2, 0};
    p.a.c->send(trystero::protocol::room_frame("T2", action, m));
    webcpp::test::require(BOOST_TEST(run_until(p.io, [&] { return p.b.kept.size() == 1; })));
    p.b.c->end("test");
    BOOST_TEST_EQ(p.b.c->kept_count(), 0U);
    BOOST_TEST(!p.b.c->live());
    // A closed peer connection refuses a description.
    std::optional<trystero::backend::error_code> refused;
    p.answerer->set_local_description([&refused](trystero::backend::error_code ec, std::string_view,
                                                 std::string_view) { refused = ec; });
    BOOST_TEST(run_until(p.io, [&] { return refused.has_value(); }));
    BOOST_TEST(refused && *refused == make_error_code(trystero::error::backend_failure));
}

// A disconnection ends the connection after 5 s, as Trystero's; a reconnection before then
// cancels it.
void a_disconnection_ends_after_5_s_unless_it_reconnects() {
    const std::array<std::string_view, 1> tokens{"T1"};
    {
        link_pair p(tokens, tokens);
        p.net.disconnect(0);
        BOOST_TEST(p.net.advance(4'999ms));
        BOOST_TEST(p.a.ended.empty());
        BOOST_TEST(p.net.advance(1ms));
        BOOST_TEST_EQ(p.a.ended, "connection lost");
    }
    {
        link_pair p(tokens, tokens);
        p.net.disconnect(0);
        BOOST_TEST(p.net.advance(4'000ms));
        p.net.reconnect(0);
        BOOST_TEST(p.net.advance(10'000ms));
        BOOST_TEST(p.a.ended.empty());
        BOOST_TEST(p.a.c->live());
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
    the_end_fails_the_waiters_in_the_order_of_waiting();
    the_last_binding_ends_or_idles_the_connection();
    a_binding_made_during_a_drain_waits_for_the_next();
    an_end_inside_a_drain_fails_the_waiters_after_it();
    an_end_inside_one_binding_fails_its_remaining_waiters();
    a_delivery_that_kicks_its_binding();
    a_delivery_that_drops_its_binding();
    a_waiter_that_releases_its_binding_during_detach();
    detach_fails_the_waiters_of_its_binding_alone();
    a_detached_binding_neither_sends_nor_waits();
    a_binding_is_on_its_connection_alone();
    a_wait_at_the_threshold_completes_at_once();
    a_binding_has_one_turn_per_drain();
    taking_every_kept_frame_restarts_the_window();
    a_withdrawal_that_empties_the_kept_frames_restarts_the_window();
    keeps_that_ends_the_connection_keeps_nothing();
    kept_may_take_its_frame();
    the_end_closes_the_connection_and_frees_the_kept_frames();
    a_disconnection_ends_after_5_s_unless_it_reconnects();
    return boost::report_errors();
}
