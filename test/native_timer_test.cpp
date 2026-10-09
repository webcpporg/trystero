// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the native timer, Asio's steady_timer: a wait that is cancelled, restarted or destroyed
// never calls its handler, even when its expiry was already queued, which Asio cannot take back.

#include <webcpp/trystero/native/backend.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <chrono>
#include <memory>
#include <memory_resource>

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<trystero::native::timer> make_timer(asio::io_context& io) {
    return std::allocate_shared<trystero::native::timer>(
        std::pmr::polymorphic_allocator<>(std::pmr::get_default_resource()), io.get_executor(),
        std::pmr::get_default_resource());
}

/**
 * Starts `first`, then `second`, both at 0 ms, so that both have expired when the io_context first
 * looks: Asio then queues both completions as successes in one pass, `first`'s ahead, and what
 * `first` does to `second` comes after `second`'s expiry was queued.
 */
template <class Then>
void both_expired(asio::io_context& io, trystero::native::timer& first,
                  trystero::native::timer& second, int& second_fired, Then then) {
    first.start(0ms, then);
    second.start(0ms, [&second_fired] { ++second_fired; });
    io.run_for(100ms);  // lint-run: the timers' expiries are the events under test
    io.restart();
}

void a_cancelled_wait_never_fires() {
    asio::io_context io;
    const auto first = make_timer(io);
    const auto second = make_timer(io);
    int fired = 0;
    both_expired(io, *first, *second, fired, [&] { second->cancel(); });
    BOOST_TEST_EQ(fired, 0);
}

void a_destroyed_timer_never_fires() {
    asio::io_context io;
    const auto first = make_timer(io);
    auto second = make_timer(io);
    int fired = 0;
    both_expired(io, *first, *second, fired, [&] { second.reset(); });
    BOOST_TEST_EQ(fired, 0);
}

void a_restarted_timer_fires_only_its_new_wait() {
    asio::io_context io;
    const auto first = make_timer(io);
    const auto second = make_timer(io);
    int fired = 0;
    int restarted = 0;
    both_expired(io, *first, *second, fired, [&] { second->start(10ms, [&] { ++restarted; }); });
    io.run_for(100ms);  // lint-run: the restarted wait expires
    BOOST_TEST_EQ(fired, 0);
    BOOST_TEST_EQ(restarted, 1);
}

void an_uncancelled_wait_fires_once() {
    asio::io_context io;
    const auto timer = make_timer(io);
    int fired = 0;
    timer->start(1ms, [&] { ++fired; });
    io.run_for(100ms);  // lint-run: the wait expires
    BOOST_TEST_EQ(fired, 1);
}

}  // namespace

int main() {
    an_uncancelled_wait_fires_once();
    a_cancelled_wait_never_fires();
    a_destroyed_timer_never_fires();
    a_restarted_timer_fires_only_its_new_wait();
    return boost::report_errors();
}
