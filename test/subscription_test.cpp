// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the listeners of an event alone, as Trystero 0.27.0's event emitter (emitter.ts) runs
// them: in the order added, a listener added while an event is reported waits for the next one, a
// listener removed before its turn does not run, `once` removes before it runs, `replay` calls at
// once with what is current, and dropping a subscription leaves its listener.

#include <webcpp/trystero/subscription.hpp>

#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <array>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cases.hpp"

namespace trystero = webcpp::trystero;

namespace {

using event = trystero::detail::listeners<void(int)>;

event make_event() {
    return event(std::pmr::polymorphic_allocator<>(std::pmr::new_delete_resource()));
}

void listeners_run_in_order() {
    auto e = make_event();
    std::vector<std::string> calls;
    e.add([&](int v) { calls.push_back("a" + std::to_string(v)); }, {});
    e.add([&](int v) { calls.push_back("b" + std::to_string(v)); }, {});
    e.add([&](int v) { calls.push_back("c" + std::to_string(v)); }, {});
    e.emit(1);
    e.emit(2);
    const std::vector<std::string> expected{"a1", "b1", "c1", "a2", "b2", "c2"};
    BOOST_TEST_ALL_EQ(calls.begin(), calls.end(), expected.begin(), expected.end());
    BOOST_TEST_EQ(e.size(), 3U);
}

// The scenario of the specification: the first of two join listeners unsubscribes itself and the
// second while it runs; the second does not run, and neither runs again.
void listeners_unsubscribe_while_dispatching() {
    auto e = make_event();
    std::vector<std::string> calls;
    trystero::subscription first;
    trystero::subscription second;
    first = e.add(
        [&](int) {
            calls.emplace_back("first");
            first.unsubscribe();
            second.unsubscribe();
        },
        {});
    second = e.add([&](int) { calls.emplace_back("second"); }, {});
    const auto third = e.add([&](int) { calls.emplace_back("third"); }, {});
    BOOST_TEST(first.active());
    BOOST_TEST(second.active());
    e.emit(1);
    e.emit(2);
    const std::vector<std::string> expected{"first", "third", "third"};
    BOOST_TEST_ALL_EQ(calls.begin(), calls.end(), expected.begin(), expected.end());
    BOOST_TEST(!first.active());
    BOOST_TEST(!second.active());
    BOOST_TEST(third.active());
    BOOST_TEST_EQ(e.size(), 1U);
    // A second unsubscribe, and one of a subscription of no listener, do nothing.
    first.unsubscribe();
    trystero::subscription().unsubscribe();
}

// `once` removes the listener before it runs: an event it reports itself does not reach it again.
void once_runs_once() {
    auto e = make_event();
    std::vector<int> calls;
    std::optional<trystero::subscription> s;
    s = e.add(
        [&](int v) {
            calls.push_back(v);
            if (v == 1) {
                e.emit(10);  // reported from within: the listener is already gone
            }
        },
        {.once = true, .replay = false});
    BOOST_TEST(s->active());
    e.emit(1);
    e.emit(2);
    const std::vector<int> expected{1};
    BOOST_TEST_ALL_EQ(calls.begin(), calls.end(), expected.begin(), expected.end());
    BOOST_TEST(!s->active());
    BOOST_TEST(e.empty());
}

// A listener added while an event is reported runs from the next one on.
void added_while_dispatching_waits() {
    auto e = make_event();
    std::vector<std::string> calls;
    bool added = false;
    e.add(
        [&](int v) {
            calls.push_back("outer" + std::to_string(v));
            if (!added) {
                added = true;
                e.add([&](int w) { calls.push_back("inner" + std::to_string(w)); }, {});
            }
        },
        {});
    e.emit(1);
    e.emit(2);
    const std::vector<std::string> expected{"outer1", "outer2", "inner2"};
    BOOST_TEST_ALL_EQ(calls.begin(), calls.end(), expected.begin(), expected.end());
}

// A subscription destroyed, or moved from, leaves its listener in place, as dropping the function
// Trystero returns does; the one it was moved to removes it.
void dropping_a_subscription_keeps_the_listener() {
    auto e = make_event();
    int calls = 0;
    {
        const auto dropped = e.add([&](int) { ++calls; }, {});
    }
    static_cast<void>(e.add([&](int) { ++calls; }, {}));
    e.emit(1);
    BOOST_TEST_EQ(calls, 2);
    auto moved_from = e.add([&](int) { ++calls; }, {});
    trystero::subscription moved_to = std::move(moved_from);
    BOOST_TEST(!moved_from.active());  // NOLINT(bugprone-use-after-move): refers to none
    BOOST_TEST(moved_to.active());
    e.emit(2);
    BOOST_TEST_EQ(calls, 5);
    moved_to.unsubscribe();
    e.emit(3);
    BOOST_TEST_EQ(calls, 7);
}

// `replay` calls the listener with each current item within `add`, before it is in place; with
// `once`, only with the first; an event reported meanwhile does not reach it.
void replay_reports_what_is_current() {
    auto e = make_event();
    const std::array<int, 3> current{1, 2, 3};
    std::vector<int> replayed;
    const auto all =
        e.add([&](int v) { replayed.push_back(v); }, {.once = false, .replay = true}, current);
    const std::vector<int> expected{1, 2, 3};
    BOOST_TEST_ALL_EQ(replayed.begin(), replayed.end(), expected.begin(), expected.end());
    BOOST_TEST(all.active());
    std::vector<int> first_only;
    const auto once =
        e.add([&](int v) { first_only.push_back(v); }, {.once = true, .replay = true}, current);
    BOOST_TEST_EQ(first_only.size(), 1U);
    BOOST_TEST(!once.active());
    // An event reported during the replay does not reach the listener, not yet in place.
    std::vector<int> during;
    const auto nested = e.add(
        [&](int v) {
            during.push_back(v);
            if (v == 1) {
                e.emit(100);
            }
        },
        {.once = false, .replay = true}, current);
    BOOST_TEST_ALL_EQ(during.begin(), during.end(), expected.begin(), expected.end());
    BOOST_TEST(nested.active());
    BOOST_TEST_EQ(e.size(), 2U);
    // The first listener, in place, saw the event reported during the replay.
    const std::vector<int> with_nested{1, 2, 3, 100};
    BOOST_TEST_ALL_EQ(replayed.begin(), replayed.end(), with_nested.begin(), with_nested.end());
    // Without `replay` the items are not read.
    std::vector<int> none;
    static_cast<void>(
        e.add([&](int v) { none.push_back(v); }, {.once = false, .replay = false}, current));
    BOOST_TEST(none.empty());
}

// A listener taken one at a time answers through `call_first`; `once` removes it first.
void call_first_answers_once() {
    trystero::detail::listeners<bool(int)> gate{
        std::pmr::polymorphic_allocator<>(std::pmr::new_delete_resource())};
    const auto s = gate.add([](int v) { return v > 1; }, {.once = true, .replay = false});
    BOOST_TEST(!gate.empty());
    BOOST_TEST(gate.call_first(2));
    BOOST_TEST(gate.empty());
    BOOST_TEST(!s.active());
}

// Clearing, as a room does when it is left, removes every listener, even within an emission: those
// whose turn had not come do not run, and every subscription is inactive.
void clearing_removes_every_listener() {
    auto e = make_event();
    std::vector<std::string> calls;
    const auto first = e.add(
        [&](int) {
            calls.emplace_back("first");
            e.clear();
        },
        {});
    const auto second = e.add([&](int) { calls.emplace_back("second"); }, {});
    e.emit(1);
    e.emit(2);
    const std::vector<std::string> expected{"first"};
    BOOST_TEST_ALL_EQ(calls.begin(), calls.end(), expected.begin(), expected.end());
    BOOST_TEST(e.empty());
    BOOST_TEST(!first.active());
    BOOST_TEST(!second.active());
    const auto later = e.add([&](int) { calls.emplace_back("later"); }, {});
    e.emit(3);
    BOOST_TEST_EQ(calls.back(), "later");
    BOOST_TEST(later.active());

    // A clear during a replay removes the listener being added, which replays no further.
    std::size_t replayed = 0;
    const std::array<int, 2> current{1, 2};
    const auto replaying = e.add(
        [&](int) {
            ++replayed;
            e.clear();
        },
        {.once = false, .replay = true}, current);
    BOOST_TEST_EQ(replayed, 1U);
    BOOST_TEST(!replaying.active());
    BOOST_TEST(e.empty());
}

#ifndef WEBCPP_TRYSTERO_NO_EXCEPTIONS
// A listener that throws ends the emission, which still forgets the listeners removed meanwhile:
// what a removed listener holds is released, as after an emission that ends normally.
void a_throwing_listener_ends_the_emission_cleanly() {
    auto e = make_event();
    const auto held = std::make_shared<int>(0);
    auto removed = e.add([held](int) {}, {});
    // It removes the first listener, then throws.
    e.add(
        [&](int v) {
            if (v == 1) {
                removed.unsubscribe();
                throw std::runtime_error("listener failed");
            }
        },
        {});
    bool thrown = false;
    try {
        e.emit(1);
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    BOOST_TEST(thrown);
    BOOST_TEST_EQ(held.use_count(), 1);  // the removed listener was forgotten
    std::size_t runs = 0;
    e.add([&](int) { ++runs; }, {});
    e.emit(2);
    BOOST_TEST_EQ(runs, 1U);
    BOOST_TEST_EQ(e.size(), 2U);
}
#endif

}  // namespace

int main() {
    // Each case in a frame of its own, which main's never holds (cases.hpp).
    using webcpp::test::run_case;
    run_case(listeners_run_in_order);
    run_case(listeners_unsubscribe_while_dispatching);
    run_case(once_runs_once);
    run_case(added_while_dispatching_waits);
    run_case(dropping_a_subscription_keeps_the_listener);
    run_case(replay_reports_what_is_current);
    run_case(call_first_answers_once);
    run_case(clearing_removes_every_listener);
#ifndef WEBCPP_TRYSTERO_NO_EXCEPTIONS
    run_case(a_throwing_listener_ends_the_emission_cleanly);
#endif
    return webcpp::test::exit_status(boost::report_errors());
}
