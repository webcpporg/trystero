// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_PEER_LINK_HPP
#define WEBCPP_TRYSTERO_DETAIL_PEER_LINK_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/action_wire.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/action_wire.hpp>

#include <boost/assert.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace webcpp::trystero::detail {

/**
 * One room's connection to one remote peer, 0.27.0's peer (`peer.ts`): the negotiated connection
 * and its open data channel, whose frames are the room's action frames, written bare
 * (`peer-transport.ts`'s `sendFrame`). It lives in the room's resource, and ends with the room or
 * with the peer: no other room ever uses it.
 *
 * | Frame                          | Done                                                      |
 * | before `bind`, at most 17 KiB  | kept, in order, at most 64, the first starting a 10 s     |
 * |                                | deadline                                                  |
 * | before `bind`, longer, a 65th  | the link ends, as 0.27.0's `failData`                     |
 * | or the deadline passed         |                                                           |
 * | after `bind`, at most 64 KiB   | to `callbacks::frame`                                     |
 * | after `bind`, longer           | the link ends, as 0.27.0's `failData`                     |
 *
 * The connection's state ends it as 0.27.0's does: `failed` and `closed` at once, `disconnected`
 * after a grace of 5 s that a return to `connected` cancels; so does its channel's close.
 *
 * Tip: its handlers name it weakly, so that a link the room let go reaches nothing; its destructor
 * ends nothing and calls nothing, since it may run while the io_context is destroyed.
 */
class peer_link final : public channel_link, public std::enable_shared_from_this<peer_link> {
    /** Keeps the constructor for `make`, while `std::allocate_shared` can still call it. */
    struct private_tag {
        explicit private_tag() = default;
    };

public:
    /** What a link hands its room once bound. Both are set. */
    struct callbacks {
        /** An action frame, in arrival order. */
        handler<void(backend::bytes frame)> frame;
        /** The link ended, with the reason; called once. */
        handler<void(std::string_view reason)> ended;
    };

    /** The most frames kept before `bind`, 0.27.0's `maxQueuedDataFrames`. */
    static constexpr std::size_t max_pending_frames = protocol::max_queued_data_frames;
    /** How long after the first kept frame the link waits for `bind`, 0.27.0's
     * `pendingDataTimeoutMs`. */
    static constexpr std::chrono::milliseconds pending_frames_timeout =
        protocol::pending_data_timeout;
    /** The longest frame kept before `bind`, 0.27.0's `maxRoomFrameBytes`: an action frame and
     * its overlay's header. */
    static constexpr std::size_t max_unbound_frame_bytes = protocol::max_room_frame_bytes;
    /** The longest frame after `bind`, 0.27.0's `maxBulkFrameBytes`. */
    static constexpr std::size_t max_bound_frame_bytes = protocol::max_bulk_frame_bytes;
    /** How long a `disconnected` connection has to come back, 0.27.0's grace. */
    static constexpr std::chrono::milliseconds disconnected_grace{5'000};

    /**
     * A link to `peer` over its negotiated connection and open channel, made in `memory`, the
     * room's: it takes over their events at once, keeping every frame until `bind`.
     *
     * @param peer The remote peer's id, copied.
     * @param pc The negotiated connection.
     * @param dc Its open data channel.
     * @param factory The backend, for the link's timers; it outlives the link.
     * @param memory The room's resource, which outlives the link.
     * @return The link.
     */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    static std::shared_ptr<peer_link> make(std::string_view peer,
                                           std::shared_ptr<backend::peer_connection> pc,
                                           std::shared_ptr<backend::data_channel> dc,
                                           backend::factory& factory,
                                           std::pmr::memory_resource* memory) {
        // NOLINTEND(performance-unnecessary-value-param)
        auto link = std::allocate_shared<peer_link>(std::pmr::polymorphic_allocator<>(memory),
                                                    private_tag{}, peer, std::move(pc),
                                                    std::move(dc), factory, memory);
        link->start();
        return link;
    }

    /** For `make` alone, through its tag. */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    peer_link(private_tag /*tag*/, std::string_view peer,
              std::shared_ptr<backend::peer_connection> pc,
              std::shared_ptr<backend::data_channel> dc, backend::factory& factory,
              std::pmr::memory_resource* memory)
        // NOLINTEND(performance-unnecessary-value-param)
        : factory_(factory),
          memory_(memory),
          peer_(peer, memory),
          pc_(std::move(pc)),
          dc_(std::move(dc)),
          pending_(memory),
          waiters_(memory),
          reason_(memory) {}

    /** A link has one identity and cannot be copied. */
    peer_link(const peer_link&) = delete;
    /** A link cannot be replaced by copying. */
    peer_link& operator=(const peer_link&) = delete;

    /** Ends nothing: what it holds goes quietly, its handlers reaching nothing any more. */
    ~peer_link() = default;

    /** The remote peer's id. */
    [[nodiscard]] std::string_view peer() const { return peer_; }

    /** Not ended, and its channel open: 0.27.0's peer not stale. */
    [[nodiscard]] bool live() const { return !ended_ && dc_->is_open(); }

    /** Whether `bind` ran. */
    [[nodiscard]] bool bound() const { return bound_; }

    /** How many frames wait for `bind`. */
    [[nodiscard]] std::size_t pending_count() const { return pending_.size(); }

    /**
     * Hands the frames to `cb` from now on, those kept before it first, in their order; a link
     * that ended before calls `cb.ended` at once, with its reason. Once.
     *
     * @param cb The room's callbacks, both set.
     */
    void bind(callbacks cb) {
        BOOST_ASSERT(cb.frame && cb.ended);
        BOOST_ASSERT(!bound_);
        bound_ = true;
        cb_ = std::move(cb);
        if (ended_) {
            const std::pmr::string reason(reason_, memory_);
            auto ended = std::exchange(cb_.ended, {});
            cb_.frame = {};
            ended(reason);
            return;
        }
        pending_timer_.reset();
        auto frames = std::exchange(pending_, std::pmr::deque<backend::bytes>(memory_));
        // The room may end the link, or let it go, from inside a frame's call, as 0.27.0 stops
        // replaying once its peer is no longer bound.
        const auto self = weak_from_this().lock();
        for (auto& frame : frames) {
            if (ended_) {
                return;
            }
            cb_.frame(std::move(frame));
        }
    }

    /**
     * Ends the link, once: the channel and the connection closed, their handlers cleared, the kept
     * frames dropped, every drain waiter told `false`, then `callbacks::ended` called, when bound.
     *
     * @param reason Why, read during the call.
     */
    void end(std::string_view reason) {
        if (ended_) {
            return;
        }
        ended_ = true;
        reason_ = reason;  // `reason` may be a view into what the calls below free
        const auto self = weak_from_this().lock();
        pending_timer_.reset();
        disconnect_timer_.reset();
        dc_->set_handlers({});
        pc_->set_handlers({});
        dc_->close();
        pc_->close();
        pending_ = std::pmr::deque<backend::bytes>(memory_);
        drained(false);
        if (!bound_) {
            return;
        }
        auto ended = std::exchange(cb_.ended, {});
        cb_.frame = {};
        ended(reason_);
    }

    /** Sends an action frame as it is; false when the link ended or its channel is not open. */
    bool send(std::span<const std::uint8_t> frame) override {
        if (!live()) {
            return false;
        }
        dc_->send(frame);
        return true;
    }

    /** The bytes the channel buffers; 0 once the link ended. */
    [[nodiscard]] std::size_t buffered_amount() const override {
        return ended_ ? 0 : dc_->buffered_amount();
    }

    /** `protocol::max_bulk_frame_bytes` when the connection's `max_message_size` is at least that,
     * else `protocol::max_action_frame_bytes`, as 0.27.0's getter reads `sctp.maxMessageSize` at
     * each transfer. */
    [[nodiscard]] std::size_t max_bulk_frame_bytes() const override {
        const auto size = pc_->max_message_size();
        return size && *size >= protocol::max_bulk_frame_bytes ? protocol::max_bulk_frame_bytes
                                                               : protocol::max_action_frame_bytes;
    }

    /** Calls `done(true)` once the channel buffers at most `data_channel::low_threshold`, at once
     * when it does already, or `done(false)` when the link ends first or has ended. */
    void wait_drained(handler<void(bool)> done) override {
        if (!live()) {
            done(false);
            return;
        }
        if (dc_->buffered_amount() <= backend::data_channel::low_threshold) {
            done(true);
            return;
        }
        waiters_.push_back(std::move(done));
    }

private:
    /** Takes over the channel's and the connection's events, which replaces the handlers the
     * negotiation set. */
    void start() {
        const auto weak = weak_from_this();
        const std::pmr::polymorphic_allocator<> allocator(memory_);
        dc_->set_handlers({
            .open = {},
            .message =
                {
                    std::allocator_arg,
                    allocator,
                    [weak](backend::bytes frame) {
                        if (const auto self = weak.lock()) {
                            self->on_frame(std::move(frame));
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
                            self->drained(true);
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
    }

    /** A frame from the channel, kept or handed over within 0.27.0's bounds (`peer.ts`'s
     * `onmessage`). */
    void on_frame(backend::bytes&& frame) {
        if (ended_) {
            return;
        }
        if (bound_) {
            if (frame.size() > max_bound_frame_bytes) {
                fail_data();
                return;
            }
            cb_.frame(std::move(frame));
            return;
        }
        if (frame.size() > max_unbound_frame_bytes || pending_.size() >= max_pending_frames) {
            fail_data();
            return;
        }
        pending_.push_back(std::move(frame));
        if (!pending_timer_) {
            pending_timer_ = factory_.make_timer(memory_);
            pending_timer_->start(pending_frames_timeout,
                                  {std::allocator_arg, std::pmr::polymorphic_allocator<>(memory_),
                                   [weak = weak_from_this()] {
                                       if (const auto self = weak.lock()) {
                                           self->fail_data();
                                       }
                                   }});
        }
    }

    /** 0.27.0's `failData`, which warns "excessive pre-handler data; disconnecting peer". */
    void fail_data() { end("excessive pre-handler data"); }

    /** Ends a failed or closed connection at once, and a disconnected one after its grace, which a
     * later disconnection does not extend, as 0.27.0 starts it only from an open connection. */
    void on_state(backend::connection_state state) {
        using backend::connection_state;
        if (state == connection_state::failed || state == connection_state::closed) {
            end(state == connection_state::failed ? "connection failed" : "connection closed");
        } else if (state == connection_state::disconnected) {
            if (in_grace_) {
                return;
            }
            if (!disconnect_timer_) {
                disconnect_timer_ = factory_.make_timer(memory_);
            }
            in_grace_ = true;
            disconnect_timer_->start(
                disconnected_grace, {std::allocator_arg, std::pmr::polymorphic_allocator<>(memory_),
                                     [weak = weak_from_this()] {
                                         if (const auto self = weak.lock()) {
                                             self->in_grace_ = false;
                                             self->end("connection lost");
                                         }
                                     }});
        } else if (in_grace_) {
            in_grace_ = false;
            disconnect_timer_->cancel();
        }
    }

    /** Gives every waiter of this drain its turn, in the order it began to wait: one that waits
     * again from its call waits for the next drain. */
    void drained(bool open) {
        // A waiter may let the room release its last reference to the link.
        const auto self = weak_from_this().lock();
        const auto count = waiters_.size();
        for (std::size_t i = 0; i < count && !waiters_.empty(); ++i) {
            auto waiter = std::move(waiters_.front());
            waiters_.pop_front();
            waiter(open);
        }
        if (waiters_.empty()) {
            // A cleared deque keeps blocks (libc++ keeps up to two): a new one frees them.
            waiters_ = std::pmr::deque<handler<void(bool)>>(memory_);
        }
    }

    backend::factory& factory_;
    std::pmr::memory_resource* memory_;
    std::pmr::string peer_;
    std::shared_ptr<backend::peer_connection> pc_;
    std::shared_ptr<backend::data_channel> dc_;
    callbacks cb_;
    std::pmr::deque<backend::bytes> pending_;
    std::pmr::deque<handler<void(bool)>> waiters_;
    std::shared_ptr<backend::timer> pending_timer_;
    std::shared_ptr<backend::timer> disconnect_timer_;
    std::pmr::string reason_;
    bool bound_ = false;
    bool ended_ = false;
    /** Whether a disconnection's grace runs on `disconnect_timer_`. */
    bool in_grace_ = false;
};

}  // namespace webcpp::trystero::detail

#endif
