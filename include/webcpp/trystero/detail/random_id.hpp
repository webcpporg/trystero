// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_RANDOM_ID_HPP
#define WEBCPP_TRYSTERO_DETAIL_RANDOM_ID_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <boost/assert.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>

namespace webcpp::trystero::detail {

/** The longest id @ref random_id draws: a Nostr subscription's, 64 characters. */
inline constexpr std::size_t max_random_id_size = 64;

/** An id of `n` characters drawn from the backend's randomness, as `protocol::random_id` maps
 * them, allocated in `memory`; `n` is at most @ref max_random_id_size. */
inline std::pmr::string random_id(backend::factory& factory, std::size_t n,
                                  std::pmr::memory_resource* memory) {
    BOOST_ASSERT(n <= max_random_id_size);
    std::array<std::uint8_t, max_random_id_size> random{};
    const auto drawn = std::span(random).first(n);
    factory.random(drawn);
    return protocol::random_id(drawn, memory);
}

}  // namespace webcpp::trystero::detail

#endif
