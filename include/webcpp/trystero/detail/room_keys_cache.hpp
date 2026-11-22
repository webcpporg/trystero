// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_ROOM_KEYS_CACHE_HPP
#define WEBCPP_TRYSTERO_DETAIL_ROOM_KEYS_CACHE_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/post.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/room_keys.hpp>

#include <webcpp/crypto/secret.hpp>
#include <webcpp/trystero/detail/wipe.hpp>

#include <webcpp/asio/any_io_executor.hpp>  // lint-world: names the executor only

#include <boost/system/error_code.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/**
 * The keys of an app's rooms, as 0.27.0's `genKey` and `derivedRoomKeysCache` give them: a room
 * without a password keys AES with the SHA-256 of its domain-tagged path, computed at each call;
 * with one, the room's PBKDF2 master is derived once per room id, off the room's thread through
 * `backend::factory::derive_password_master`, then HKDF gives the AES and HMAC keys, and the
 * master is wiped. The derived keys of at most @ref capacity rooms are kept, the least recently
 * asked for leaving first, and a failed derivation is forgotten, so that the next call derives
 * again.
 *
 * The password is the app's, one for all its rooms (0.1.0's ruling), so the keys are kept by room
 * id alone. Everything is in the app's resource.
 *
 * Tip: a derivation outlives the cache when the app ends during it: it owns its copy of the
 * password, its salt and its master, which the backend reads and writes until its completion, and
 * its completion reaches the cache through a weak reference.
 */
class room_keys_cache : public std::enable_shared_from_this<room_keys_cache> {
public:
    /** How many rooms' derived keys the cache keeps, 0.27.0's 64. */
    static constexpr std::size_t capacity = 64;

    /** What a call of @ref get completes with: the error, and the keys, valid for the call. */
    using done_handler = handler<void(boost::system::error_code, const protocol::room_keys&)>;

    /** A cache of the rooms of `app_id`, whose password is `password`, both copied. */
    room_keys_cache(webcpp::asio::any_io_executor ex, backend::factory& factory,
                    std::string_view app_id, std::string_view password,
                    std::pmr::memory_resource* memory)
        : ex_(std::move(ex)),
          factory_(factory),
          memory_(memory),
          app_id_(app_id, memory),
          password_(password, memory),
          entries_(memory),
          recency_(memory) {}

    /** The cache holds a secret, which no copy must duplicate. */
    room_keys_cache(const room_keys_cache&) = delete;
    /** Not assignable either. */
    room_keys_cache& operator=(const room_keys_cache&) = delete;

    /** Wipes the password; a derivation under way completes without the cache. */
    ~room_keys_cache() { webcpp::trystero::detail::wipe(password_.data(), password_.size()); }

    /**
     * The keys of `room_id`, through `done`, posted to the executor: after the call returns, even
     * when they are known at once.
     *
     * @param room_id The room's id, read during the call.
     * @param done Called with the keys; or, when the derivation failed, with
     * `error::backend_failure` and keys of zeros, the cache then forgetting it.
     */
    void get(std::string_view room_id, done_handler done) {
        if (password_.empty()) {
            protocol::room_keys keys{
                .aes = protocol::room_aes_key_without_password(app_id_, room_id, memory_),
                .hmac = {},
            };
            post(ex_, memory_,
                 [keys = std::move(keys), done = std::move(done)]() mutable { done({}, keys); });
            return;
        }
        if (const auto it = entries_.find(room_id); it != entries_.end()) {
            recency_.splice(recency_.begin(), recency_, it->second.place);
            const auto d = it->second.keys;
            if (d->keys) {
                post(ex_, memory_, [d, done = std::move(done)]() mutable { done({}, *d->keys); });
            } else {
                d->waiters.push_back(std::move(done));
            }
            return;
        }
        derive(room_id, std::move(done));
    }

    /** How many rooms' keys the cache holds now, or derives. */
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    /** One room's derivation and, once it succeeded, its keys. */
    struct derivation {
        derivation(std::pmr::memory_resource* memory, std::string_view room_id,
                   std::string_view password)
            : room_id(room_id, memory), password(password, memory), waiters(memory) {}

        derivation(const derivation&) = delete;
        derivation& operator=(const derivation&) = delete;

        /** Wipes the password's copy; the master and the keys wipe themselves. */
        ~derivation() { webcpp::trystero::detail::wipe(password.data(), password.size()); }

        std::pmr::string room_id;
        std::pmr::string password;
        std::array<std::uint8_t, 32> salt{};
        webcpp::crypto::secret<32> master;
        std::optional<protocol::room_keys> keys;
        std::pmr::vector<done_handler> waiters;
    };

    struct entry {
        std::shared_ptr<derivation> keys;
        std::pmr::list<std::pmr::string>::iterator place;
    };

    std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    /** Starts the derivation of `room_id`'s keys, which the cache holds no entry of. */
    void derive(std::string_view room_id, done_handler done) {
        if (entries_.size() >= capacity) {
            entries_.erase(recency_.back());
            recency_.pop_back();
        }
        auto d = std::allocate_shared<derivation>(allocator(), memory_, room_id, password_);
        d->salt = protocol::pbkdf2_salt(app_id_, room_id, memory_);
        d->waiters.push_back(std::move(done));
        recency_.emplace_front(room_id);
        entries_.try_emplace(recency_.front(), entry{.keys = d, .place = recency_.begin()});
        const std::span<const std::uint8_t> password(
            reinterpret_cast<const std::uint8_t*>(d->password.data()), d->password.size());
        const std::span<std::uint8_t, 32> master = d->master.writable();
        factory_.derive_password_master(
            password, d->salt, master,
            handler<void(boost::system::error_code)>(
                std::allocator_arg, allocator(),
                [weak = weak_from_this(), d](boost::system::error_code ec) {
                    derived(weak, d, ec);
                }));
    }

    /** Ends the derivation `d`: its keys from its master by HKDF, its master wiped, and every
     * waiter called; a failure leaves the cache. */
    static void derived(const std::weak_ptr<room_keys_cache>& weak,
                        const std::shared_ptr<derivation>& d, boost::system::error_code ec) {
        if (!ec) {
            auto keys = protocol::derive_room_keys(d->master.bytes(),
                                                   d->waiters.get_allocator().resource());
            if (keys) {
                d->keys.emplace(std::move(*keys));
            } else {
                ec = make_error_code(error::backend_failure);
            }
        }
        const auto master = d->master.writable();
        webcpp::trystero::detail::wipe(master.data(), master.size());
        const auto self = weak.lock();
        if (ec) {
            ec = make_error_code(error::backend_failure);
            if (self) {
                self->forget(d);
            }
        }
        // A waiter may ask again, or end the app: the waiters are taken out first, and `d` keeps
        // the keys each is handed.
        auto waiters =
            std::exchange(d->waiters, std::pmr::vector<done_handler>(d->waiters.get_allocator()));
        const protocol::room_keys none;
        for (auto& waiter : waiters) {
            waiter(ec, d->keys ? *d->keys : none);
        }
    }

    /** Forgets `d`, if the cache still holds it. */
    void forget(const std::shared_ptr<derivation>& d) {
        const auto it = entries_.find(d->room_id);
        if (it != entries_.end() && it->second.keys == d) {
            recency_.erase(it->second.place);
            entries_.erase(it);
        }
    }

    webcpp::asio::any_io_executor ex_;
    backend::factory& factory_;
    std::pmr::memory_resource* memory_;
    std::pmr::string app_id_;
    std::pmr::string password_;
    // By room id, each key a view of its node in the recency list, which owns the text.
    std::pmr::map<std::string_view, entry, std::less<>> entries_;
    // The room ids, the most recently asked for first.
    std::pmr::list<std::pmr::string> recency_;
};

}  // namespace webcpp::trystero::detail

#endif
