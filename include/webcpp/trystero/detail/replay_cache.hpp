// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_REPLAY_CACHE_HPP
#define WEBCPP_TRYSTERO_DETAIL_REPLAY_CACHE_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/topics.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <vector>

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
    /** Remembers nothing yet; the digests are allocated in `memory`, up to `capacity` of them. */
    replay_cache(std::size_t capacity, std::pmr::memory_resource* memory)
        : digests_(memory), capacity_(capacity) {}

    /** Whether `opened`, an offer's or an answer's text, is among the last `capacity` used. */
    [[nodiscard]] bool contains(std::string_view opened) const {
        return std::ranges::find(digests_, protocol::sha256(opened)) != digests_.end();
    }

    /** Remembers `opened` as used, in place of the oldest once `capacity` are remembered. */
    void insert(std::string_view opened) {
        if (capacity_ == 0) {
            return;
        }
        if (digests_.size() < capacity_) {
            digests_.push_back(protocol::sha256(opened));
            return;
        }
        digests_[next_] = protocol::sha256(opened);
        next_ = (next_ + 1) % capacity_;
    }

private:
    // A ring once full, whose oldest digest is at next_.
    std::pmr::vector<std::array<std::uint8_t, 32>> digests_;
    std::size_t capacity_;
    std::size_t next_ = 0;
};

}  // namespace webcpp::trystero::detail

#endif
