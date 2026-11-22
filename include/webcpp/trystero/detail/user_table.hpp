// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_USER_TABLE_HPP
#define WEBCPP_TRYSTERO_DETAIL_USER_TABLE_HPP

#include <webcpp/trystero/config.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace webcpp::trystero::detail {

/**
 * The users of a room and their active peers, in the room's resource: a user joins with its first
 * peer and leaves with its last, as 0.27.0's room counts a user's peers (`room.ts`,
 * `emitActivatedEvents` and `removePeerEntry`).
 *
 * A peer belongs to one user for as long as it is in the table: a peer known already is never
 * added again, under its user or under another.
 */
class user_table {
public:
    /** The peers of each user, in the order they joined. */
    using user_map =
        std::pmr::map<std::pmr::string, std::pmr::vector<std::pmr::string>, std::less<>>;

    /** No user yet; what the table holds is allocated in `memory`. */
    explicit user_table(std::pmr::memory_resource* memory)
        : user_by_peer_(memory), peers_by_user_(memory) {}

    /**
     * Adds an active peer of a user.
     *
     * @return Whether the user is new, `peer_id` its first peer; false as well when `peer_id` is
     * known already, which changes nothing.
     */
    bool add(std::string_view peer_id, std::string_view user_id) {
        if (user_by_peer_.contains(peer_id)) {
            return false;
        }
        auto* const memory = user_by_peer_.get_allocator().resource();
        user_by_peer_.try_emplace(std::pmr::string(peer_id, memory),
                                  std::pmr::string(user_id, memory));
        auto user = peers_by_user_.find(user_id);
        const bool first = user == peers_by_user_.end();
        if (first) {
            user = peers_by_user_
                       .try_emplace(std::pmr::string(user_id, memory),
                                    std::pmr::vector<std::pmr::string>(memory))
                       .first;
        }
        user->second.emplace_back(peer_id);
        return first;
    }

    /**
     * Removes a peer.
     *
     * @return Its user, when `peer_id` was its last peer; nothing when the user keeps another peer
     * or `peer_id` is not in the table.
     */
    std::optional<std::pmr::string> remove(std::string_view peer_id) {
        const auto peer = user_by_peer_.find(peer_id);
        if (peer == user_by_peer_.end()) {
            return std::nullopt;
        }
        const auto user = peers_by_user_.find(peer->second);
        user_by_peer_.erase(peer);
        auto& peers = user->second;
        peers.erase(std::ranges::find(peers, peer_id));
        if (!peers.empty()) {
            return std::nullopt;
        }
        auto node = peers_by_user_.extract(user);
        return std::move(node.key());
    }

    /** The peers of a user, in the order they joined; none for a user not in the table. The span is
     * valid until the table changes. */
    [[nodiscard]] std::span<const std::pmr::string> peers_of(std::string_view user_id) const {
        const auto user = peers_by_user_.find(user_id);
        if (user == peers_by_user_.end()) {
            return {};
        }
        return user->second;
    }

    /** The user of a peer, or nothing for a peer not in the table. The view is valid until the
     * peer is removed. */
    [[nodiscard]] std::optional<std::string_view> user_of(std::string_view peer_id) const {
        const auto peer = user_by_peer_.find(peer_id);
        if (peer == user_by_peer_.end()) {
            return std::nullopt;
        }
        return peer->second;
    }

    /** Every user, by id, with its peers in the order they joined. */
    [[nodiscard]] const user_map& users() const { return peers_by_user_; }

    /** Removes every peer and every user, reporting none. */
    void clear() noexcept {
        user_by_peer_.clear();
        peers_by_user_.clear();
    }

private:
    std::pmr::map<std::pmr::string, std::pmr::string, std::less<>> user_by_peer_;
    user_map peers_by_user_;
};

}  // namespace webcpp::trystero::detail

#endif
