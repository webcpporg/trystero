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
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>

#include <chrono>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/**
 * A connected peer: its data channel, in the room envelope. It announces the room's presence when
 * it starts, hands every action frame for this room to the room together with the frame it arrived
 * in, and reports the end of the connection once. What it sends is already a room frame
 * (action_wire writes the envelope).
 *
 * Tip: a frame for another room's token is ignored rather than buffered; this library does not
 * share a connection between rooms, so no other room of ours can claim it.
 */
class peer_link final : public std::enable_shared_from_this<peer_link>, public channel_link {
public:
    /** What the link reports to its room. */
    struct callbacks {
        /** An action frame: `action` is a view into `frame`, which the receiver may keep. */
        handler<void(std::string_view peer, backend::bytes&& frame,
                     std::span<const std::uint8_t> action)>
            frame;
        /** The connection ended, with the reason; called once. */
        handler<void(std::string_view peer, std::string_view reason)> closed;
    };

    /** A link over a connected peer's connection and data channel, in `memory`. */
    peer_link(backend::factory& factory, std::string_view peer, std::string_view token,
              std::shared_ptr<backend::peer_connection> pc,
              std::shared_ptr<backend::data_channel> channel, callbacks cb, logger log,
              std::pmr::memory_resource* memory)
        : factory_(factory),
          memory_(memory),
          peer_(peer, memory),
          token_(token, memory),
          pc_(std::move(pc)),
          channel_(std::move(channel)),
          cb_(std::move(cb)),
          log_(std::move(log)),
          drain_waiters_(memory) {}

    /** Takes over the channel's and the connection's events, and announces the room's presence. */
    void start() {
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
        send_presence(true);
    }

    /** The peer's id. */
    std::string_view peer() const { return peer_; }

    /** Sends a room frame; false when the channel is no longer open. */
    bool send(std::span<const std::uint8_t> room_frame) override {
        if (ended_ || !channel_->is_open()) {
            return false;
        }
        channel_->send(room_frame);
        return true;
    }

    /** The bytes the channel buffers; 0 once the link ended. */
    std::size_t buffered_amount() const override {
        return ended_ ? 0 : channel_->buffered_amount();
    }

    /** Calls `done(true)` once the channel's buffer is below Trystero's threshold, `done(false)` if
     * it closes first. */
    void wait_drained(handler<void(bool)> done) override {
        if (ended_) {
            done(false);
            return;
        }
        if (channel_->buffered_amount() <= backend::data_channel::low_threshold) {
            done(true);
            return;
        }
        drain_waiters_.push_back(std::move(done));
    }

    /** Says the room is left and closes the connection without reporting it: the room already
     * knows. */
    void close() {
        if (ended_) {
            return;
        }
        send_presence(false);
        ended_ = true;
        release();
        on_drained(false);
    }

private:
    void send_presence(bool present) {
        if (channel_->is_open()) {
            channel_->send(protocol::presence_frame(token_, present, memory_));
        }
    }

    void on_message(backend::bytes&& frame) {
        const auto envelope = protocol::decode_envelope(frame);
        if (!envelope) {
            end("invalid or incompatible room frame");
            return;
        }
        if (envelope->token != token_) {
            return;
        }
        if (envelope->type == protocol::envelope::kind::presence) {
            if (!envelope->present) {
                end("peer left the room");
            }
            return;
        }
        const auto action = envelope->payload;
        cb_.frame(peer_, std::move(frame), action);
    }

    void on_state(backend::connection_state state) {
        using backend::connection_state;
        if (state == connection_state::failed || state == connection_state::closed) {
            end(state == connection_state::failed ? "connection failed" : "connection closed");
        } else if (state == connection_state::disconnected) {
            // Trystero waits five seconds before treating a disconnection as the end.
            if (!disconnect_timer_) {
                disconnect_timer_ = factory_.make_timer(memory_);
            }
            const auto weak = weak_from_this();
            disconnect_timer_->start(
                std::chrono::milliseconds(5000),
                {std::allocator_arg, std::pmr::polymorphic_allocator<>(memory_), [weak] {
                     if (const auto self = weak.lock()) {
                         self->end("connection lost");
                     }
                 }});
        } else if (disconnect_timer_) {
            disconnect_timer_->cancel();
        }
    }

    void on_drained(bool open) {
        for (auto& waiter : std::exchange(drain_waiters_, {})) {
            waiter(open);
        }
    }

    void release() {
        disconnect_timer_.reset();
        channel_->set_handlers({});
        pc_->set_handlers({});
        channel_->close();
        pc_->close();
    }

    void end(std::string_view reason) {
        if (ended_) {
            return;
        }
        ended_ = true;
        log_("webrtc", "connection with ", peer_, " ended: ", reason);
        release();
        on_drained(false);
        auto closed = std::move(cb_.closed);  // the room may destroy this link from inside the call
        closed(peer_, reason);
    }

    backend::factory& factory_;
    std::pmr::memory_resource* memory_;
    std::pmr::string peer_;
    std::pmr::string token_;
    std::shared_ptr<backend::peer_connection> pc_;
    std::shared_ptr<backend::data_channel> channel_;
    callbacks cb_;
    logger log_;
    std::shared_ptr<backend::timer> disconnect_timer_;
    std::pmr::vector<handler<void(bool)>> drain_waiters_;
    bool ended_ = false;
};

}  // namespace webcpp::trystero::detail

#endif
