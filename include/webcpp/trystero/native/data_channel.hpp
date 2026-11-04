// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_NATIVE_DATA_CHANNEL_HPP
#define WEBCPP_TRYSTERO_NATIVE_DATA_CHANNEL_HPP

#include <webcpp/trystero/config.hpp>

// libdatachannel reports errors by throwing, and the data channel over it catches them: it exists
// only with exceptions, and without them (WEBCPP_TRYSTERO_NO_EXCEPTIONS) this header declares
// nothing.
#ifndef WEBCPP_TRYSTERO_NO_EXCEPTIONS

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/native/timer.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>

#include <boost/asio/any_io_executor.hpp>  // lint-world: posts handlers only
#include <boost/asio/bind_allocator.hpp>   // lint-world: allocates the pending-frame flush
#include <boost/asio/post.hpp>             // lint-world: posts handlers only
#include <rtc/datachannel.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <memory_resource>
#include <utility>
#include <variant>

namespace webcpp::trystero::native {

/**
 * A data channel of libdatachannel, the native `RTCDataChannel`.
 *
 * Its events arrive on libdatachannel's threads; each is posted to the executor and handled there,
 * through a weak reference, so a late event for a destroyed channel does nothing. It exists only
 * with exceptions: libdatachannel reports errors by throwing.
 *
 * A text message, as a peer can send, or a binary one larger than any room frame
 * (`protocol::max_room_frame_bytes`), is dropped on libdatachannel's thread: neither can be a
 * frame. Binary frames received before the message handler is installed wait in the app's
 * resource, in order, up to 64 frames for 10 seconds. Exceeding either bound closes the channel.
 * Installing a receiver posts their delivery, so handler registration never calls it inline.
 *
 * Tip: a received message is copied once, out of libdatachannel's `std::vector<std::byte>` into the
 * backend's bytes, in the room's resource, on the executor: what runs on libdatachannel's threads
 * touches no resource of the room, and its posts carry Asio's default allocator.
 */
class data_channel : public backend::data_channel,
                     public std::enable_shared_from_this<data_channel> {
    struct private_tag {
        explicit private_tag() = default;
    };

public:
    /**
     * Wraps a channel of libdatachannel, and takes over its events.
     *
     * @param ex The executor every handler of the channel runs on.
     * @param channel libdatachannel's channel.
     * @param memory The resource the wrapper, its handlers and its frames are allocated in.
     * @return The channel.
     */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    static std::shared_ptr<data_channel> create(boost::asio::any_io_executor ex,
                                                std::shared_ptr<rtc::DataChannel> channel,
                                                std::pmr::memory_resource* memory) {
        // NOLINTEND(performance-unnecessary-value-param)
        auto self = std::allocate_shared<data_channel>(std::pmr::polymorphic_allocator<>(memory),
                                                       private_tag{}, std::move(ex),
                                                       std::move(channel), memory);
        self->start();
        return self;
    }

    /**
     * Constructs the wrapper; only `create` can, which then takes over the channel's events.
     *
     * @param ex The executor every handler of the channel runs on.
     * @param channel libdatachannel's channel.
     * @param memory The resource its handlers and its frames are allocated in.
     */
    data_channel(private_tag /*tag*/, boost::asio::any_io_executor ex,
                 std::shared_ptr<rtc::DataChannel> channel, std::pmr::memory_resource* memory)
        : ex_(std::move(ex)),
          channel_(std::move(channel)),
          memory_(memory),
          pending_(memory),
          pending_timer_(ex_, memory) {}

    /** Destroys the wrapper, which closes the channel without reporting the close. */
    ~data_channel() override {
        channel_->resetCallbacks();
        // A channel whose connection is gone may refuse to close, which leaves nothing to do.
        try {
            channel_->close();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }

    /**
     * Replaces the handlers, kept in a shared object so that one replacing them during its own call
     * outlives the call.
     *
     * @param h The new handlers, kept in the channel's resource.
     */
    void set_handlers(handlers h) override {
        h_ = std::allocate_shared<handlers>(std::pmr::polymorphic_allocator<>(memory_),
                                            std::move(h));
        if (!pending_.empty() && h_->message && !flush_posted_) {
            flush_posted_ = true;
            boost::asio::post(
                ex_, boost::asio::bind_allocator(std::pmr::polymorphic_allocator<>(memory_),
                                                 [weak = weak_from_this()] {
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
     * @return Whether this wrapper has not closed and libdatachannel's channel is open.
     */
    bool is_open() const override { return !closed_ && channel_->isOpen(); }

    /**
     * Sends a binary message; one the channel refuses, closed in between, is dropped, and its close
     * event reports the close.
     *
     * @param frame The message, read during the call.
     */
    void send(std::span<const std::uint8_t> frame) override {
        // A channel closed in between refuses the frame, and its close event reports it.
        try {
            if (is_open()) {
                channel_->send(reinterpret_cast<const std::byte*>(frame.data()), frame.size());
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }

    /**
     * The bytes queued to send.
     *
     * @return libdatachannel's `bufferedAmount()`.
     */
    std::size_t buffered_amount() const override { return channel_->bufferedAmount(); }

    /** Closes the channel, which the handler `close` reports; a refusal is ignored. */
    void close() override {
        closed_ = true;
        clear_pending();
        // A channel whose connection is gone may refuse to close, which leaves nothing to do.
        try {
            channel_->close();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }

private:
    /** Posts `f` from one of libdatachannel's threads to the executor, where it runs on this
     * channel if the channel still exists then. */
    template <class F>
    static void post_to(const boost::asio::any_io_executor& ex,
                        const std::weak_ptr<data_channel>& weak, F f) {
        boost::asio::post(ex, [weak, f = std::move(f)]() mutable {
            if (const auto self = weak.lock()) {
                f(*self);
            }
        });
    }

    void start() {
        const std::weak_ptr<data_channel> weak = shared_from_this();
        const auto ex = ex_;
        channel_->setBufferedAmountLowThreshold(low_threshold);
        channel_->onOpen(
            [weak, ex] { post_to(ex, weak, [](data_channel& self) { self.on_open(); }); });
        channel_->onMessage([weak, ex](rtc::message_variant message) {
            auto* binary = std::get_if<rtc::binary>(&message);
            // Trystero frames are binary, and none is larger than the largest room frame.
            if (binary == nullptr || binary->size() > protocol::max_room_frame_bytes) {
                return;
            }
            post_to(ex, weak,
                    [binary = std::move(*binary)](data_channel& self) { self.on_message(binary); });
        });
        channel_->onClosed(
            [weak, ex] { post_to(ex, weak, [](data_channel& self) { self.on_closed(); }); });
        channel_->onBufferedAmountLow(
            [weak, ex] { post_to(ex, weak, [](data_channel& self) { self.on_drained(); }); });
    }

    // The handlers run from a copy of the shared object, which a handler may replace.

    void on_open() {
        if (const auto h = h_; !closed_ && h && h->open) {
            h->open();
        }
    }

    void on_message(const rtc::binary& binary) {
        if (!is_open()) {
            return;
        }
        // An already posted message may be ahead of set_handlers' flush. A receiver now
        // exists, so drain old frames before admitting the new one against the queue bound.
        if (!pending_.empty() && h_ && h_->message) {
            flush_pending();
            if (!is_open()) {
                return;
            }
        }
        const auto h = h_;
        if ((!h || !h->message || !pending_.empty()) && pending_.size() >= max_pending_frames) {
            close();
            return;
        }
        backend::bytes frame(binary.size(), memory_);
        std::memcpy(frame.data(), binary.data(), binary.size());
        if (h && h->message && pending_.empty()) {
            h->message(std::move(frame));
            return;
        }
        if (pending_.empty()) {
            pending_timer_.start(std::chrono::seconds(10),
                                 {std::allocator_arg, std::pmr::polymorphic_allocator<>(memory_),
                                  [weak = weak_from_this()] {
                                      if (const auto self = weak.lock();
                                          self && !self->pending_.empty()) {
                                          self->close();
                                      }
                                  }});
        }
        pending_.push_back(std::move(frame));
    }

    void clear_pending() {
        pending_.clear();
        pending_timer_.cancel();
    }

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

    void on_drained() {
        if (const auto h = h_; h && h->buffered_amount_low) {
            h->buffered_amount_low();
        }
    }

    boost::asio::any_io_executor ex_;
    std::shared_ptr<rtc::DataChannel> channel_;
    std::pmr::memory_resource* memory_;
    std::shared_ptr<handlers> h_;
    static constexpr std::size_t max_pending_frames = 64;
    std::pmr::deque<backend::bytes> pending_;
    timer pending_timer_;
    bool flush_posted_ = false;
    bool closed_ = false;
};

}  // namespace webcpp::trystero::native

#endif  // WEBCPP_TRYSTERO_NO_EXCEPTIONS

#endif
