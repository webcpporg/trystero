// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_CONSTRUCTION_WINDOW_HPP
#define WEBCPP_TRYSTERO_DETAIL_CONSTRUCTION_WINDOW_HPP

#include <webcpp/trystero/config.hpp>

#include <boost/circular_buffer.hpp>

#include <chrono>
#include <cstddef>
#include <memory_resource>

namespace webcpp::trystero::detail {

/**
 * Allows at most `limit` constructions in any span of `window`: it keeps the times of the last
 * `limit` it allowed, and allows another once the oldest of them is a window old.
 *
 * Tip: it counts what was made, not what is still held, so a place freed early, by a connection
 * that failed or was closed at once, is not taken again until the window has passed: a browser
 * counts a closed connection against its limit until its garbage collector runs.
 */
class construction_window {
public:
    /** No construction yet; the times are allocated in `memory`. */
    construction_window(std::size_t limit, std::chrono::milliseconds window,
                        std::pmr::memory_resource* memory)
        : times_(limit, memory), window_ms_(static_cast<double>(window.count())) {}

    /** Takes a construction at `now_ms`, or returns false and takes none when `limit` were
     * allowed in the window that ends then. */
    bool take(double now_ms) {
        if (times_.capacity() == 0 || (times_.full() && now_ms - times_.front() < window_ms_)) {
            return false;
        }
        times_.push_back(now_ms);  // in the oldest's place once full
        return true;
    }

private:
    boost::circular_buffer<double, std::pmr::polymorphic_allocator<double>> times_;
    double window_ms_;
};

}  // namespace webcpp::trystero::detail

#endif
