// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_APP_IMPL_HPP
#define WEBCPP_TRYSTERO_DETAIL_APP_IMPL_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/ice_servers.hpp>
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/detail/post.hpp>
#include <webcpp/trystero/detail/room_keys_cache.hpp>
#include <webcpp/trystero/detail/wipe.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/identity.hpp>
#include <webcpp/trystero/options.hpp>
#include <webcpp/trystero/protocol/ids.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>
#include <webcpp/trystero/room_error.hpp>
#include <webcpp/trystero/subscription.hpp>

#include <webcpp/asio/any_io_executor.hpp>  // lint-world: names the executor only

#include <boost/assert.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
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
 * What a room of the app is to the app: what a join and the page's connectivity ask of it. A room
 * implements it; the app reaches it through the registry, which the room leaves before it ends.
 */
class room_endpoint {
public:
    /** Whether the room is in its farewell grace period. */
    [[nodiscard]] virtual bool leaving() const = 0;
    /** Finishes its farewell and completes every pending leave. */
    virtual void finish_farewell() = 0;
    /** The app's relays went offline (`offline`), so the room closes its relay sockets and holds
     * them closed, or online, so it reconnects every one not connected. */
    virtual void relays_offline(bool offline) = 0;

protected:
    room_endpoint() = default;
    room_endpoint(const room_endpoint&) = default;
    room_endpoint& operator=(const room_endpoint&) = default;
    ~room_endpoint() = default;
};

/**
 * The state an `app` and its rooms share: the configuration, the endpoint key and the self id it
 * derives, the user identity, the rooms by id, the rooms' keys and the relays' connectivity.
 * Nothing else is shared, as in 0.27.0, whose rooms share the identity and the relays alone: each
 * room has its own connection to each peer. Every allocation goes through `app_config::memory`,
 * except the key of a user identity the program gives, which is in the program's resource.
 *
 * Tip: its users are the `app` handle and every room not yet ended; when the last goes, it stops
 * watching the page's connectivity at that call. Its destructor ends nothing: it may run while the
 * io_context is destroyed, from the last operation pending there.
 */
class app_impl : public std::enable_shared_from_this<app_impl> {
public:
    /** The executor shared by the app and its rooms. */
    using executor_type = webcpp::asio::any_io_executor;

    /**
     * Copies the configuration into its resource, makes the endpoint key and the user identity,
     * sets aside what no peer could use, each kept as a report, and watches the page's
     * connectivity, which the relays follow while they are `relay_connectivity::automatic`.
     *
     * The endpoint key is drawn from the system's generator, 0.27.0's `createEndpointIdentity`,
     * and derives the self id; the user identity is the configuration's, or an ephemeral one made
     * here. When either cannot be made, @ref identity_failure says which, and the self id is
     * empty if the endpoint key is the one missing.
     *
     * Tip: `endpoint_scalar` and `user_scalar`, when not empty, are the endpoint key's and the
     * ephemeral user key's scalars in place of ones drawn: the library's own tests fix their
     * peers' keys, so that their ids are known; a scalar that is no valid key, zero for instance,
     * makes its key fail. `user_scalar` is unused when the configuration gives an identity.
     *
     * @param ex The executor every app and room event runs on.
     * @param factory The backend that makes the app's and the rooms' platform objects.
     * @param config The app's configuration, moved from.
     * @param endpoint_scalar The endpoint key's scalar, 32 bytes; empty draws one.
     * @param user_scalar The ephemeral user key's scalar, 32 bytes; empty draws one.
     */
    app_impl(executor_type ex, backend::factory& factory, app_config config,
             std::span<const std::uint8_t> endpoint_scalar = {},
             std::span<const std::uint8_t> user_scalar = {})
        : memory_(config.memory),
          ex_(std::move(ex)),
          factory_(factory),
          log_(std::move(config.log), memory_),
          app_id_(config.app_id, memory_),
          endpoint_(make_endpoint_identity(endpoint_scalar, memory_)),
          identity_(std::move(config.identity)),
          persistence_(identity_ ? identity_persistence::imported
                                 : identity_persistence::ephemeral),
          password_(config.password, memory_),
          room_keys_(std::allocate_shared<room_keys_cache>(allocator(), ex_, factory_, app_id_,
                                                           password_, memory_)),
          relay_urls_(config.relay_urls, memory_),
          ice_servers_(copy_ice_servers(config.ice_servers, memory_)),
          setup_reports_(memory_),
          errors_(allocator()),
          rooms_(memory_) {
        resolve_identity(user_scalar);
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
        watch_page();
    }

    /** The app state has one identity and cannot be copied. */
    app_impl(const app_impl&) = delete;
    /** The app state cannot be replaced by copying. */
    app_impl& operator=(const app_impl&) = delete;

    /** Wipes the app's copy of the password; it touches only its own member, so it may run while
     * the io_context is destroyed. */
    ~app_impl() { webcpp::trystero::detail::wipe(password_.data(), password_.size()); }

    /** The executor every app and room event runs on. */
    [[nodiscard]] const executor_type& get_executor() const { return ex_; }

    /** The backend that makes the app and room platform objects. */
    [[nodiscard]] backend::factory& factory() const { return factory_; }

    /** The app resource, which outlives its rooms and pending operations. */
    [[nodiscard]] std::pmr::memory_resource* memory() const { return memory_; }

    /** Names the user of a remote peer; the room copies the id during the call. */
    using user_resolver = handler<std::string_view(std::string_view peer_id)>;

    /** Names the user of each remote peer that becomes active in a room of the app from now on
     * with `resolver`, moved into the app resource; an empty one names the peer's own id. The
     * library's own tests group peers into users with it. */
    void resolve_users_with(user_resolver resolver) {
        user_resolver_ = user_resolver(std::allocator_arg, allocator(), std::move(resolver));
    }

    /** The user of the remote peer `peer_id`: until identities are derived from keys, the peer's
     * own id. */
    [[nodiscard]] std::string_view user_of(std::string_view peer_id) {
        return user_resolver_ ? user_resolver_(peer_id) : peer_id;
    }

    /** The shared logger, whose sentences use the app resource. */
    [[nodiscard]] const logger& log() const { return log_; }

    /** The application id shared by every room. */
    [[nodiscard]] std::string_view app_id() const { return app_id_; }

    /**
     * The local peer id shared by every room, derived from the endpoint key.
     *
     * @return The id; empty when the endpoint key could not be made.
     */
    [[nodiscard]] std::string_view self_id() const {
        return endpoint_ ? std::string_view(endpoint_->peer_id) : std::string_view();
    }

    /**
     * The endpoint key's compact public key.
     *
     * @return The key; empty when it could not be made.
     */
    [[nodiscard]] std::string_view endpoint_key() const {
        return endpoint_ ? std::string_view(endpoint_->compact) : std::string_view();
    }

    /**
     * The id of the user every room of the app presents.
     *
     * @return The id; empty when the ephemeral user identity could not be made.
     */
    [[nodiscard]] std::string_view user_id() const {
        return identity_ ? identity_->user_id() : std::string_view();
    }

    /**
     * Where the user identity comes from.
     *
     * @return `imported` for the configuration's, `ephemeral` for one made with the app.
     */
    [[nodiscard]] identity_persistence persistence() const { return persistence_; }

    /**
     * Why the app has no identity, the message 0.27.0's rooms report with `identity-failed`.
     *
     * Tip: 0.27.0 reports the error's own message when it has one, WebCrypto's for instance, and
     * these texts only as its fallbacks; the port reports the fallbacks always.
     *
     * @return "endpoint identity resolution failed" when the endpoint key could not be made,
     * "identity resolution failed" when the ephemeral user identity could not be; empty when both
     * exist.
     */
    [[nodiscard]] std::string_view identity_failure() const {
        if (!endpoint_) {
            return "endpoint identity resolution failed";
        }
        if (!identity_) {
            return "identity resolution failed";
        }
        return {};
    }

    /** The immutable password shared by this app's rooms. */
    [[nodiscard]] std::string_view password() const { return password_; }

    /** The keys of the app's rooms, derived once per room id when the app has a password. */
    [[nodiscard]] room_keys_cache& room_keys() const { return *room_keys_; }

    /** The relays configured once for the app. */
    [[nodiscard]] std::span<const std::pmr::string> relay_urls() const { return relay_urls_; }

    /** How the relays of the app's rooms connect: `relay_connectivity::automatic` until the
     * program sets another. */
    [[nodiscard]] relay_connectivity connectivity() const { return connectivity_; }

    /** Whether the relays of the app's rooms are held closed now: offline, or automatic since the
     * page's `offline` event. A room joined meanwhile opens none. */
    [[nodiscard]] bool relays_offline() const { return relays_offline_; }

    /**
     * Sets how the relays of the app's rooms connect, as 0.27.0's `setRelayConnectivity` sets it
     * for its page: `offline` holds every relay closed; `online` reconnects at once every relay not
     * connected, its reconnection delay back at its least, as each call does; `automatic` follows
     * the page's events from now on, and reconnects the relays a program held offline.
     *
     * Tip: a value outside the enumeration, which only a cast can make, is stored and taken as
     * `online`, where 0.27.0 throws "invalid relay connectivity mode".
     *
     * @return The connectivity now in place, `mode`.
     */
    relay_connectivity set_connectivity(relay_connectivity mode) {
        if (mode == relay_connectivity::automatic) {
            const bool held = connectivity_ != relay_connectivity::automatic && relays_offline_;
            connectivity_ = mode;
            watch_page();
            if (held) {
                set_relays_offline(false);
            }
            return connectivity_;
        }
        page_watch_.reset();
        connectivity_ = mode;
        set_relays_offline(mode == relay_connectivity::offline);
        return connectivity_;
    }

    /** The validated ICE servers used for every negotiation. */
    [[nodiscard]] std::span<const backend::ice_server> ice_servers() const { return ice_servers_; }

    /** What the constructor set aside, a sentence each, in its order. */
    [[nodiscard]] std::span<const std::pmr::string> setup_reports() const { return setup_reports_; }

    /** Adds a listener of each part of the configuration set aside, reported with
     * `error::invalid_configuration` and no room, and posts the delivery of the reports the
     * constructor made, which no listener could see then.
     *
     * Tip: the delivery reaches the listeners in place when it runs, and it hands each report over
     * once, so one added after it gets none. */
    subscription on_error(handler<void(const room_error&)> f, listen_options options) {
        auto out = errors_.add(std::move(f), options);
        if (delivered_reports_ != setup_reports_.size()) {
            post(ex_, memory_, [weak = weak_from_this()] {
                if (const auto self = weak.lock()) {
                    self->deliver_setup_reports();
                }
            });
        }
        return out;
    }

    /** `f` as a handler of the app's resource. */
    template <class Signature, class F>
    handler<Signature> bind(F&& f) const {
        return handler<Signature>(std::allocator_arg, allocator(), std::forward<F>(f));
    }

    /** Counts a user: the `app` handle, or a room not yet ended. */
    void add_user() { ++users_; }

    /** Releases a user; the last one's release ends the watch of the page's connectivity, so a
     * page event after it reaches neither the rooms nor the program's log. */
    void release_user() {
        if (users_ == 0) {
            return;
        }
        --users_;
        if (users_ == 0) {
            page_watch_.reset();
        }
    }

    /** How many rooms the registry holds. */
    [[nodiscard]] std::size_t counted_rooms() const { return rooms_.size(); }

    /** Registers a room by a weak reference: its control block is in the room's resource, so the
     * room unregisters before its state goes, in every way it ends, its destructor included. */
    void register_room(std::string_view room_id, const std::shared_ptr<room_endpoint>& room) {
        rooms_.insert_or_assign(std::pmr::string(room_id, memory_), entry{
                                                                        .room = room,
                                                                        .raw = room.get(),
                                                                    });
    }

    /** Forgets the room of `room_id`, if it is still `room`'s entry. */
    void unregister_room(std::string_view room_id, const room_endpoint* room) {
        if (const auto it = rooms_.find(room_id); it != rooms_.end() && it->second.raw == room) {
            rooms_.erase(it);
        }
    }

    /** The room of `room_id`, while it lives. */
    [[nodiscard]] std::shared_ptr<room_endpoint> find_room(std::string_view room_id) const {
        const auto it = rooms_.find(room_id);
        return it == rooms_.end() ? nullptr : it->second.room.lock();
    }

private:
    struct entry {
        std::weak_ptr<room_endpoint> room;
        const room_endpoint* raw;  // identity only, for unregister_room; never dereferenced
    };

    /** Hands every report not yet delivered to the listeners in place, one at a time: a listener
     * may remove itself, or another, from inside its call. */
    void deliver_setup_reports() {
        while (delivered_reports_ < setup_reports_.size() && !errors_.empty()) {
            const std::string_view what = setup_reports_[delivered_reports_++];
            room_error e;
            e.code = make_error_code(error::invalid_configuration);
            e.message = what;
            e.app_id = app_id_;
            errors_.emit(e);
        }
    }

    /**
     * Watches the page's connectivity through the backend, while the relays are automatic, unless
     * it is watched already.
     *
     * Tip: the handler names the app by its address: the watch is the app's, and the backend calls
     * no handler of a watch once it is destroyed.
     */
    void watch_page() {
        if (page_watch_) {
            return;
        }
        page_watch_ =
            factory_.watch_connectivity(bind<void(bool)>([this](bool online) {
                                            if (connectivity_ == relay_connectivity::automatic) {
                                                set_relays_offline(!online);
                                            }
                                        }),
                                        memory_);
    }

    /** Holds the relays of every room closed (`offline`), or reconnects them. */
    void set_relays_offline(bool offline) {
        relays_offline_ = offline;
        log_("app", offline ? "the relays go offline" : "the relays go online");
        // A room may leave from inside the call, through the log sink: the registry is read first.
        std::pmr::vector<std::shared_ptr<room_endpoint>> rooms(memory_);
        for (const auto& [id, e] : rooms_) {
            if (auto room = e.room.lock()) {
                rooms.push_back(std::move(room));
            }
        }
        for (const auto& room : rooms) {
            room->relays_offline(offline);
        }
    }

    /**
     * Makes the ephemeral user identity when the configuration gave none, and logs what could not
     * be made.
     *
     * @param user_scalar The ephemeral user key's scalar; empty draws one.
     */
    void resolve_identity(std::span<const std::uint8_t> user_scalar) {
        if (!identity_) {
            if (auto made = user_identity_access::make(user_scalar, memory_)) {
                identity_ = std::move(*made);
            }
        }
        if (const auto failure = identity_failure(); !failure.empty()) {
            log_("app", failure);
        }
    }

    void set_aside(std::string_view what) {
        log_("app", what);
        setup_reports_.emplace_back(what);
    }

    [[nodiscard]] std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    std::pmr::memory_resource* memory_;
    executor_type ex_;
    backend::factory& factory_;
    logger log_;
    std::pmr::string app_id_;
    std::optional<endpoint_identity> endpoint_;
    std::optional<user_identity> identity_;
    identity_persistence persistence_;
    std::pmr::string password_;
    std::shared_ptr<room_keys_cache> room_keys_;
    std::pmr::vector<std::pmr::string> relay_urls_;
    std::pmr::vector<backend::ice_server> ice_servers_;
    std::pmr::vector<std::pmr::string> setup_reports_;
    listeners<void(const room_error&)> errors_;
    std::size_t delivered_reports_ = 0;  // the setup reports handed to a handler, in their order
    std::pmr::map<std::pmr::string, entry, std::less<>> rooms_;
    std::size_t users_ = 0;
    relay_connectivity connectivity_ = relay_connectivity::automatic;
    bool relays_offline_ = false;
    user_resolver user_resolver_;
    // The page's connectivity, watched while the relays are automatic and the app has a user:
    // last, so that it ends first.
    std::shared_ptr<void> page_watch_;
};

}  // namespace webcpp::trystero::detail

#endif
