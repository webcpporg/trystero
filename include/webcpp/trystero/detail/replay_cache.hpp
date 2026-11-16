// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_REPLAY_CACHE_HPP
#define WEBCPP_TRYSTERO_DETAIL_REPLAY_CACHE_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/topics.hpp>

#include <boost/circular_buffer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace webcpp::trystero::detail {

/**
 * The digests of the last `capacity` offers and answers a room made or acted on, so that it refuses
 * one that comes to it as a peer's.
 *
 * Tip: each offer or answer describes a connection made for it, with ICE credentials and a DTLS
 * fingerprint of its own, so a room never acts on the same one twice, nor on its own: one that
 * opens with the room key and is known was sent by someone who read the relays. The digest is
 * the SHA-256 of the opened text, not of the sealed one, whose encoding a resender can vary (the
 * base64's padding, the IV's digits) while it still opens to the same text.
 */
class replay_cache {
public:
    /** Remembers nothing yet; the digests are allocated in `memory`, up to `capacity` of them, as
     * they come. */
    replay_cache(std::size_t capacity, std::pmr::memory_resource* memory)
        : digests_(capacity, memory) {}

    /** Deleted: a boost::circular_buffer moves by swapping its allocator, which std::pmr's
     * cannot. */
    replay_cache(replay_cache&&) = delete;
    /** Deleted, as the move constructor is. */
    replay_cache& operator=(replay_cache&&) = delete;

    /** Whether `opened`, an offer's or an answer's text, is among the last `capacity` used. */
    [[nodiscard]] bool contains(std::string_view opened) const {
        return std::ranges::find(digests_, protocol::sha256(opened)) != digests_.end();
    }

    /** Remembers `opened` as used, in place of the oldest once `capacity` are remembered. */
    void insert(std::string_view opened) { digests_.push_back(protocol::sha256(opened)); }

private:
    using digest = std::array<std::uint8_t, 32>;

    // Its storage grows with the digests remembered, up to `capacity`: a quiet room needs a few.
    boost::circular_buffer_space_optimized<digest, std::pmr::polymorphic_allocator<digest>>
        digests_;
};

}  // namespace webcpp::trystero::detail

#endif
