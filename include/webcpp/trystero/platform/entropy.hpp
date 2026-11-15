// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PLATFORM_ENTROPY_HPP
#define WEBCPP_TRYSTERO_PLATFORM_ENTROPY_HPP

#include <webcpp/trystero/config.hpp>

#ifdef WEBCPP_TARGET_EMSCRIPTEN
#include <sys/random.h>  // lint-world: getentropy, the page's crypto.getRandomValues
#else
#include <openssl/rand.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace webcpp::trystero::platform::detail {

/**
 * Fills `out` from the platform's one secure source of randomness, its result checked: natively
 * OpenSSL's generator; on emscripten the C library's entropy call, which Emscripten draws from
 * the page's `crypto.getRandomValues`.
 *
 * @param out The bytes to fill.
 * @return Whether every byte was filled; on false, the bytes must not be used.
 */
inline bool fill_random(std::span<std::uint8_t> out) noexcept {
#ifdef WEBCPP_TARGET_EMSCRIPTEN
    // The C library's entropy call fills at most 256 bytes at a time.
    constexpr std::size_t most = 256;
#else
    // OpenSSL's generator takes its count as an int.
    constexpr std::size_t most = std::size_t{1} << 30U;
#endif
    for (std::size_t at = 0; at < out.size(); at += most) {
        const auto piece = std::min(most, out.size() - at);
#ifdef WEBCPP_TARGET_EMSCRIPTEN
        if (getentropy(out.data() + at, piece) != 0) {  // lint-world: Trystero's randomness
            return false;
        }
#else
        const auto count = static_cast<int>(piece);     // at most 2^30, in an int's range
        if (RAND_bytes(out.data() + at, count) != 1) {  // lint-world: Trystero's randomness
            return false;
        }
#endif
    }
    return true;
}

}  // namespace webcpp::trystero::platform::detail

#endif
