// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_LOG_HPP
#define WEBCPP_TRYSTERO_DETAIL_LOG_HPP

#include <webcpp/trystero/config.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

namespace webcpp::trystero::detail {

/**
 * A room's diagnostics: a category ("nostr", "signal", "webrtc", "room", "action") and a sentence, handed to the
 * application's `room_config::log`. A sentence that has to be built is given as a function that builds it, called
 * only when there is a sink: a room without one composes no text at all.
 */
class logger {
public:
    using sink = std::function<void(std::string_view category, std::string_view text)>;

    logger() = default;
    explicit logger(sink s) : sink_(std::move(s)) {}

    explicit operator bool() const { return static_cast<bool>(sink_); }

    void operator()(std::string_view category, std::string_view text) const {
        if (sink_) sink_(category, text);
    }

    template <class Compose>
        requires std::is_invocable_r_v<std::string, Compose>
    void operator()(std::string_view category, Compose&& compose) const {
        if (sink_) sink_(category, std::forward<Compose>(compose)());
    }

private:
    sink sink_;
};

}  // namespace webcpp::trystero::detail

#endif
