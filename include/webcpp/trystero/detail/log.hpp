// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_LOG_HPP
#define WEBCPP_TRYSTERO_DETAIL_LOG_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/handler.hpp>

#include <boost/system/error_code.hpp>

#include <array>
#include <charconv>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace webcpp::trystero::detail {

/**
 * A room's diagnostics: a category ("nostr", "signal", "webrtc", "room", "action") and a sentence,
 * handed to the application's `room_config::log`. The sentence is given as its parts (texts,
 * integers, error codes), joined into a string of the room's resource only when there is a sink: a
 * room without one composes no text at all.
 *
 * Tip: every part of a room shares the one sink, through a `std::shared_ptr` made in the room's
 * resource, so the sink outlives a part that is still running when the room goes.
 */
class logger {
public:
    /** Where the sentences go: `room_config::log`. */
    using sink = handler<void(std::string_view category, std::string_view text)>;

    /** A logger without a sink, which composes nothing. */
    logger() noexcept = default;

    /** Moves `s` into `memory`, where the sentences are composed too; an empty `s` makes a logger
     * that composes none. */
    logger(sink s, std::pmr::memory_resource* memory) : memory_(memory) {
        if (s) {
            // The allocator constructs the sink with itself, which moves the target into `memory`.
            sink_ =
                std::allocate_shared<sink>(std::pmr::polymorphic_allocator<>(memory), std::move(s));
        }
    }

    /** Whether there is a sink. */
    explicit operator bool() const noexcept { return sink_ != nullptr; }

    /** Composes a sentence of `parts` and hands it to the sink, when there is one. */
    template <class... Parts>
    void operator()(std::string_view category, const Parts&... parts) const {
        if (!sink_) {
            return;
        }
        std::pmr::string text(memory_);
        (append(text, parts), ...);
        (*sink_)(category, text);
    }

private:
    template <class Part>
    static void append(std::pmr::string& text, const Part& part) {
        if constexpr (std::is_same_v<Part, boost::system::error_code>) {
            std::array<char, 128> buffer{};
            text += part.message(buffer.data(), buffer.size());
        } else if constexpr (std::is_integral_v<Part> && !std::is_same_v<Part, bool> &&
                             !std::is_same_v<Part, char>) {
            std::array<char, 24> buffer{};
            const auto [end, ec] =
                std::to_chars(buffer.data(), buffer.data() + buffer.size(), part);
            text.append(buffer.data(), end);
        } else {
            text += std::string_view(part);
        }
    }

    std::shared_ptr<sink> sink_;
    std::pmr::memory_resource* memory_ = nullptr;
};

}  // namespace webcpp::trystero::detail

#endif
