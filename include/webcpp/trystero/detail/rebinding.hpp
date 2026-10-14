// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_REBINDING_HPP
#define WEBCPP_TRYSTERO_DETAIL_REBINDING_HPP
#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>

#include <boost/assert.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

namespace webcpp::trystero::detail {
class connection;

/** The bounded delay after the given number of unsuccessful bindings. */
constexpr std::chrono::milliseconds rebinding_step(std::chrono::milliseconds handshake_timeout,
                                                   std::uint32_t ends) {
    BOOST_ASSERT(ends >= 1);
    constexpr std::chrono::milliseconds keep_time{10'000};  // Trystero's (C/data-limits.mjs:4)
    constexpr std::chrono::milliseconds margin{1'000};
    constexpr std::chrono::milliseconds cap{300'000};
    if (handshake_timeout >= cap - margin) {
        return cap;
    }
    auto step = std::max(handshake_timeout, keep_time) + margin;
    for (std::uint32_t i = 1; i < ends && step < cap; ++i) {
        step = step >= cap / 2 ? cap : step * 2;
    }
    return std::min(step, cap);
}

/** The last eight nonces issued since a completed handshake. */
class recent_challenges {
public:
    /** The most challenges remembered between completed handshakes. */
    static constexpr std::size_t capacity = 8;

    /** Keeps the bounded challenge history in the room resource. */
    explicit recent_challenges(std::pmr::polymorphic_allocator<> a) : values_(a) {}

    /** Remembers a challenge, dropping the oldest beyond capacity. */
    void push(std::pmr::string challenge) {
        values_.push_back(std::move(challenge));
        if (values_.size() > capacity) {
            values_.pop_front();
        }
    }

    /** Whether any remembered challenge satisfies the response matcher. */
    template <class Matches>
    [[nodiscard]] bool any_of(Matches&& matches) const {
        return std::ranges::any_of(values_, std::forward<Matches>(matches));
    }

    /** Forgets challenges after a completed handshake. */
    void clear() { values_.clear(); }

private:
    std::pmr::deque<std::pmr::string> values_;
};

/** One room's history of a peer on a particular shared connection. */
struct rebind_state {
    /** Creates one peer's history in the room resource. */
    explicit rebind_state(std::pmr::polymorphic_allocator<> a) : challenges(a) {}

    /** The consecutive binding ends since a completed handshake. */
    std::uint32_t ends = 0;
    /** Whether this room has met the peer on this connection. */
    bool met = false;
    /** Whether the lower id's own-kick backoff refuses binding. */
    bool refusing = false;
    /** Whether the higher id drops frames until it sends presence. */
    bool dropping = false;
    /** The connection this history belongs to, without keeping it alive. */
    std::weak_ptr<connection> on;
    /** The next scheduled rebinding, cancelled when history ends. */
    std::shared_ptr<backend::timer> timer;
    /** The initiator's recent challenges whose correct answers still pass. */
    recent_challenges challenges;
};
}  // namespace webcpp::trystero::detail
#endif
