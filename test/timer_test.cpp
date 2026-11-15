// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the timer over webcpp::asio's steady_timer, on both targets: a wait fires once; a wait
// that is cancelled, restarted or destroyed never calls its handler, even when its expiry was
// already queued, which Asio cannot take back; and its waits are allocated in the resource it is
// given. On emscripten the io_context's run family suspends into node's event loop, where the
// reactor's one setTimeout wakes it.

#include <webcpp/trystero/detail/timer.hpp>

#include <webcpp/asio/io_context.hpp>

#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using namespace std::chrono_literals;

namespace {

/** A resource that counts what it gives, and what is still out. */
class counting_resource final : public std::pmr::memory_resource {
public:
    /** How many allocations it made. */
    std::size_t allocations = 0;
    /** How many of them are not yet returned. */
    std::size_t outstanding = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        ++outstanding;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        --outstanding;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

std::shared_ptr<trystero::detail::timer> make_timer(asio::io_context& io,
                                                    std::pmr::memory_resource* memory) {
    return std::allocate_shared<trystero::detail::timer>(std::pmr::polymorphic_allocator<>(memory),
                                                         io.get_executor(), memory);
}

/**
 * Starts `first`, then `second`, both at 0 ms, so that both have expired when the io_context first
 * looks: Asio then queues both completions as successes in one pass, `first`'s ahead, and what
 * `first` does to `second` comes after `second`'s expiry was queued.
 */
template <class Then>
void both_expired(asio::io_context& io, trystero::detail::timer& first,
                  trystero::detail::timer& second, std::size_t& second_fired, Then then) {
    first.start(0ms, then);
    second.start(0ms, [&second_fired] { ++second_fired; });
    io.run_for(100ms);  // lint-run: the timers' expiries are the events under test
    io.restart();
}

void an_uncancelled_wait_fires_once() {
    asio::io_context io;
    counting_resource memory;
    const auto timer = make_timer(io, &memory);
    const std::size_t made = memory.allocations;
    std::size_t fired = 0;
    timer->start(1ms, [&] { ++fired; });
    io.run_for(100ms);  // lint-run: the wait expires
    BOOST_TEST_EQ(fired, 1U);
    // The wait was allocated in the timer's resource, and given back once it completed.
    BOOST_TEST_GT(memory.allocations, made);
    BOOST_TEST_EQ(memory.outstanding, 2U);  // the timer, and its count of waits
}

void a_cancelled_wait_never_fires() {
    asio::io_context io;
    std::pmr::memory_resource* const memory = std::pmr::get_default_resource();
    const auto first = make_timer(io, memory);
    const auto second = make_timer(io, memory);
    std::size_t fired = 0;
    both_expired(io, *first, *second, fired, [&] { second->cancel(); });
    BOOST_TEST_EQ(fired, 0U);
}

void a_destroyed_timer_never_fires() {
    asio::io_context io;
    std::pmr::memory_resource* const memory = std::pmr::get_default_resource();
    const auto first = make_timer(io, memory);
    auto second = make_timer(io, memory);
    std::size_t fired = 0;
    both_expired(io, *first, *second, fired, [&] { second.reset(); });
    BOOST_TEST_EQ(fired, 0U);
}

void a_restarted_timer_fires_only_its_new_wait() {
    asio::io_context io;
    std::pmr::memory_resource* const memory = std::pmr::get_default_resource();
    const auto first = make_timer(io, memory);
    const auto second = make_timer(io, memory);
    std::size_t fired = 0;
    std::size_t restarted = 0;
    both_expired(io, *first, *second, fired, [&] { second->start(10ms, [&] { ++restarted; }); });
    io.run_for(100ms);  // lint-run: the restarted wait expires
    BOOST_TEST_EQ(fired, 0U);
    BOOST_TEST_EQ(restarted, 1U);
}

}  // namespace

int main() {
    an_uncancelled_wait_fires_once();
    a_cancelled_wait_never_fires();
    a_destroyed_timer_never_fires();
    a_restarted_timer_fires_only_its_new_wait();
    return webcpp::test::exit_status(boost::report_errors());
}
