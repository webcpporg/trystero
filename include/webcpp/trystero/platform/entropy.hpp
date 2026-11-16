// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PLATFORM_ENTROPY_HPP
#define WEBCPP_TRYSTERO_PLATFORM_ENTROPY_HPP

#include <webcpp/trystero/config.hpp>

#ifdef WEBCPP_TARGET_EMSCRIPTEN
#include <emscripten/em_asm.h>
#else
#include <openssl/rand.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace webcpp::trystero::platform::detail {

#ifdef WEBCPP_TARGET_EMSCRIPTEN

// clang-format off
/**
 * Fills the `size` bytes at `data` from the page's `crypto.getRandomValues`: 0 when it filled
 * them, -1 when the page has no such function or it threw, caught here, so that no JavaScript
 * exception enters WebAssembly, as one does through the C library's entropy call, Emscripten's,
 * which lets the page's exception through.
 */
// NOLINTNEXTLINE(misc-const-correctness): JavaScript writes to the pointed WebAssembly storage
inline int js_fill_random(void* data, std::size_t size) {
    return EM_ASM_INT(({
    const data = $0 >>> 0;
    const size = $1 >>> 0;

    try {
        const view = HEAPU8.subarray(data, data + size);
        /* Web Crypto fills no view of a SharedArrayBuffer, which a threaded build's memory is. */
        if (view.buffer instanceof ArrayBuffer) {
            crypto.getRandomValues(view);
        } else {
            view.set(crypto.getRandomValues(new Uint8Array(size)));
        }
        return 0;
    } catch (_) {
        return -1;
    }
    }), data, size);
}
// clang-format on

#endif

/**
 * Fills `out` from the platform's one secure source of randomness, its result checked: natively
 * OpenSSL's generator; on emscripten the page's `crypto.getRandomValues`, whose absence or
 * exception is a failure, not an exception in WebAssembly.
 *
 * @param out The bytes to fill.
 * @return Whether every byte was filled; on false, the bytes must not be used.
 */
inline bool fill_random(std::span<std::uint8_t> out) noexcept {
#ifdef WEBCPP_TARGET_EMSCRIPTEN
    // Web Crypto fills at most 65536 bytes a call, and throws for more.
    constexpr std::size_t most = std::size_t{1} << 16U;
#else
    // OpenSSL's generator takes its count as an int.
    constexpr std::size_t most = std::size_t{1} << 30U;
#endif
    for (std::size_t at = 0; at < out.size(); at += most) {
        const auto piece = std::min(most, out.size() - at);
#ifdef WEBCPP_TARGET_EMSCRIPTEN
        if (js_fill_random(out.data() + at, piece) != 0) {  // lint-world: Trystero's randomness
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
