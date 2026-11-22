// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_WEBRTC_DATA_CHANNEL_HPP
#define WEBCPP_TRYSTERO_WEBRTC_DATA_CHANNEL_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/action_wire.hpp>

#include <webcpp/webrtc/data_channel.hpp>
#include <webcpp/webrtc/error.hpp>
#include <webcpp/webrtc/options.hpp>
#include <webcpp/webrtc/runtime.hpp>
#include <webcpp/webrtc/types.hpp>

#include <webcpp/asio/any_io_executor.hpp>  // lint-world: names the executor only
#include <webcpp/asio/bind_allocator.hpp>   // lint-world: binds an allocator only
#include <webcpp/asio/buffer.hpp>           // lint-world: a frame's buffer only
#include <webcpp/asio/error.hpp>            // lint-world: Asio's codes only
#include <webcpp/asio/post.hpp>             // lint-world: posts handlers only

#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <utility>

/**
 * The backend's WebRTC over webcpp::webrtc, the same on both targets: natively libdatachannel's,
 * in the browser the page's own. The platform backend makes its peer connections here, and what
 * remains of the platform is webcpp::webrtc's.
 */
namespace webcpp::trystero::webrtc {

/** The executor of the backend's WebRTC objects: the app's. */
using executor_type = webcpp::asio::any_io_executor;

/** The allocator of their state, their queues and their texts: the room's resource. */
using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

/**
 * A data channel over webcpp::webrtc's, the backend's `RTCDataChannel`, which carries a peer's
 * frames.
 *
 * It reads every message as it comes, with `async_receive`, on the executor. A text message, as a
 * peer can send, or a binary one larger than any frame (`protocol::max_bulk_frame_bytes`, a wide
 * chunk's bound), closes the channel: neither can be a frame, and 0.27.0's `failData` (`peer.ts`)
 * ends the peer for either.
 *
 * Frames received while no message handler is installed wait in the room's resource, in order:
 * the open event and a first message reach the executor in either order, and the room's link
 * (`detail::peer_link`) installs its handler from the open event, so that only that gap is
 * covered. The frames wait under the bounds the link puts on a peer's frames before its room binds
 * it, 0.27.0's (`peer.ts`): at most `max_pending_frames`, each at most `max_pending_frame_bytes`;
 * one more, or a longer one, closes the channel, as the link would end on it. They have no time
 * bound of their own: the link's 10 s starts as it takes them, and a channel no link takes is one
 * the signaling closes, which discards them. Installing a receiver posts their delivery, so that
 * `set_handlers` never calls it inline. In the browser, a message larger than webcpp::webrtc's
 * `options::max_message_size`, or past its receive budget, closes the channel too.
 *
 * Tip: a received frame is read once, by webcpp::webrtc, into a vector of the room's resource,
 * which the message handler then owns; what the platform holds before is the platform's.
 *
 * @see "The backend's WebRTC", in the guide.
 */
class data_channel : public backend::data_channel,
                     public std::enable_shared_from_this<data_channel> {
    struct private_tag {
        explicit private_tag() = default;
    };

public:
    /** webcpp::webrtc's data channel it holds. */
    using channel_type = ::webcpp::webrtc::basic_data_channel<executor_type, allocator_type>;

    /** The most frames kept while no message handler is installed, 0.27.0's
     * `maxQueuedDataFrames`, as the room's link keeps before its room binds it: one more closes
     * the channel. */
    static constexpr std::size_t max_pending_frames = protocol::max_queued_data_frames;

    /** The longest frame kept while no message handler is installed, 0.27.0's
     * `maxRoomFrameBytes`, as the room's link keeps before its room binds it: a longer one closes
     * the channel. */
    static constexpr std::size_t max_pending_frame_bytes = protocol::max_room_frame_bytes;

    /**
     * Takes over a channel of webcpp::webrtc: its threshold, and its events.
     *
     * @param runtime The runtime of the channel's connection, which the channel keeps alive.
     * @param channel The channel, made or accepted by a connection of the same executor and
     * allocator.
     * @param memory The room's resource, the channel's: the wrapper, its handlers and its frames
     * are allocated in it.
     * @return The channel.
     */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    static std::shared_ptr<data_channel> make(std::shared_ptr<::webcpp::webrtc::runtime> runtime,
                                              channel_type channel,
                                              std::pmr::memory_resource* memory) {
        // NOLINTEND(performance-unnecessary-value-param)
        auto self = std::allocate_shared<data_channel>(std::pmr::polymorphic_allocator<>(memory),
                                                       private_tag{}, std::move(runtime),
                                                       std::move(channel), memory);
        self->start();
        return self;
    }

    /**
     * Constructs the wrapper; only `make` can, which then takes over the channel's events.
     *
     * @param runtime The runtime of the channel's connection.
     * @param channel The channel.
     * @param memory The resource its handlers and its frames are allocated in.
     */
    data_channel(private_tag /*tag*/, std::shared_ptr<::webcpp::webrtc::runtime> runtime,
                 channel_type channel, std::pmr::memory_resource* memory)
        : runtime_(std::move(runtime)),
          memory_(memory),
          pending_(memory),
          frame_(memory),
          channel_(std::move(channel)) {}

    /** Not copyable: its operations reach it by its address. */
    data_channel(const data_channel&) = delete;
    /** Not copy-assignable: its operations reach it by its address. */
    data_channel& operator=(const data_channel&) = delete;

    /**
     * Destroys the wrapper, which closes the channel without reporting the close: webcpp::webrtc
     * aborts its pending operations, whose handlers find the wrapper gone.
     */
    ~data_channel() override = default;

    /**
     * Replaces the handlers, kept in a shared object so that one replacing them during its own call
     * outlives the call.
     *
     * A message handler installed while frames wait posts their delivery.
     *
     * @param h The new handlers, kept in the channel's resource.
     */
    void set_handlers(handlers h) override {
        h_ = std::allocate_shared<handlers>(allocator(), std::move(h));
        if (!pending_.empty() && h_->message && !flush_posted_) {
            flush_posted_ = true;
            webcpp::asio::post(channel_.get_executor(),
                               webcpp::asio::bind_allocator(allocator(), [weak = weak_from_this()] {
                                   if (const auto self = weak.lock()) {
                                       self->flush_posted_ = false;
                                       self->flush_pending();
                                   }
                               }));
        }
    }

    /**
     * Whether the channel is open.
     *
     * @return Whether this wrapper has not closed it and its `readyState` is "open".
     */
    [[nodiscard]] bool is_open() const override {
        return !closed_ && channel_.ready_state() == ::webcpp::webrtc::data_channel_state::open;
    }

    /**
     * Sends a binary message when the channel is open; one the channel refuses, as one that would
     * pass webcpp::webrtc's bound on what it buffers, is dropped.
     *
     * @param frame The message, read during the call.
     */
    void send(std::span<const std::uint8_t> frame) override {
        if (!is_open()) {
            return;
        }
        // A channel closed in between refuses the frame, and its close event reports it; a frame
        // refused leaves nothing to drain.
        if (channel_.send(webcpp::asio::buffer(frame.data(), frame.size()))) {
            return;
        }
        watch_drain();
    }

    /**
     * The bytes queued to send.
     *
     * @return Its `bufferedAmount`.
     */
    [[nodiscard]] std::size_t buffered_amount() const override {
        return channel_.buffered_amount();
    }

    /** Closes the channel, which the handler `close` reports, and discards the frames kept. */
    void close() override {
        closed_ = true;
        clear_pending();
        channel_.close();
    }

    /**
     * The frames received while no message handler is installed, which wait for one.
     *
     * @return Their number, at most `max_pending_frames`.
     */
    [[nodiscard]] std::size_t pending_frames() const noexcept { return pending_.size(); }

private:
    using buffer_type =
        webcpp::asio::dynamic_vector_buffer<std::uint8_t,
                                            std::pmr::polymorphic_allocator<std::uint8_t>>;

    [[nodiscard]] std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    /** Binds `f` to the channel's resource, which webcpp::webrtc allocates its operation in. */
    template <class F>
    auto bound(F&& f) const {
        return webcpp::asio::bind_allocator(allocator(), std::forward<F>(f));
    }

    void start() {
        const auto weak = weak_from_this();
        channel_.set_buffered_amount_low_threshold(low_threshold);
        channel_.async_wait_open(bound([weak](boost::system::error_code ec) {
            if (const auto self = weak.lock(); self && !ec) {
                self->on_open();
            }
        }));
        channel_.async_wait_close(bound([weak](boost::system::error_code ec) {
            if (const auto self = weak.lock(); self && !ec) {
                self->on_closed();
            }
        }));
        receive();
    }

    /** Reads the next message into a fresh frame of the channel's resource. */
    void receive() {
        // A frame no larger than the largest message webcpp::webrtc accepts, so that none stays
        // queued: natively libdatachannel cuts a longer one to it, and in the browser a longer one
        // closes the channel.
        frame_.clear();
        buffer_.emplace(frame_, ::webcpp::webrtc::options{}.max_message_size);
        channel_.async_receive(*buffer_,
                               bound([weak = weak_from_this()](boost::system::error_code ec,
                                                               std::size_t /*size*/, bool text) {
                                   if (const auto self = weak.lock()) {
                                       self->on_received(ec, text);
                                   }
                               }));
    }

    void on_received(boost::system::error_code ec, bool text) {
        buffer_.reset();
        // Closed or destroyed, or closing: in the browser a message past max_message_size, or
        // past the receive budget, closes the channel. Its close event reports it.
        if (ec == webcpp::asio::error::operation_aborted || ec == ::webcpp::webrtc::error::closed ||
            ec == ::webcpp::webrtc::error::message_too_large ||
            ec == ::webcpp::webrtc::error::receive_overflow) {
            return;
        }
        if (ec) {
            // A message no buffer can take stays queued, and the platform's failure stays too: the
            // channel cannot go on.
            close();
            return;
        }
        // Trystero frames are binary, and none is larger than a wide chunk: a message that is
        // neither closes the channel, as 0.27.0's failData (peer.ts) closes it, before the frame
        // goes anywhere.
        if (text || frame_.size() > protocol::max_bulk_frame_bytes) {
            frame_.clear();
            close();
            return;
        }
        auto frame = std::exchange(frame_, backend::bytes(memory_));
        if (!closed_) {
            receive();
        }
        on_message(std::move(frame));
    }

    // The handlers run from a copy of the shared object, which a handler may replace.

    void on_open() {
        if (const auto h = h_; !closed_ && h && h->open) {
            h->open();
        }
    }

    void on_message(backend::bytes frame) {
        if (!is_open()) {
            return;
        }
        // A delivery posted by set_handlers may still be ahead: a receiver now exists, so the
        // frames kept go first, before the new one counts against the bound.
        if (!pending_.empty() && h_ && h_->message) {
            flush_pending();
            if (!is_open()) {
                return;
            }
        }
        if (const auto h = h_; h && h->message && pending_.empty()) {
            h->message(std::move(frame));
            return;
        }
        // No receiver yet, or frames ahead of this one: the bounds of the link's unbound frames.
        if (frame.size() > max_pending_frame_bytes || pending_.size() >= max_pending_frames) {
            close();
            return;
        }
        pending_.push_back(std::move(frame));
    }

    void clear_pending() { pending_.clear(); }

    void flush_pending() {
        while (!pending_.empty()) {
            if (!is_open()) {
                clear_pending();
                return;
            }
            const auto h = h_;
            if (!h || !h->message) {
                return;
            }
            auto frame = std::move(pending_.front());
            pending_.pop_front();
            if (pending_.empty()) {
                clear_pending();
            }
            h->message(std::move(frame));
        }
    }

    void on_closed() {
        closed_ = true;
        clear_pending();
        if (const auto h = h_) {
            if (auto f = std::exchange(h->close, {})) {
                f();
            }
        }
    }

    /**
     * Waits for the buffered amount to fall to `low_threshold` once a send left it above: the
     * handler `buffered_amount_low` reports the fall, as `onbufferedamountlow` does.
     */
    void watch_drain() {
        if (draining_ || channel_.buffered_amount() <= low_threshold) {
            return;
        }
        draining_ = true;
        channel_.async_wait_buffered_amount_low(
            bound([weak = weak_from_this()](boost::system::error_code ec) {
                const auto self = weak.lock();
                if (!self) {
                    return;
                }
                self->draining_ = false;
                if (const auto h = self->h_; !ec && h && h->buffered_amount_low) {
                    h->buffered_amount_low();
                }
            }));
    }

    // The runtime outlives the channel, and the channel goes first, before the frame it reads into.
    std::shared_ptr<::webcpp::webrtc::runtime> runtime_;
    std::pmr::memory_resource* memory_;
    std::shared_ptr<handlers> h_;
    std::pmr::deque<backend::bytes> pending_;
    backend::bytes frame_;
    std::optional<buffer_type> buffer_;
    bool flush_posted_ = false;
    bool draining_ = false;
    bool closed_ = false;
    channel_type channel_;
};

}  // namespace webcpp::trystero::webrtc

#endif
