// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_WIPE_HPP
#define WEBCPP_TRYSTERO_DETAIL_WIPE_HPP

#include <webcpp/trystero/config.hpp>

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace webcpp::trystero::detail {

/**
 * Zeroes `byte_count` bytes at `wiped_data` with stores the optimiser cannot drop.
 *
 * This stands in until webcpp::crypto publishes a wipe of its own; the library does not reach
 * into another library's detail namespace.
 *
 * Tip: a `memset` before an object dies is a dead store a compiler may remove; stores through
 * a volatile pointer, then a signal fence, are kept.
 *
 * @param wiped_data The first byte, or null when `byte_count` is 0.
 * @param byte_count How many bytes.
 */
inline void wipe(void* wiped_data, std::size_t byte_count) noexcept {
    auto* const wiped_bytes = static_cast<unsigned char volatile*>(wiped_data);
    for (std::size_t byte_index = 0; byte_index < byte_count; ++byte_index) {
        wiped_bytes[byte_index] = 0;
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

/**
 * Zeroes an object's bytes, as @ref wipe does.
 *
 * @tparam T A trivially copyable type, a C library's key schedule or context.
 * @param wiped_object The object.
 */
template <class T>
void wipe_object(T& wiped_object) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "only a plain state is wiped");
    wipe(&wiped_object, sizeof wiped_object);
}

}  // namespace webcpp::trystero::detail

#endif  // WEBCPP_TRYSTERO_DETAIL_WIPE_HPP
