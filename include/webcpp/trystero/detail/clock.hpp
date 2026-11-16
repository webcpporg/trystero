// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_CLOCK_HPP
#define WEBCPP_TRYSTERO_DETAIL_CLOCK_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>

#include <chrono>

namespace webcpp::trystero::detail {

/**
 * A time of the backend's monotonic clock, as the core keeps it: when a relay opened, a ping left
 * or a construction was made, which its rate limits and timeouts compare.
 *
 * Tip: the clock is the backend's, never read here; the memory backend of the tests makes it
 * virtual. Its origin is the backend's, which a time point takes as the steady clock's epoch.
 */
using time_point = std::chrono::steady_clock::time_point;  // lint-world: names a type only

/** The time of the backend's monotonic clock, its `steady_now`. */
inline time_point steady_now(backend::factory& factory) {
    return factory.steady_now();
}

/** The time of the backend's wall clock, its `unix_now`, in whole seconds since the Unix epoch:
 * what a Nostr event's `created_at` and a subscription's `since` say. */
inline std::chrono::sys_seconds unix_now(backend::factory& factory) {
    return factory.unix_now();
}

}  // namespace webcpp::trystero::detail

#endif
