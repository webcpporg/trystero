// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_NATIVE_TIMER_HPP
#define WEBCPP_TRYSTERO_NATIVE_TIMER_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>

#include <boost/asio/any_io_executor.hpp>  // lint-world: Trystero's timers
#include <boost/asio/bind_allocator.hpp>   // lint-world: allocates timer waits
#include <boost/asio/steady_timer.hpp>     // lint-world: Trystero's timers
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <utility>

namespace webcpp::trystero::native {

/**
 * An Asio timer whose waits are allocated in the room's resource.
 *
 * Tip: Asio cannot take back an expiry it has already queued, which it then completes as a
 * success; so each wait carries its number, and `cancel`, a later `start` and the destructor end
 * it, by changing the number or freeing it, before its handler runs.
 *
 * @see "The native backend", in the guide.
 */
class timer : public trystero::backend::timer {
public:
    /**
     * Constructs a timer, not started.
     *
     * @param ex The executor it fires on.
     * @param memory The room's resource, which each wait is allocated in; it must outlive the
     * timer's pending waits.
     */
    timer(const boost::asio::any_io_executor& ex, std::pmr::memory_resource* memory)
        : timer_(ex),
          memory_(memory),
          waits_(
              std::allocate_shared<std::uint64_t>(std::pmr::polymorphic_allocator<>(memory), 0)) {}

    /**
     * Calls `fire` on the executor once `delay` has passed, in place of a pending expiry.
     *
     * @param delay How long to wait.
     * @param fire What to call when the timer expires; never called once the timer is cancelled,
     * started again or destroyed, even when its expiry was already queued.
     */
    void start(std::chrono::milliseconds delay, handler<void()> fire) override {
        const std::uint64_t wait = ++*waits_;
        timer_.expires_after(delay);
        timer_.async_wait(boost::asio::bind_allocator(
            std::pmr::polymorphic_allocator<>(memory_),
            [waits = std::weak_ptr<std::uint64_t>(waits_), wait,
             fire = std::move(fire)](boost::system::error_code ec) mutable {
                if (const auto current = waits.lock(); !ec && current && *current == wait) {
                    fire();
                }
            }));
    }

    /** Cancels the pending expiry, if any, whose handler is then never called. */
    void cancel() override {
        ++*waits_;
        timer_.cancel();
    }

private:
    boost::asio::steady_timer timer_;  // lint-world: Trystero's timers
    std::pmr::memory_resource* memory_;
    // The number of the wait under way: a handler that finds another, or none, does nothing.
    std::shared_ptr<std::uint64_t> waits_;
};

}  // namespace webcpp::trystero::native

#endif
