// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_CONSTRUCTION_WINDOW_HPP
#define WEBCPP_TRYSTERO_DETAIL_CONSTRUCTION_WINDOW_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/detail/clock.hpp>

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
        : times_(limit, memory), window_(window) {}

    /** Deleted: a boost::circular_buffer moves by swapping its allocator, which std::pmr's
     * cannot. */
    construction_window(construction_window&&) = delete;
    /** Deleted, as the move constructor is. */
    construction_window& operator=(construction_window&&) = delete;

    /** Takes a construction at `now`, or returns false and takes none when `limit` were allowed in
     * the window that ends then. */
    bool take(time_point now) {
        if (times_.capacity() == 0 || (times_.full() && now - times_.front() < window_)) {
            return false;
        }
        times_.push_back(now);  // in the oldest's place once full
        return true;
    }

    /** Takes a construction at `now_ms`, milliseconds of the backend's clock as a backend reads
     * its own `now_ms`, as take(time_point) does. */
    bool take(double now_ms) { return take(from_backend_ms(now_ms)); }

private:
    boost::circular_buffer<time_point, std::pmr::polymorphic_allocator<time_point>> times_;
    std::chrono::milliseconds window_;
};

}  // namespace webcpp::trystero::detail

#endif
