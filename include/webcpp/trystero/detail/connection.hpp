// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_CONNECTION_HPP
#define WEBCPP_TRYSTERO_DETAIL_CONNECTION_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/action_wire.hpp>
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>

#include <boost/assert.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/** What a connection asks of its app. Every handler is in the app's resource, and every one is
 * set. */
struct connection_callbacks {
    /** The remote advertised (`present`) or withdrew a room's token. */
    handler<void(std::string_view peer, std::string_view token, bool present)> presence;
    /** Whether to keep a frame of `token`, which no room bound to this connection has: the app
     * says no for a token of no room of it, and for a room whose dropping window runs. */
    handler<bool(std::string_view peer, std::string_view token)> keeps;
    /** A frame of `token` was kept. */
    handler<void(std::string_view peer, std::string_view token)> kept;
    /** The connection ended, with the reason; called once. */
    handler<void(std::string_view peer, std::string_view reason)> ended;
};

// clang-tidy reports a class's destructor at its first declaration: binding is final, and nothing
// destroys it through channel_link, whose destructor is protected.
class binding;  // NOLINT(cppcoreguidelines-virtual-class-destructor)

/**
 * One remote peer's connection, shared by every room of the app bound to it: it decodes each
 * frame's envelope, routes a room frame to the binding of its token, keeps a few frames for a room
 * of the app not bound yet, and records the rooms the remote advertises.
 *
 * | Frame                               | Done                                                      |
 * | does not decode                     | the connection ends, for every room                       |
 * | presence(1, T), T new               | recorded; a 65th distinct token ends the connection       |
 * | presence(0, T)                      | forgotten; frames kept for T dropped                      |
 * | room frame, T bound                 | to the binding of T                                       |
 * | room frame, T kept by the app       | kept: 64 per connection, all dropped 10 s after the first |
 * | room frame, any other T             | dropped                                                   |
 *
 * Tip: it holds plain pointers to its bindings, which remove themselves before they go, so that it
 * keeps nothing of a room's resource after the room has ended.
 */
class connection : public std::enable_shared_from_this<connection> {
public:
    /** The most distinct tokens a remote may advertise at once. */
    static constexpr std::size_t max_remote_rooms = 64;
    /** The most frames kept for rooms not bound yet. */
    static constexpr std::size_t max_kept_frames = 64;
    /** How long after the first kept frame every kept frame is dropped. */
    static constexpr std::chrono::milliseconds kept_frames_ttl{10'000};

    /** A connection to `peer` over its negotiated connection and open channel, in `memory`, the
     * app's resource.
     *
     * @pre Every handler of `cb` is set. */
    connection(backend::factory& factory, std::string_view peer,
               std::shared_ptr<backend::peer_connection> pc,
               std::shared_ptr<backend::data_channel> channel, connection_callbacks cb, logger log,
               std::pmr::memory_resource* memory)
        : factory_(factory),
          memory_(memory),
          peer_(peer, memory),
          pc_(std::move(pc)),
          channel_(std::move(channel)),
          cb_(std::move(cb)),
          log_(std::move(log)),
          remote_tokens_(memory),
          bound_(memory),
          kept_(memory),
          drain_order_(memory),
          attached_(memory) {
        BOOST_ASSERT(cb_.presence && cb_.keeps && cb_.kept && cb_.ended);
    }

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;

    /** Takes over the channel's and the connection's events, which replaces the handlers a room's
     * negotiation set, and advertises `tokens`. */
    void start(std::span<const std::string_view> tokens) {
        const auto weak = weak_from_this();
        const std::pmr::polymorphic_allocator<> allocator(memory_);
        channel_->set_handlers({
            .open = {},
            .message =
                {
                    std::allocator_arg,
                    allocator,
                    [weak](backend::bytes frame) {
                        if (const auto self = weak.lock()) {
                            self->on_message(std::move(frame));
                        }
                    },
                },
            .close =
                {
                    std::allocator_arg,
                    allocator,
                    [weak] {
                        if (const auto self = weak.lock()) {
                            self->end("data channel closed");
                        }
                    },
                },
            .buffered_amount_low =
                {
                    std::allocator_arg,
                    allocator,
                    [weak] {
                        if (const auto self = weak.lock()) {
                            self->on_drained(true);
                        }
                    },
                },
        });
        pc_->set_handlers({
            .ice_candidate = {},
            .data_channel = {},
            .state =
                {
                    std::allocator_arg,
                    allocator,
                    [weak](backend::connection_state state) {
                        if (const auto self = weak.lock()) {
                            self->on_state(state);
                        }
                    },
                },
        });
        log_("webrtc", "connection with ", peer_, " adopted");
        for (const auto token : tokens) {
            send_presence(token, true);
        }
    }

    /** The remote peer's id. */
    [[nodiscard]] std::string_view peer() const { return peer_; }

    /** Not stale, as Trystero says it: not ended, and its channel neither closing nor closed. A
     * connection is adopted once its channel is open, so "still opening" does not arise here. */
    [[nodiscard]] bool live() const { return !ended_ && channel_->is_open(); }

    /** How many frames are kept for rooms not bound yet. */
    [[nodiscard]] std::size_t kept_count() const { return kept_.size(); }

    /** Whether the remote advertises the room of `token`. */
    [[nodiscard]] bool advertised(std::string_view token) const {
        return remote_tokens_.contains(token);
    }

    /** Tells the remote this peer joined (`present`) or left the room of `token`. */
    void send_presence(std::string_view token, bool present) {
        if (!ended_ && channel_->is_open()) {
            channel_->send(protocol::presence_frame(token, present, memory_));
        }
    }

    /** Sends a frame as it is; false when the connection ended or its channel is no longer open.
     */
    bool send(std::span<const std::uint8_t> frame) {
        if (ended_ || !channel_->is_open()) {
            return false;
        }
        channel_->send(frame);
        return true;
    }

    /** The bytes the channel buffers; 0 once the connection ended. */
    [[nodiscard]] std::size_t buffered_amount() const {
        return ended_ ? 0 : channel_->buffered_amount();
    }

    /** Registers a room's binding by its token, and keeps the connection from idling; returns the
     * binding's id, which no other binding of this connection has.
     *
     * @pre No binding of the same token is bound: the app binds a room to a connection once. */
    std::uint64_t attach(binding& b);

    /** Removes a binding; with none left, ends the connection after a kick or a zero idle time,
     * else after `idle_timeout`. */
    void detach(binding& b, bool after_kick, std::chrono::milliseconds idle_timeout);

    /** Gives the binding a place in the drain order, once. */
    void wait_drained(const binding& b);

    /** The frames kept for `token`, in their order; they are no longer kept. */
    std::pmr::deque<backend::bytes> take_kept(std::string_view token) {
        std::pmr::deque<backend::bytes> out(memory_);
        for (auto it = kept_.begin(); it != kept_.end();) {
            if (it->first == token) {
                out.push_back(std::move(it->second));
                it = kept_.erase(it);
            } else {
                ++it;
            }
        }
        forget_kept_if_empty();
        return out;
    }

    /** Ends the connection for every room, once: its handlers cleared, its bindings let go, the
     * connection and its channel closed, `ended` called last. */
    void end(std::string_view reason);

private:
    friend class binding;
    void remove(binding& b);
    void on_message(backend::bytes&& frame);
    void on_state(backend::connection_state state);
    void on_drained(bool open);
    void keep(std::string_view token, backend::bytes&& frame);
    void forget_kept_if_empty();

    backend::factory& factory_;
    std::pmr::memory_resource* memory_;
    std::pmr::string peer_;
    std::shared_ptr<backend::peer_connection> pc_;
    std::shared_ptr<backend::data_channel> channel_;
    connection_callbacks cb_;
    logger log_;
    std::pmr::set<std::pmr::string, std::less<>> remote_tokens_;
    std::pmr::map<std::pmr::string, binding*, std::less<>> bound_;
    std::pmr::deque<std::pair<std::pmr::string, backend::bytes>> kept_;
    std::shared_ptr<backend::timer> kept_timer_;
    std::shared_ptr<backend::timer> idle_timer_;
    std::shared_ptr<backend::timer> disconnect_timer_;
    // Bindings are named by id, never by address: a binding made during a drain may take the block
    // of one destroyed in it, and must not inherit its turn.
    std::pmr::deque<std::uint64_t> drain_order_;
    std::pmr::map<std::uint64_t, binding*> attached_;  // by id: in the order of attachment
    std::uint64_t next_binding_id_ = 0;
    bool ended_ = false;
};

/**
 * A room's use of one connection: the action wire's link to a peer, whose frames carry the room's
 * token. It lives in the room's resource and owns its drain waiters; the connection holds only a
 * pointer to it, which it removes when it detaches.
 */
class binding final : public channel_link, public std::enable_shared_from_this<binding> {
public:
    /** Binds the room of `token` to `c`, which hands it every room frame of the token. */
    binding(const std::shared_ptr<connection>& c, std::string_view token,
            handler<void(backend::bytes&&, std::span<const std::uint8_t>)> deliver,
            std::pmr::memory_resource* room_memory)
        : connection_(c),
          token_(token, room_memory),
          deliver_(std::move(deliver)),
          waiters_(room_memory) {
        id_ = c->attach(*this);
    }

    binding(const binding&) = delete;
    binding& operator=(const binding&) = delete;

    /** Leaves the connection quietly: its waiters freed uncalled, its place in the connection
     * forgotten, and nothing ended, idled, closed or called back, since it may run while the
     * io_context is destroyed. */
    ~binding() {
        if (std::exchange(detached_, true)) {
            return;
        }
        waiters_.clear();
        deliver_ = {};
        if (const auto c = connection_.lock()) {
            c->remove(*this);
        }
    }

    /** The room's token. */
    [[nodiscard]] std::string_view token() const { return token_; }

    /** The binding's id on its connection, which no other binding of the connection has. */
    [[nodiscard]] std::uint64_t id() const { return id_; }

    /** Whether this binding is on `c`, by identity, alive or not: by the owner, so that a
     * connection made at the address of one destroyed is not taken for it. */
    [[nodiscard]] bool is_on(const std::shared_ptr<connection>& c) const {
        return !connection_.owner_before(c) && !c.owner_before(connection_);
    }

    /** The connection, until the binding detaches or the connection is gone. */
    [[nodiscard]] std::shared_ptr<connection> shared_connection() const {
        return detached_ ? nullptr : connection_.lock();
    }

    bool send(std::span<const std::uint8_t> room_frame) override {
        const auto c = shared_connection();
        return c && c->send(room_frame);
    }

    [[nodiscard]] std::size_t buffered_amount() const override {
        const auto c = shared_connection();
        return c ? c->buffered_amount() : 0;
    }

    void wait_drained(handler<void(bool)> done) override {
        const auto c = shared_connection();
        if (!c || !c->live()) {
            done(false);
            return;
        }
        if (c->buffered_amount() <= backend::data_channel::low_threshold) {
            done(true);
            return;
        }
        waiters_.push_back(std::move(done));
        c->wait_drained(*this);
    }

    /** A room frame of this binding's token, from the connection. */
    void deliver(backend::bytes&& frame, std::span<const std::uint8_t> action) {
        if (detached_) {
            return;
        }
        // The room may detach or destroy this binding from inside the call (a kick): it and its
        // delivery stay alive until the call returns. A lock, not shared_from_this,
        // which would abort without exceptions when no shared_ptr owns the binding.
        const auto self = weak_from_this().lock();
        deliver_(std::move(frame), action);
    }

    /** This binding's turn at a drain: each of its waiters, in its order. */
    void drained(bool open) {
        for (auto& w : std::exchange(waiters_, decltype(waiters_)(waiters_.get_allocator()))) {
            w(open);
        }
    }

    /** Leaves the connection: every waiter is told `false` and freed, no frame is delivered any
     * more, and the connection idles or ends when no binding is left.
     *
     * Tip: the delivery is not reset here, since it may be the very call that detaches; it goes
     * with the binding, which its room owns. */
    void detach(bool after_kick, std::chrono::milliseconds idle_timeout) {
        if (std::exchange(detached_, true)) {
            return;
        }
        // A waiter told `false` may release the room's last reference to this binding.
        const auto self = weak_from_this().lock();
        drained(false);
        if (const auto c = connection_.lock()) {
            c->detach(*this, after_kick, idle_timeout);
        }
    }

private:
    std::weak_ptr<connection> connection_;
    std::pmr::string token_;
    handler<void(backend::bytes&&, std::span<const std::uint8_t>)> deliver_;
    std::pmr::vector<handler<void(bool)>> waiters_;
    bool detached_ = false;
    std::uint64_t id_ = 0;
};

inline std::uint64_t connection::attach(binding& b) {
    BOOST_ASSERT(!bound_.contains(b.token()));
    bound_.insert_or_assign(std::pmr::string(b.token(), memory_), &b);
    const auto id = ++next_binding_id_;
    attached_.emplace(id, &b);
    idle_timer_.reset();
    return id;
}

inline void connection::wait_drained(const binding& b) {
    if (std::ranges::find(drain_order_, b.id()) == drain_order_.end()) {
        drain_order_.push_back(b.id());
    }
}

/** Forgets a binding, and nothing else: its maps, its place in the drain order. */
inline void connection::remove(binding& b) {
    if (const auto it = bound_.find(b.token()); it != bound_.end() && it->second == &b) {
        bound_.erase(it);
    }
    attached_.erase(b.id());
    std::erase(drain_order_, b.id());
}

inline void connection::detach(binding& b, bool after_kick,
                               std::chrono::milliseconds idle_timeout) {
    remove(b);
    if (ended_ || !bound_.empty()) {
        return;
    }
    if (after_kick || idle_timeout <= std::chrono::milliseconds::zero()) {
        end(after_kick ? "its last room kicked the peer" : "no room left");
        return;
    }
    idle_timer_ = factory_.make_timer(memory_);
    idle_timer_->start(
        idle_timeout,
        {std::allocator_arg, std::pmr::polymorphic_allocator<>(memory_), [weak = weak_from_this()] {
             if (const auto self = weak.lock()) {
                 self->end("idle");
             }
         }});
}

inline void connection::end(std::string_view reason_view) {
    if (ended_) {
        return;
    }
    ended_ = true;
    // The reason may be a view into what the calls below free.
    const std::pmr::string reason(reason_view, memory_);
    log_("webrtc", "connection with ", peer_, " ended: ", reason);
    const auto self = shared_from_this();
    idle_timer_.reset();
    kept_timer_.reset();
    disconnect_timer_.reset();
    channel_->set_handlers({});
    pc_->set_handlers({});
    channel_->close();
    pc_->close();
    // The bindings are let go before anyone is told: a room may destroy its binding, or itself,
    // from inside `ended` (Trystero's clear, C/shared-peer.mjs:170-180). Every attached binding is
    // told, those of the drain order first, then the others in the order they attached: an end
    // from inside a drain's turn leaves waiters of that drain in no drain order. A failure may
    // detach or destroy another binding, so each is looked up before its turn; one attached
    // meanwhile finds the connection not live, and fails at once.
    bound_.clear();
    auto order = std::exchange(drain_order_, std::pmr::deque<std::uint64_t>(memory_));
    for (const auto& [id, b] : attached_) {
        if (std::ranges::find(order, id) == order.end()) {
            order.push_back(id);
        }
    }
    for (const auto id : order) {
        if (const auto it = attached_.find(id); it != attached_.end()) {
            it->second->drained(false);
        }
    }
    attached_.clear();
    kept_ = decltype(kept_)(memory_);
    auto ended = std::move(cb_.ended);
    ended(peer_, reason);
}

inline void connection::on_message(backend::bytes&& frame) {
    const auto envelope = protocol::decode_envelope(frame);
    if (!envelope) {
        end("invalid or incompatible room frame");
        return;
    }
    // The app may end this connection from inside any call below: it stays alive until it returns.
    const auto self = shared_from_this();
    const std::string_view token = envelope->token;  // into `frame`, which outlives every use
    if (envelope->type == protocol::envelope::kind::presence) {
        if (envelope->present) {
            if (!remote_tokens_.contains(token) && remote_tokens_.size() >= max_remote_rooms) {
                end("too many advertised rooms");
                return;
            }
            remote_tokens_.emplace(token);
        } else {
            if (const auto it = remote_tokens_.find(token); it != remote_tokens_.end()) {
                remote_tokens_.erase(it);
            }
            std::erase_if(kept_, [token](const auto& k) { return k.first == token; });
            forget_kept_if_empty();
        }
        cb_.presence(peer_, token, envelope->present);
        return;
    }
    if (const auto it = bound_.find(token); it != bound_.end()) {
        const auto action = envelope->payload;
        it->second->deliver(std::move(frame), action);
        return;
    }
    if (cb_.keeps(peer_, token) && !ended_) {
        keep(token, std::move(frame));
    }
}

inline void connection::keep(std::string_view token, backend::bytes&& frame) {
    if (kept_.size() >= max_kept_frames) {
        return;
    }
    kept_.emplace_back(std::pmr::string(token, memory_), std::move(frame));
    if (!kept_timer_) {
        // All kept frames go 10 s after the first, as Trystero's (C/shared-peer.mjs:425-428).
        kept_timer_ = factory_.make_timer(memory_);
        kept_timer_->start(kept_frames_ttl,
                           {std::allocator_arg, std::pmr::polymorphic_allocator<>(memory_),
                            [weak = weak_from_this()] {
                                if (const auto self = weak.lock()) {
                                    self->kept_ = decltype(self->kept_)(self->memory_);
                                    self->kept_timer_.reset();
                                }
                            }});
    }
    // `token` may be a view into the frame just kept, which the app may take from inside the call.
    const std::pmr::string kept_token(kept_.back().first, memory_);
    cb_.kept(peer_, kept_token);
}

/** With nothing kept, the window stops, so that the next kept frame has 10 s of its own, as
 * Trystero's syncDataTimers (C/shared-peer.mjs:417-422). */
inline void connection::forget_kept_if_empty() {
    if (!kept_.empty()) {
        return;
    }
    kept_timer_.reset();
    // A cleared deque keeps blocks (libc++ keeps up to two): a new one frees them.
    kept_ = decltype(kept_)(memory_);
}

inline void connection::on_state(backend::connection_state state) {
    using backend::connection_state;
    if (state == connection_state::failed || state == connection_state::closed) {
        end(state == connection_state::failed ? "connection failed" : "connection closed");
    } else if (state == connection_state::disconnected) {
        // Trystero waits five seconds before treating a disconnection as the end.
        if (!disconnect_timer_) {
            disconnect_timer_ = factory_.make_timer(memory_);
        }
        disconnect_timer_->start(std::chrono::milliseconds(5000),
                                 {std::allocator_arg, std::pmr::polymorphic_allocator<>(memory_),
                                  [weak = weak_from_this()] {
                                      if (const auto self = weak.lock()) {
                                          self->end("connection lost");
                                      }
                                  }});
    } else if (disconnect_timer_) {
        disconnect_timer_->cancel();
    }
}

inline void connection::on_drained(bool open) {
    // Every binding waiting at this drain gets its turn, in the order it began to wait. A turn may
    // detach another binding of the snapshot, even destroy it: each is looked up by its id before
    // its turn, so that one made meanwhile, at the same address, is not taken for it.
    const auto self = shared_from_this();
    const auto order = std::exchange(drain_order_, std::pmr::deque<std::uint64_t>(memory_));
    for (const auto id : order) {
        if (const auto it = attached_.find(id); it != attached_.end()) {
            it->second->drained(open);
        }
    }
}

}  // namespace webcpp::trystero::detail

#endif
