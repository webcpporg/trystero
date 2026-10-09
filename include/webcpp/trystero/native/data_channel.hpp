// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_NATIVE_DATA_CHANNEL_HPP
#define WEBCPP_TRYSTERO_NATIVE_DATA_CHANNEL_HPP

#include <webcpp/trystero/config.hpp>

// libdatachannel reports errors by throwing, and the data channel over it catches them: it exists only
// with exceptions, and without them (WEBCPP_TRYSTERO_NO_EXCEPTIONS) this header declares nothing.
#ifndef WEBCPP_TRYSTERO_NO_EXCEPTIONS

#include <webcpp/trystero/backend.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <rtc/datachannel.hpp>

#include <cstring>
#include <memory>
#include <memory_resource>
#include <utility>
#include <variant>

namespace webcpp::trystero::native {

/**
 * A data channel of libdatachannel. Its events arrive on libdatachannel's threads; each is posted to the executor and
 * handled there, through a weak reference, so a late event for a destroyed channel does nothing.
 *
 * Tip: a received message is copied once, out of libdatachannel's `std::vector<std::byte>` into the backend's bytes, in
 * the room's resource, on the executor: what runs on libdatachannel's threads touches no resource of the room, and
 * its posts carry Asio's default allocator.
 */
class data_channel : public backend::data_channel, public std::enable_shared_from_this<data_channel> {
    struct private_tag {
        explicit private_tag() = default;
    };

public:
    static std::shared_ptr<data_channel> create(boost::asio::any_io_executor ex, std::shared_ptr<rtc::DataChannel> channel,
                                                std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
        auto self = std::allocate_shared<data_channel>(std::pmr::polymorphic_allocator<>(memory), private_tag{}, std::move(ex), std::move(channel),
                                                       memory);
        self->start();
        return self;
    }

    data_channel(private_tag /*tag*/, boost::asio::any_io_executor ex, std::shared_ptr<rtc::DataChannel> channel, std::pmr::memory_resource* memory)
        : ex_(std::move(ex)), channel_(std::move(channel)), memory_(memory) {}

    ~data_channel() override {
        channel_->resetCallbacks();
        try {
            channel_->close();
        } catch (...) {  // a channel whose connection is gone may refuse
        }
    }

    /** The handlers, kept in a shared object so that one replacing them during its own call outlives the call. */
    void set_handlers(handlers h) override { h_ = std::allocate_shared<handlers>(std::pmr::polymorphic_allocator<>(memory_), std::move(h)); }

    bool is_open() const override { return channel_->isOpen(); }

    void send(std::span<const std::uint8_t> frame) override {
        try {
            if (channel_->isOpen()) channel_->send(reinterpret_cast<const std::byte*>(frame.data()), frame.size());
        } catch (...) {  // closed in between: the close event reports it
        }
    }

    std::size_t buffered_amount() const override { return channel_->bufferedAmount(); }

    void close() override {
        try {
            channel_->close();
        } catch (...) {
        }
    }

private:
    void start() {
        std::weak_ptr<data_channel> weak = shared_from_this();
        auto ex = ex_;
        channel_->setBufferedAmountLowThreshold(low_threshold);
        channel_->onOpen([weak, ex] {
            boost::asio::post(ex, [weak] {
                if (auto self = weak.lock()) {
                    if (auto h = self->h_; h && h->open) h->open();
                }
            });
        });
        channel_->onMessage([weak, ex](rtc::message_variant message) {
            auto* binary = std::get_if<rtc::binary>(&message);
            if (binary == nullptr) return;  // Trystero frames are binary
            boost::asio::post(ex, [weak, binary = std::move(*binary)] {
                auto self = weak.lock();
                if (!self) return;
                auto h = self->h_;
                if (!h || !h->message) return;
                backend::bytes frame(binary.size(), self->memory_);
                std::memcpy(frame.data(), binary.data(), binary.size());
                h->message(std::move(frame));
            });
        });
        channel_->onClosed([weak, ex] {
            boost::asio::post(ex, [weak] {
                if (auto self = weak.lock()) {
                    if (auto h = self->h_) {
                        if (auto f = std::exchange(h->close, {})) f();
                    }
                }
            });
        });
        channel_->onBufferedAmountLow([weak, ex] {
            boost::asio::post(ex, [weak] {
                if (auto self = weak.lock()) {
                    if (auto h = self->h_; h && h->buffered_amount_low) h->buffered_amount_low();
                }
            });
        });
    }

    boost::asio::any_io_executor ex_;
    std::shared_ptr<rtc::DataChannel> channel_;
    std::pmr::memory_resource* memory_;
    std::shared_ptr<handlers> h_;
};

}  // namespace webcpp::trystero::native

#endif  // WEBCPP_TRYSTERO_NO_EXCEPTIONS

#endif
