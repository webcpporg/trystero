// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A room's handshakes alone, 0.27.0's handshake manager (`handshake.ts`'s
// `createHandshakeManager`): who initiates, the ready frame before the join, the join once both
// sides are ready, the 16 buffered messages, the 10 s on the virtual clock, and a clear that fails
// a waiting step. The bench records each call the manager makes of its room, and clears a failed
// peer from within the call, as a room does.

#include <webcpp/trystero/detail/handshake_manager.hpp>

#include "support/memory_backend.hpp"

#include <webcpp/asio/io_context.hpp>

#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
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
using phase = detail::handshake_manager::phase;

namespace {

/** A manager of the handshakes of `self`, on the virtual clock, and what it asked of its room. */
struct bench {
    asio::io_context io;
    trystero::test::memory_network net{io};
    // The virtual clock starts with the network's first timer: one made here, so that the clock
    // moves with advance() from the start.
    std::shared_ptr<trystero::backend::timer> clock_start;
    /** Each call of the room, in order: "steps <peer> initiator|responder", "passed <peer>",
     * "admit <peer>", "ready <peer> <json>", "activate <peer>", "failed <peer>: <reason>". */
    std::vector<std::string> calls;
    /** The steps that run, by peer: `finish` ends them. */
    std::map<std::string, trystero::handler<void(std::string_view)>, std::less<>> steps;
    std::shared_ptr<detail::handshake_manager> hs;

    explicit bench(std::string_view self = "M", bool defer_responder_ready = false) {
        net.virtual_clock = true;
        auto* const memory = std::pmr::get_default_resource();
        clock_start = net.make_timer(memory);
        hs = std::make_shared<detail::handshake_manager>(
            self, defer_responder_ready,
            detail::handshake_callbacks{
                .run_steps =
                    [this](std::string_view peer, bool initiator,
                           trystero::handler<void(std::string_view)> done) {
                        calls.push_back("steps " + std::string(peer) +
                                        (initiator ? " initiator" : " responder"));
                        steps.insert_or_assign(std::string(peer), std::move(done));
                    },
                .send_ready =
                    [this](std::string_view peer, std::string_view json) {
                        calls.push_back("ready " + std::string(peer) + " " + std::string(json));
                    },
                .local_passed =
                    [this](std::string_view peer) {
                        calls.push_back("passed " + std::string(peer));
                        return true;
                    },
                .admit =
                    [this](std::string_view peer) {
                        calls.push_back("admit " + std::string(peer));
                        return true;
                    },
                .activate =
                    [this](std::string_view peer) {
                        calls.push_back("activate " + std::string(peer));
                    },
                .failed =
                    [this](std::string_view peer, std::string_view reason) {
                        calls.push_back("failed " + std::string(peer) + ": " + std::string(reason));
                        hs->clear(peer);  // as a room lets the peer go
                    },
            },
            net, memory);
    }

    /** Adds and starts `peer`'s handshake. */
    void begin(std::string_view peer) {
        hs->add(peer);
        hs->start(peer);
    }

    /** Ends `peer`'s steps: "" passes them. */
    void finish(std::string_view peer, std::string_view reason = {}) {
        const auto it = steps.find(peer);
        webcpp::test::require(BOOST_TEST(it != steps.end()));
        auto done = std::move(it->second);
        steps.erase(it);
        done(reason);
    }

    /** How many of the calls start with `prefix`. */
    [[nodiscard]] std::size_t count(std::string_view prefix) const {
        return static_cast<std::size_t>(std::ranges::count_if(
            calls, [prefix](const std::string& c) { return c.starts_with(prefix); }));
    }

    void advance(std::chrono::milliseconds by) {
        if (!net.advance(by)) {
            BOOST_ERROR("the virtual clock fired max_steps timers without moving past them");
        }
    }
};

// The side whose id is lower initiates, in UTF-16 code unit order, as 0.27.0's `effectiveSelfId <
// id` compares its ids: "Z" is below "a", whatever the letters' case says, and U+FF21, one code
// unit, is above U+1F600, whose first unit is a surrogate, though its UTF-8 bytes are below.
void the_lower_id_initiates() {
    bench b("M");
    b.begin("A");
    b.begin("Z");
    BOOST_TEST(b.calls == (std::vector<std::string>{"steps A responder", "steps Z initiator"}));
    bench upper("Z");
    upper.begin("a");
    bench lower("a");
    lower.begin("Z");
    BOOST_TEST(upper.calls == (std::vector<std::string>{"steps a initiator"}));
    BOOST_TEST(lower.calls == (std::vector<std::string>{"steps Z responder"}));
    const std::string fullwidth = "\xef\xbc\xa1";  // U+FF21, UTF-16 FF21
    const std::string emoji = "\xf0\x9f\x98\x80";  // U+1F600, UTF-16 D83D DE00
    BOOST_TEST(fullwidth < emoji);                 // in bytes
    bench wide(fullwidth);
    wide.begin(emoji);
    bench astral(emoji);
    astral.begin(fullwidth);
    BOOST_TEST(wide.calls == (std::vector<std::string>{"steps " + emoji + " responder"}));
    BOOST_TEST(astral.calls == (std::vector<std::string>{"steps " + fullwidth + " initiator"}));
    // A peer is started once: a second start, or a start before its add, runs nothing.
    b.hs->start("A");
    b.hs->start("N");
    BOOST_TEST_EQ(b.count("steps"), 2U);
}

// With the peer's ready frame in, the steps' end sends this side's ready frame, empty, before the
// peer joins: the room's join, and what its listeners send, follow the frame.
void ready_goes_before_activate() {
    bench b;
    b.hs->add("A");
    b.hs->receive_ready("A");  // kept until the steps pass
    b.hs->start("A");
    BOOST_TEST(b.hs->phase_of("A") == phase::running_steps);
    b.finish("A");
    BOOST_TEST(b.calls == (std::vector<std::string>{"steps A responder", "passed A", "admit A",
                                                    "ready A ", "activate A"}));
    BOOST_TEST(b.hs->phase_of("A") == phase::active);
}

// Either side may be ready first: the peer joins once both are, and once only.
void both_ready_activate_once() {
    bench b;
    b.begin("A");
    b.finish("A");
    BOOST_TEST(b.hs->phase_of("A") == phase::awaiting_peer_ready);
    BOOST_TEST_EQ(b.count("ready A"), 1U);
    BOOST_TEST_EQ(b.count("activate"), 0U);
    b.hs->receive_ready("A");
    b.hs->receive_ready("A");
    BOOST_TEST_EQ(b.count("activate A"), 1U);
    BOOST_TEST(b.hs->phase_of("A") == phase::active);

    b.begin("B");
    b.hs->receive_ready("B");
    b.hs->receive_ready("B");
    BOOST_TEST_EQ(b.count("activate B"), 0U);
    b.finish("B");
    b.hs->receive_ready("B");
    BOOST_TEST_EQ(b.count("activate B"), 1U);
    BOOST_TEST_EQ(b.count("ready B"), 1U);
}

// A deferred responder, 0.27.0's deferResponderReady (a room with max_users), admits the peer and
// sends its ready frame only once the initiator's arrived; an initiator does not wait.
void a_deferred_responder_waits_for_the_initiators_ready() {
    bench b("M", true);
    b.begin("A");  // M responds to A
    b.finish("A");
    BOOST_TEST(b.hs->phase_of("A") == phase::awaiting_remote_ready);
    BOOST_TEST_EQ(b.count("admit"), 0U);
    b.hs->receive_ready("A");
    BOOST_TEST((std::vector<std::string>(b.calls.end() - 3, b.calls.end())) ==
               (std::vector<std::string>{"admit A", "ready A ", "activate A"}));
    b.begin("Z");  // M initiates with Z
    b.finish("Z");
    BOOST_TEST(b.hs->phase_of("Z") == phase::awaiting_peer_ready);
    BOOST_TEST_EQ(b.count("ready Z"), 1U);
}

// Messages wait for the steps in order, 16 at most: a 17th fails the handshake, with 0.27.0's
// reason. A message a step waits for goes to it at once, and is not buffered.
void a_17th_buffered_message_fails_the_handshake() {
    bench b;
    b.hs->add("A");
    b.hs->receive_data("A", "m0");  // before the start, kept too
    b.hs->start("A");
    for (std::size_t i = 1; i < detail::handshake_manager::max_buffered_messages; ++i) {
        b.hs->receive_data("A", "m" + std::to_string(i));
    }
    BOOST_TEST_EQ(b.count("failed"), 0U);
    std::vector<std::string> read;
    const auto next = [&read](boost::system::error_code ec, std::string_view json) {
        BOOST_TEST(!ec);
        read.emplace_back(json);
    };
    b.hs->next_message("A", next);
    b.hs->next_message("A", next);
    BOOST_TEST(read == (std::vector<std::string>{"m0", "m1"}));
    b.hs->receive_data("A", "m16");
    b.hs->receive_data("A", "m17");
    BOOST_TEST_EQ(b.count("failed"), 0U);
    b.hs->receive_data("A", "m18");
    BOOST_TEST(b.calls.back() == "failed A: too many pending handshake messages");
    BOOST_TEST(!b.hs->phase_of("A").has_value());

    bench waiting;
    waiting.begin("A");
    std::vector<std::string> got;
    waiting.hs->next_message("A", [&got](boost::system::error_code ec, std::string_view json) {
        BOOST_TEST(!ec);
        got.emplace_back(json);
    });
    for (std::size_t i = 0; i <= detail::handshake_manager::max_buffered_messages; ++i) {
        waiting.hs->receive_data("A", "w" + std::to_string(i));
    }
    BOOST_TEST(got == (std::vector<std::string>{"w0"}));
    BOOST_TEST_EQ(waiting.count("failed"), 0U);
}

// A handshake not active 10 s after its start fails, with 0.27.0's reason; one that completed, or
// was cleared, does not. A handshake failed by its steps reports their reason.
void the_timer_fails_after_10_s() {
    bench b;
    b.begin("A");
    b.begin("B");
    b.finish("B");
    b.hs->receive_ready("B");
    b.begin("C");
    b.hs->clear("C");
    b.advance(9'999ms);
    BOOST_TEST_EQ(b.count("failed"), 0U);
    b.advance(1ms);
    BOOST_TEST(b.calls.back() == "failed A: handshake timed out after 10000ms");
    b.advance(20'000ms);
    BOOST_TEST_EQ(b.count("failed"), 1U);
    BOOST_TEST(b.hs->phase_of("B") == phase::active);

    bench failing;
    failing.begin("A");
    failing.finish("A", "peer identity verification failed");
    BOOST_TEST(failing.calls.back() == "failed A: peer identity verification failed");
    failing.advance(20'000ms);
    BOOST_TEST_EQ(failing.count("failed"), 1U);
}

// Clearing a peer fails a step that waits for its message with peer_disconnected (0.27.0's
// "peer disconnected during handshake"); the steps of a cleared or replaced handshake, ending
// later, reach nothing.
void clear_fails_a_waiting_step() {
    bench b;
    b.begin("A");
    std::optional<boost::system::error_code> waited;
    b.hs->next_message("A", [&waited](boost::system::error_code ec, std::string_view json) {
        waited = ec;
        BOOST_TEST(json.empty());
    });
    BOOST_TEST(!waited.has_value());
    b.hs->clear("A");
    BOOST_TEST(waited == trystero::error::peer_disconnected);
    BOOST_TEST_EQ(detail::handshake_manager::disconnected_reason,
                  "peer disconnected during handshake");
    std::optional<boost::system::error_code> late;
    b.hs->next_message("A", [&late](boost::system::error_code ec, std::string_view) { late = ec; });
    BOOST_TEST(late == trystero::error::peer_disconnected);
    b.finish("A");
    BOOST_TEST_EQ(b.count("passed"), 0U);

    // A second add replaces the handshake: the first one's steps and timer reach nothing.
    b.begin("B");
    auto first = std::move(b.steps.at("B"));
    b.steps.erase("B");
    b.advance(5'000ms);
    b.begin("B");
    first("");
    BOOST_TEST_EQ(b.count("passed"), 0U);
    b.advance(5'000ms);
    BOOST_TEST_EQ(b.count("failed"), 0U);
    b.advance(5'000ms);
    BOOST_TEST(b.calls.back() == "failed B: handshake timed out after 10000ms");
}

}  // namespace

int main() {
    using webcpp::test::run_case;
    run_case(the_lower_id_initiates);
    run_case(ready_goes_before_activate);
    run_case(both_ready_activate_once);
    run_case(a_deferred_responder_waits_for_the_initiators_ready);
    run_case(a_17th_buffered_message_fails_the_handshake);
    run_case(the_timer_fails_after_10_s);
    run_case(clear_fails_a_waiting_step);
    // Under Asyncify, a main that suspended loses what it returns: the status goes first.
    return webcpp::test::exit_status(boost::report_errors());
}
