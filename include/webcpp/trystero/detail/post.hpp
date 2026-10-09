// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_POST_HPP
#define WEBCPP_TRYSTERO_DETAIL_POST_HPP

#include <webcpp/trystero/config.hpp>

#include <boost/asio/any_io_executor.hpp>  // lint-world: posts handlers only
#include <boost/asio/bind_allocator.hpp>   // lint-world: binds an allocator only
#include <boost/asio/io_context.hpp>       // lint-world: posts handlers only
#include <boost/asio/post.hpp>             // lint-world: posts handlers only

#include <memory_resource>
#include <utility>

namespace webcpp::trystero::detail {

/**
 * Posts a handler, allocated as its associated allocator says, to `ex`.
 *
 * Tip: Asio wraps what it posts through an `any_io_executor` in a function object allocated with
 * `std::allocator` (two `std::aligned_alloc` calls per post, measured), whatever the handler's
 * allocator; through an io_context's own executor it allocates the operation as the handler's
 * allocator says. So an executor that is an io_context's is posted to directly.
 */
template <class Handler>
void post_handler(const boost::asio::any_io_executor& ex, Handler&& handler) {
    if (const auto* io = ex.target<boost::asio::io_context::executor_type>()) {
        boost::asio::post(*io, std::forward<Handler>(handler));
        return;
    }
    boost::asio::post(ex, std::forward<Handler>(handler));
}

/** Posts `f` to `ex`, allocated in `memory`. */
template <class F>
void post(const boost::asio::any_io_executor& ex, std::pmr::memory_resource* memory, F&& f) {
    post_handler(ex, boost::asio::bind_allocator(std::pmr::polymorphic_allocator<>(memory),
                                                 std::forward<F>(f)));
}

}  // namespace webcpp::trystero::detail

#endif
