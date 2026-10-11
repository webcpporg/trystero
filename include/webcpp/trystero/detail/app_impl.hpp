// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_APP_IMPL_HPP
#define WEBCPP_TRYSTERO_DETAIL_APP_IMPL_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/connection.hpp>
#include <webcpp/trystero/detail/ice_servers.hpp>
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/options.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <boost/asio/any_io_executor.hpp>  // lint-world: names the executor only

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/**
 * What a room of the app is to the app: the calls a connection's events become. A room implements
 * it; the app reaches it through the registry, which the room leaves before it ends.
 */
class room_endpoint {
public:
    /** The remote advertised this room's token (`present`) or withdrew it. */
    virtual void presence_changed(std::string_view peer, bool present) = 0;
    /** Whether the connection keeps this room's frames from `peer` while the room is not bound to
     * it: false while the room drops them (its dropping window). */
    [[nodiscard]] virtual bool keeps_frames_from(std::string_view peer) const = 0;
    /** A frame of this room's token from `peer` was kept. */
    virtual void frame_kept(std::string_view peer) = 0;
    /** The app's connection to `peer` ended. */
    virtual void connection_ended(std::string_view peer, std::string_view reason) = 0;

protected:
    room_endpoint() = default;
    room_endpoint(const room_endpoint&) = default;
    room_endpoint& operator=(const room_endpoint&) = default;
    ~room_endpoint() = default;
};

/**
 * The state an `app` and its rooms share: the configuration, the self id, the rooms by token, and
 * one connection per remote peer. Every allocation goes through `app_config::memory`.
 *
 * Tip: its users are the `app` handle and every room not yet ended; when the last goes, it ends
 * every connection at that call. Its destructor ends nothing: it may run while the io_context is
 * destroyed, from the last operation pending there (the spec's section 4.2, "The app's users").
 */
class app_impl : public std::enable_shared_from_this<app_impl> {
public:
    using executor_type = boost::asio::any_io_executor;

    /** Copies the configuration into its resource, draws the self id unless it names one, and sets
     * aside what no peer could use, each kept as a report. */
    app_impl(executor_type ex, backend::factory& factory, app_config config)
        : memory_(config.memory),
          ex_(std::move(ex)),
          factory_(factory),
          log_(std::move(config.log), memory_),
          app_id_(config.app_id, memory_),
          self_id_(config.self_id, memory_),
          relay_urls_(config.relay_urls, memory_),
          ice_servers_(copy_ice_servers(config.ice_servers, memory_)),
          idle_timeout_(config.idle_timeout),
          setup_reports_(memory_),
          rooms_(memory_),
          connections_(memory_) {
        if (self_id_.size() > protocol::max_peer_id_bytes) {
            self_id_.clear();
            set_aside("self_id is longer than any peer accepts, so the app uses a random one");
        }
        if (self_id_.empty()) {
            self_id_ = random_id(protocol::peer_id_size);
        }
        if (idle_timeout_ < std::chrono::milliseconds::zero()) {
            idle_timeout_ = std::chrono::milliseconds::zero();
            set_aside("idle_timeout is negative, so the app closes an unused connection at once");
        }
        if (ice_servers_.empty()) {
            ice_servers_ = default_ice_servers(memory_);
        }
        for (auto& server : ice_servers_) {
            for (auto& url : server.urls) {
                lowercase_scheme(url);
            }
        }
        for (auto it = ice_servers_.begin(); it != ice_servers_.end();) {
            const auto fault = ice_server_fault(*it);
            if (fault.empty()) {
                ++it;
                continue;
            }
            std::pmr::string what("the ICE server ", memory_);
            what += it->urls.empty() ? std::string_view("(no URL)") : std::string_view(it->urls[0]);
            what += ' ';
            what += fault;
            what += ", so the app goes on without it";
            set_aside(what);
            it = ice_servers_.erase(it);
        }
        if (relay_urls_.empty()) {
            for (const auto url : protocol::derive_relays(app_id_, protocol::default_nostr_relays,
                                                          config.relay_redundancy, memory_)) {
                relay_urls_.emplace_back(url);
            }
        }
    }

    app_impl(const app_impl&) = delete;
    app_impl& operator=(const app_impl&) = delete;

    [[nodiscard]] const executor_type& get_executor() const { return ex_; }

    [[nodiscard]] backend::factory& factory() const { return factory_; }

    [[nodiscard]] std::pmr::memory_resource* memory() const { return memory_; }

    [[nodiscard]] const logger& log() const { return log_; }

    [[nodiscard]] std::string_view app_id() const { return app_id_; }

    [[nodiscard]] std::string_view self_id() const { return self_id_; }

    [[nodiscard]] std::span<const std::pmr::string> relay_urls() const { return relay_urls_; }

    [[nodiscard]] std::span<const backend::ice_server> ice_servers() const { return ice_servers_; }

    [[nodiscard]] std::chrono::milliseconds idle_timeout() const { return idle_timeout_; }

    /** What the constructor set aside, a sentence each, in its order. */
    [[nodiscard]] std::span<const std::pmr::string> setup_reports() const { return setup_reports_; }

    /** The connections not ended whose channel is open. */
    [[nodiscard]] std::pmr::vector<std::shared_ptr<connection>> live_connections() const {
        std::pmr::vector<std::shared_ptr<connection>> out(memory_);
        for (const auto& [peer, c] : connections_) {
            if (c->live()) {
                out.push_back(c);
            }
        }
        return out;
    }

    /** Counts a user: the `app` handle, or a room not yet ended. */
    void add_user() { ++users_; }

    /** Releases a user; the last one's release ends every connection. */
    void release_user() {
        if (users_ == 0) {
            return;
        }
        --users_;
        if (users_ != 0) {
            return;
        }
        auto connections = std::exchange(connections_, decltype(connections_)(memory_));
        for (auto& [peer, c] : connections) {
            c->end("the app has no room left");
        }
    }

    /** How many rooms the registry holds. */
    [[nodiscard]] std::size_t counted_rooms() const { return rooms_.size(); }

    /** Registers a room by a weak reference (the spec's section 4.1): its control block is in the
     * room's resource, so the room unregisters before its state goes, in every way it ends, its
     * destructor included (review I10). */
    void register_room(std::string_view room_id, std::string_view token,
                       const std::shared_ptr<room_endpoint>& room) {
        rooms_.insert_or_assign(std::pmr::string(token, memory_),
                                entry{
                                    .room_id = std::pmr::string(room_id, memory_),
                                    .room = room,
                                    .raw = room.get(),
                                });
    }

    /** Forgets the room of `token`, if it is still `room`'s entry (review M7). */
    void unregister_room(std::string_view token, const room_endpoint* room) {
        if (const auto it = rooms_.find(token); it != rooms_.end() && it->second.raw == room) {
            rooms_.erase(it);
        }
    }

    /** The room of `room_id`, while it lives. */
    [[nodiscard]] std::shared_ptr<room_endpoint> find_room(std::string_view room_id) const {
        for (const auto& [token, e] : rooms_) {
            if (e.room_id == room_id) {
                return e.room.lock();
            }
        }
        return nullptr;
    }

    /** The live connection to `peer`, or null. */
    [[nodiscard]] std::shared_ptr<connection> find(std::string_view peer) const {
        const auto it = connections_.find(peer);
        return it != connections_.end() && it->second->live() ? it->second : nullptr;
    }

    /** The connection a room's negotiation opened becomes the app's, or, when the app already has
     * a live one to that peer, is closed and the existing one returned (C/shared-peer.mjs:105-108).
     *
     * Tip: a stale connection to the peer, one whose channel is closing, ends first, as Trystero
     * clears it (C/shared-peer.mjs:142-149): the rooms hear its end before the new connection is
     * adopted, and a room may adopt one to the peer from inside that call, which is then the one
     * shared, or ends in its turn if it is stale by then (review of Task 3, r1). One whose end is
     * already under way, further up the stack, is let go: it ends no second time. */
    std::shared_ptr<connection> adopt(std::string_view peer_view,
                                      std::shared_ptr<backend::peer_connection> pc,
                                      std::shared_ptr<backend::data_channel> channel) {
        // `peer_view` may be a view into the stale connection, which its end frees.
        const std::pmr::string peer(peer_view, memory_);
        for (auto it = connections_.find(peer); it != connections_.end() && !it->second->live();
             it = connections_.find(peer)) {
            const auto stale = it->second;
            stale->end("replaced by a new connection to the peer");
            if (const auto again = connections_.find(peer);
                again != connections_.end() && again->second == stale) {
                connections_.erase(again);
            }
        }
        return share_or_adopt(peer, std::move(pc), std::move(channel));
    }

private:
    // Its implicit move assignment, which insert_or_assign uses, may allocate: a std::pmr member
    // copies when the two resources differ, which bugprone-exception-escape reports. Every entry
    // is in the app's resource, so it never does.
    struct entry {  // NOLINT(bugprone-exception-escape)
        std::pmr::string room_id;
        std::weak_ptr<room_endpoint> room;
        const room_endpoint* raw;  // identity only, for unregister_room; never dereferenced
    };

    /** Closes the new connection and returns the live one to `peer`, if any; else adopts it. */
    std::shared_ptr<connection> share_or_adopt(std::string_view peer,
                                               std::shared_ptr<backend::peer_connection> pc,
                                               std::shared_ptr<backend::data_channel> channel) {
        if (auto existing = find(peer)) {
            channel->set_handlers({});
            pc->set_handlers({});
            channel->close();
            pc->close();
            log_("webrtc", "a second connection with ", peer, " closed: the first is shared");
            return existing;
        }
        const auto weak = weak_from_this();
        // The connection whose `ended` this is, set once it exists: its end erases its own entry
        // only, never one a room adopted since (Trystero's owns(), C/shared-peer.mjs:210-212).
        const auto owner = std::allocate_shared<std::weak_ptr<connection>>(allocator());
        auto c = std::allocate_shared<connection>(
            allocator(), factory_, peer, std::move(pc), std::move(channel),
            connection_callbacks{
                .presence = bind<void(std::string_view, std::string_view, bool)>(
                    [weak](std::string_view p, std::string_view token, bool present) {
                        if (const auto self = weak.lock()) {
                            if (const auto room = self->room_of(token)) {
                                room->presence_changed(p, present);
                            }
                        }
                    }),
                .keeps = bind<bool(std::string_view, std::string_view)>(
                    [weak](std::string_view p, std::string_view token) {
                        const auto self = weak.lock();
                        const auto room = self ? self->room_of(token) : nullptr;
                        return room != nullptr && room->keeps_frames_from(p);
                    }),
                .kept = bind<void(std::string_view, std::string_view)>(
                    [weak](std::string_view p, std::string_view token) {
                        if (const auto self = weak.lock()) {
                            if (const auto room = self->room_of(token)) {
                                room->frame_kept(p);
                            }
                        }
                    }),
                .ended = bind<void(std::string_view, std::string_view)>(
                    [weak, owner](std::string_view p, std::string_view reason) {
                        if (const auto self = weak.lock()) {
                            self->on_connection_end(owner->lock(), p, reason);
                        }
                    }),
            },
            log_, memory_);
        *owner = c;
        connections_.insert_or_assign(std::pmr::string(peer, memory_), c);
        std::pmr::vector<std::string_view> tokens(memory_);
        for (const auto& [token, e] : rooms_) {
            tokens.emplace_back(token);
        }
        c->start(tokens);
        return c;
    }

    [[nodiscard]] std::shared_ptr<room_endpoint> room_of(std::string_view token) const {
        const auto it = rooms_.find(token);
        return it == rooms_.end() ? nullptr : it->second.room.lock();
    }

    /** Forgets `ended`, unless another connection to the peer took its place, and tells every
     * room: those of a connection the last user's release took out of the registry too. */
    void on_connection_end(const std::shared_ptr<connection>& ended, std::string_view peer_view,
                           std::string_view reason_view) {
        const std::pmr::string peer(peer_view, memory_);
        const std::pmr::string reason(reason_view, memory_);
        if (const auto it = connections_.find(peer);
            it != connections_.end() && it->second == ended) {
            connections_.erase(it);
        }
        // A room may leave the registry from inside the call: the rooms are locked first, each
        // kept alive across its call.
        std::pmr::vector<std::shared_ptr<room_endpoint>> rooms(memory_);
        for (const auto& [token, e] : rooms_) {
            if (auto r = e.room.lock()) {
                rooms.push_back(std::move(r));
            }
        }
        for (const auto& room : rooms) {
            room->connection_ended(peer, reason);
        }
    }

    void set_aside(std::string_view what) {
        log_("app", what);
        setup_reports_.emplace_back(what);
    }

    std::pmr::string random_id(std::size_t n) {
        std::array<std::uint8_t, 64> random{};
        factory_.random(std::span(random).first(n));
        std::pmr::string id(n, '0', memory_);
        for (std::size_t i = 0; i < n; ++i) {
            id[i] = protocol::id_alphabet[random[i] % protocol::id_alphabet.size()];
        }
        return id;
    }

    [[nodiscard]] std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    template <class Signature, class F>
    handler<Signature> bind(F&& f) const {
        return handler<Signature>(std::allocator_arg, allocator(), std::forward<F>(f));
    }

    std::pmr::memory_resource* memory_;
    executor_type ex_;
    backend::factory& factory_;
    logger log_;
    std::pmr::string app_id_;
    std::pmr::string self_id_;
    std::pmr::vector<std::pmr::string> relay_urls_;
    std::pmr::vector<backend::ice_server> ice_servers_;
    std::chrono::milliseconds idle_timeout_;
    std::pmr::vector<std::pmr::string> setup_reports_;
    std::pmr::map<std::pmr::string, entry, std::less<>> rooms_;
    std::pmr::map<std::pmr::string, std::shared_ptr<connection>, std::less<>> connections_;
    std::size_t users_ = 0;
};

}  // namespace webcpp::trystero::detail

#endif
