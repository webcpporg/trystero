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
#include <webcpp/trystero/detail/post.hpp>
#include <webcpp/trystero/detail/random_id.hpp>
#include <webcpp/trystero/detail/room_keys_cache.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/options.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/asio/any_io_executor.hpp>  // lint-world: names the executor only

#include <boost/assert.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
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
    /** Whether the room is in its farewell grace period. */
    [[nodiscard]] virtual bool leaving() const = 0;
    /** Finishes its farewell and completes every pending leave. */
    virtual void finish_farewell() = 0;
    /** The remote advertised this room's token (`present`) or withdrew it. */
    virtual void presence_changed(std::string_view peer, bool present) = 0;
    /** Whether the connection keeps this room's frames from `peer` while the room is not bound to
     * it: false while the room drops them (its dropping window). */
    [[nodiscard]] virtual bool keeps_frames_from(std::string_view peer) const = 0;
    /** A frame of this room's token from `peer` was kept. */
    virtual void frame_kept(std::string_view peer) = 0;
    /** The app's connection `ended` to `peer` ended: not always the one the room is bound to, so a
     * room acts only on what it holds on `ended`. */
    virtual void connection_ended(const std::shared_ptr<connection>& ended, std::string_view peer,
                                  std::string_view reason) = 0;

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
 * destroyed, from the last operation pending there.
 */
class app_impl : public std::enable_shared_from_this<app_impl> {
public:
    /** The executor shared by the app and its rooms. */
    using executor_type = webcpp::asio::any_io_executor;

    /** Copies the configuration into its resource, draws the self id unless it names one, and sets
     * aside what no peer could use, each kept as a report. */
    app_impl(executor_type ex, backend::factory& factory, app_config config)
        : memory_(config.memory),
          ex_(std::move(ex)),
          factory_(factory),
          log_(std::move(config.log), memory_),
          app_id_(config.app_id, memory_),
          self_id_(config.self_id, memory_),
          password_(config.password, memory_),
          room_keys_(std::allocate_shared<room_keys_cache>(allocator(), ex_, factory_, app_id_,
                                                           password_, memory_)),
          relay_urls_(config.relay_urls, memory_),
          ice_servers_(copy_ice_servers(config.ice_servers, memory_)),
          idle_timeout_(config.idle_timeout),
          setup_reports_(memory_),
          rooms_(memory_),
          connections_(memory_),
          seconds_(memory_) {
        if (self_id_.size() > protocol::max_peer_id_bytes) {
            self_id_.clear();
            set_aside("self_id is longer than any peer accepts, so the app uses a random one");
        }
        // A signal writes the id as a JSON string, which holds UTF-8 alone, as any JavaScript
        // string is once encoded.
        if (!protocol::json::quotable(self_id_)) {
            self_id_.clear();
            set_aside("self_id is not UTF-8, which no peer reads, so the app uses a random one");
        }
        if (self_id_.empty()) {
            self_id_ = detail::random_id(factory_, protocol::peer_id_size, memory_);
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

    /** The app state has one identity and cannot be copied. */
    app_impl(const app_impl&) = delete;
    /** The app state cannot be replaced by copying. */
    app_impl& operator=(const app_impl&) = delete;

    /** The executor every app and room event runs on. */
    [[nodiscard]] const executor_type& get_executor() const { return ex_; }

    /** The backend that makes the app and room platform objects. */
    [[nodiscard]] backend::factory& factory() const { return factory_; }

    /** The app resource, which outlives its rooms and pending operations. */
    [[nodiscard]] std::pmr::memory_resource* memory() const { return memory_; }

    /** The shared logger, whose sentences use the app resource. */
    [[nodiscard]] const logger& log() const { return log_; }

    /** The application id shared by every room. */
    [[nodiscard]] std::string_view app_id() const { return app_id_; }

    /** The local peer id shared by every room. */
    [[nodiscard]] std::string_view self_id() const { return self_id_; }

    /** The immutable password shared by this app's rooms and authenticated connections. */
    [[nodiscard]] std::string_view password() const { return password_; }

    /** The keys of the app's rooms, derived once per room id when the app has a password. */
    [[nodiscard]] room_keys_cache& room_keys() const { return *room_keys_; }

    /** The relays configured once for the app. */
    [[nodiscard]] std::span<const std::pmr::string> relay_urls() const { return relay_urls_; }

    /** How long the higher id holds a second connection to a peer that neither sends on it nor
     * closes it (`hold_second`). */
    static constexpr std::chrono::milliseconds second_wait_higher{2'000};
    /** How long the lower id holds a second connection to a peer that neither sends on the first
     * nor closes one of them (`hold_second`). */
    static constexpr std::chrono::milliseconds second_wait_lower{10'000};

    /** The validated ICE servers used for every negotiation. */
    [[nodiscard]] std::span<const backend::ice_server> ice_servers() const { return ice_servers_; }

    /** The lifetime of a connection left with no bindings after a departure. */
    [[nodiscard]] std::chrono::milliseconds idle_timeout() const { return idle_timeout_; }

    /** What the constructor set aside, a sentence each, in its order. */
    [[nodiscard]] std::span<const std::pmr::string> setup_reports() const { return setup_reports_; }

    /** Sets what is called with each part of the configuration set aside, with an empty peer id
     * and `error::invalid_configuration`, and posts the delivery of the reports the constructor
     * made, which no handler could see then.
     *
     * Tip: the delivery calls the handler current when it runs, so one replaced before it gets
     * none, and it hands each report over once, so one set after it gets none either. */
    void on_error(handler<void(std::string_view, boost::system::error_code, std::string_view)> f) {
        if (!f) {
            on_error_ = nullptr;
            return;
        }
        on_error_ = std::allocate_shared<decltype(f)>(allocator(), std::move(f));
        if (delivered_reports_ == setup_reports_.size()) {
            return;
        }
        post(ex_, memory_, [weak = weak_from_this()] {
            if (const auto self = weak.lock()) {
                self->deliver_setup_reports();
            }
        });
    }

    /** `f` as a handler of the app's resource. */
    template <class Signature, class F>
    handler<Signature> bind(F&& f) const {
        return handler<Signature>(std::allocator_arg, allocator(), std::forward<F>(f));
    }

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
        auto seconds = std::exchange(seconds_, decltype(seconds_)(memory_));
        for (auto& [peer, second] : seconds) {
            close_second(*second);
        }
    }

    /** How many rooms the registry holds. */
    [[nodiscard]] std::size_t counted_rooms() const { return rooms_.size(); }

    /** Registers a room by a weak reference: its control block is in the room's resource, so the
     * room unregisters before its state goes, in every way it ends, its destructor included. */
    void register_room(std::string_view room_id, std::string_view token,
                       const std::shared_ptr<room_endpoint>& room) {
        rooms_.insert_or_assign(std::pmr::string(token, memory_),
                                entry{
                                    .room_id = std::pmr::string(room_id, memory_),
                                    .room = room,
                                    .raw = room.get(),
                                });
    }

    /** Forgets the room of `token`, if it is still `room`'s entry. */
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
     * a live one to that peer, the existing one is returned, as in Trystero's
     * dist/shared-peer.mjs:105-108, and the new one is held as the peer's second connection
     * (`hold_second`).
     *
     * Tip: a stale connection to the peer, one whose channel is closing, ends first, as Trystero
     * clears it (dist/shared-peer.mjs:142-149): the rooms hear its end before the new
     * connection is adopted, and a room may adopt one to the peer from inside that call, which is
     * then the one shared, or ends in its turn if it is stale by then. One whose end is already
     * under way, further up the stack, is let go: it ends no second time. */
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
    /** A second connection to a peer, held unused until the peer shows which of the two it keeps:
     * the frames its channel received meanwhile, in their order, and its deadline. */
    struct second_connection {
        std::pmr::string peer;
        std::shared_ptr<backend::peer_connection> pc;
        std::shared_ptr<backend::data_channel> channel;
        std::pmr::deque<backend::bytes> frames;
        std::shared_ptr<backend::timer> deadline;
    };

    // Its implicit move assignment, which insert_or_assign uses, may allocate: a std::pmr member
    // copies when the two resources differ, which bugprone-exception-escape reports. Every entry
    // is in the app's resource, so it never does.
    struct entry {  // NOLINT(bugprone-exception-escape)
        std::pmr::string room_id;
        std::weak_ptr<room_endpoint> room;
        const room_endpoint* raw;  // identity only, for unregister_room; never dereferenced
    };

    /** Closes the new connection and returns the live one to `peer`, if any; else adopts it. */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    std::shared_ptr<connection> share_or_adopt(std::string_view peer,
                                               std::shared_ptr<backend::peer_connection> pc,
                                               std::shared_ptr<backend::data_channel> channel) {
        // NOLINTEND(performance-unnecessary-value-param)
        if (auto existing = find(peer)) {
            hold_or_close(peer, *existing, std::move(pc), std::move(channel));
            return existing;
        }
        const auto weak = weak_from_this();
        // The connection whose `ended` this is, set once it exists: its end erases its own entry
        // only, never one a room adopted since (Trystero's owns(), dist/shared-peer.mjs:210-212).
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
                .used = bind<void(std::string_view)>([weak](std::string_view p) {
                    if (const auto self = weak.lock()) {
                        if (const auto it = self->seconds_.find(p); it != self->seconds_.end()) {
                            self->forget_second(*it->second, "the peer keeps the first");
                        }
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

    /** A connection to `peer` opened while `first` is live: held as the second
     * (`hold_second`) while the peer has sent nothing on the first and none is held, else
     * closed. */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    void hold_or_close(std::string_view peer, const connection& first,
                       std::shared_ptr<backend::peer_connection> pc,
                       std::shared_ptr<backend::data_channel> channel) {
        // NOLINTEND(performance-unnecessary-value-param)
        if (!first.used() && !seconds_.contains(peer)) {
            hold_second(peer, std::move(pc), std::move(channel));
            return;
        }
        channel->set_handlers({});
        pc->set_handlers({});
        channel->close();
        pc->close();
        log_("webrtc", "another connection with ", peer,
             " closed: ", first.used() ? "the peer keeps the first" : "a second is held already");
    }

    /** Holds a second connection to `peer`, opened while the first was live and before the peer
     * sent anything on the first, until the peer shows which of the two it keeps, rather than
     * closing it at once as Trystero does.
     *
     * Each side keeps the connection whose channel opened first on its side (Trystero's
     * dist/shared-peer.mjs:105-108), so the two sides of a crossed pair keep different ones, and
     * a side sends only on the one it keeps. A frame on the first shows the peer keeps it: the
     * second is closed, as the peer closes its own. Trystero JS closes its other connection at
     * once, so that in a crossed pair the first ends, and the second, the one the peer keeps,
     * replaces it (`on_connection_end`) with the frames it received meanwhile. A peer that holds
     * its second, as this app does, closes neither, so the higher id gives way: a frame on its
     * second makes the second replace its first (`switch_to_second`), whose end closes the lower
     * id's second. A second the peer neither sends on nor closes is closed after
     * `second_wait_higher` by the higher id, and after `second_wait_lower` by the lower id.
     *
     * Closing the second at once, as Trystero does, closes in a crossed pair the connection the
     * peer keeps, just as the peer starts sending on it. libdatachannel ends an association with
     * a stream reset and a SHUTDOWN, and its ABORT comes a moment later, where a browser's ABORT
     * follows its reset at once: Chrome fails a send made in between and closes the channel with
     * that failure, which Trystero JS reports as a peer error. */
    void hold_second(std::string_view peer, std::shared_ptr<backend::peer_connection> pc,
                     std::shared_ptr<backend::data_channel> channel) {
        auto second = std::allocate_shared<second_connection>(
            allocator(), second_connection{
                             .peer = std::pmr::string(peer, memory_),
                             .pc = std::move(pc),
                             .channel = std::move(channel),
                             .frames = std::pmr::deque<backend::bytes>(memory_),
                             .deadline = factory_.make_timer(memory_),
                         });
        const auto weak = weak_from_this();
        const std::weak_ptr<second_connection> held = second;
        second->channel->set_handlers({
            .open = {},
            .message = bind<void(backend::bytes)>([weak, held](backend::bytes frame) {
                const auto self = weak.lock();
                const auto s = held.lock();
                if (!self || !s) {
                    return;
                }
                if (s->frames.size() >= connection::max_kept_frames) {
                    self->forget_second(*s, "it received more frames than are kept");
                    return;
                }
                s->frames.push_back(std::move(frame));
                // Posted: the switch replaces the handlers this call runs in.
                if (std::string_view(self->self_id_) > s->peer && s->frames.size() == 1) {
                    post(self->ex_, self->memory_, [weak, held] {
                        const auto app = weak.lock();
                        if (const auto second = held.lock(); app && second) {
                            app->switch_to_second(*second);
                        }
                    });
                }
            }),
            .close = bind<void()>([weak, held] {
                const auto self = weak.lock();
                const auto s = held.lock();
                if (self && s) {
                    self->forget_second(*s, "its channel closed");
                }
            }),
            .buffered_amount_low = {},
        });
        second->pc->set_handlers({
            .ice_candidate = {},
            .data_channel = {},
            .state = bind<void(backend::connection_state)>(
                [weak, held](backend::connection_state state) {
                    const auto self = weak.lock();
                    const auto s = held.lock();
                    if (self && s &&
                        (state == backend::connection_state::failed ||
                         state == backend::connection_state::closed)) {
                        self->forget_second(*s, "its connection ended");
                    }
                }),
        });
        second->deadline->start(
            std::string_view(self_id_) > peer ? second_wait_higher : second_wait_lower,
            bind<void()>([weak, held] {
                const auto self = weak.lock();
                const auto s = held.lock();
                if (self && s) {
                    self->forget_second(*s, "the peer kept neither");
                }
            }));
        seconds_.insert_or_assign(std::pmr::string(peer, memory_), second);
        log_("webrtc", "a second connection with ", peer,
             " held until the peer shows which one it keeps");
    }

    /** The peer sends on the second connection the higher id holds, which it keeps: the second
     * replaces the first, which ends, and takes the frames it received. As in
     * `on_connection_end`, the second takes the first's place before the rooms hear of the end,
     * and its frames come after. */
    void switch_to_second(second_connection& s) {
        const auto it = seconds_.find(s.peer);
        if (it == seconds_.end() || it->second.get() != &s) {
            return;
        }
        const auto second = it->second;
        seconds_.erase(it);
        const std::pmr::string peer(second->peer, memory_);
        second->deadline.reset();
        auto frames = std::exchange(second->frames, std::pmr::deque<backend::bytes>(memory_));
        std::shared_ptr<connection> first;
        if (const auto at = connections_.find(peer); at != connections_.end()) {
            first = at->second;
            connections_.erase(at);
        }
        log_("webrtc", "the second connection with ", peer,
             " replaces the first: the peer keeps it");
        const auto replacement = share_or_adopt(peer, second->pc, second->channel);
        if (first) {
            first->end("the peer keeps the second connection");
        }
        replacement->replay(std::move(frames));
    }

    /** Closes a held second connection and forgets it, if it is still the one held. */
    void forget_second(second_connection& s, std::string_view why) {
        const auto it = seconds_.find(s.peer);
        if (it == seconds_.end() || it->second.get() != &s) {
            return;
        }
        const auto keep = it->second;  // `s` lives until this call returns
        seconds_.erase(it);
        log_("webrtc", "the second connection with ", keep->peer, " closed: ", why);
        close_second(*keep);
    }

    /** Closes a second connection without reporting its close. */
    static void close_second(second_connection& s) {
        s.deadline.reset();
        s.channel->set_handlers({});
        s.pc->set_handlers({});
        s.channel->close();
        s.pc->close();
    }

    [[nodiscard]] std::shared_ptr<room_endpoint> room_of(std::string_view token) const {
        const auto it = rooms_.find(token);
        return it == rooms_.end() ? nullptr : it->second.room.lock();
    }

    /** Forgets `ended`, unless another connection to the peer took its place, and tells every
     * room: those of a connection the last user's release took out of the registry too. */
    void on_connection_end(const std::shared_ptr<connection>& ended, std::string_view peer_view,
                           std::string_view reason_view) {
        // The slot is filled before the connection starts, and its end holds it alive.
        BOOST_ASSERT(ended);
        const std::pmr::string peer(peer_view, memory_);
        const std::pmr::string reason(reason_view, memory_);
        if (const auto it = connections_.find(peer);
            it != connections_.end() && it->second == ended) {
            connections_.erase(it);
        }
        // The second connection held to the peer replaces the first when the peer or the network
        // ended the first, which is how Trystero JS keeps the second of a crossed pair; it is
        // closed when this side ended the first. It takes the first's place before the rooms hear
        // of the end, so that none looks for the peer again, and its frames come after, once the
        // rooms let the peer go, so that they bind it again over the second.
        std::shared_ptr<connection> replacement;
        std::pmr::deque<backend::bytes> frames(memory_);
        if (const auto it = seconds_.find(peer); it != seconds_.end()) {
            const auto second = it->second;
            seconds_.erase(it);
            if (ended->ended_by_transport() && second->channel->is_open() && !find(peer)) {
                second->deadline.reset();
                frames = std::move(second->frames);
                log_("webrtc", "the second connection with ", peer, " replaces the first");
                replacement = share_or_adopt(peer, second->pc, second->channel);
            } else {
                log_("webrtc", "the second connection with ", peer, " closed: the first ended");
                close_second(*second);
            }
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
            room->connection_ended(ended, peer, reason);
        }
        if (replacement) {
            replacement->replay(std::move(frames));
        }
    }

    /** Hands every report not yet delivered to the current handler, one at a time: a handler may
     * replace itself, or clear itself, from inside its call. */
    void deliver_setup_reports() {
        while (delivered_reports_ < setup_reports_.size() && on_error_) {
            const auto f = on_error_;
            const std::string_view what = setup_reports_[delivered_reports_++];
            (*f)("", make_error_code(error::invalid_configuration), what);
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
    std::pmr::string self_id_;
    std::pmr::string password_;
    std::shared_ptr<room_keys_cache> room_keys_;
    std::pmr::vector<std::pmr::string> relay_urls_;
    std::pmr::vector<backend::ice_server> ice_servers_;
    std::chrono::milliseconds idle_timeout_;
    std::pmr::vector<std::pmr::string> setup_reports_;
    std::shared_ptr<handler<void(std::string_view, boost::system::error_code, std::string_view)>>
        on_error_;
    std::size_t delivered_reports_ = 0;  // the setup reports handed to a handler, in their order
    std::pmr::map<std::pmr::string, entry, std::less<>> rooms_;
    std::pmr::map<std::pmr::string, std::shared_ptr<connection>, std::less<>> connections_;
    std::pmr::map<std::pmr::string, std::shared_ptr<second_connection>, std::less<>> seconds_;
    std::size_t users_ = 0;
};

}  // namespace webcpp::trystero::detail

#endif
