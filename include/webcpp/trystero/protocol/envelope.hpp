// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_ENVELOPE_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_ENVELOPE_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/text.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace webcpp::trystero::protocol {

/**
 * The room envelope: every data channel message names the room it belongs to, so one connection can serve several
 * rooms of the same app.
 *
 * | Frame    | Bytes                                                        |
 * | room     | 1, token length (uint16 BE), token, action frame             |
 * | presence | 2, present (0 or 1), token length (uint16 BE), token         |
 */
inline constexpr std::size_t max_action_frame_bytes = 16 * 1024;
inline constexpr std::size_t max_room_token_bytes = 128;
inline constexpr std::size_t max_room_frame_bytes = max_action_frame_bytes + 3 + max_room_token_bytes;

/**
 * Starts a room frame in an empty buffer: the header for `token`, with room reserved for `payload_size` more bytes.
 * The action frame is then appended in place, so the frame is allocated and written once.
 */
inline bytes begin_room_frame(std::string_view token, std::size_t payload_size) {
    bytes out;
    out.reserve(3 + token.size() + payload_size);
    out.push_back(1);
    out.push_back(static_cast<std::uint8_t>((token.size() >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(token.size() & 0xff));
    out.insert(out.end(), token.begin(), token.end());
    return out;
}

inline bytes room_frame(std::string_view token, std::span<const std::uint8_t> payload) {
    auto out = begin_room_frame(token, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

inline bytes presence_frame(std::string_view token, bool present) {
    bytes out;
    out.reserve(4 + token.size());
    out.push_back(2);
    out.push_back(present ? 1 : 0);
    out.push_back(static_cast<std::uint8_t>((token.size() >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(token.size() & 0xff));
    out.insert(out.end(), token.begin(), token.end());
    return out;
}

/** A decoded envelope, as views into the frame. */
struct envelope {
    enum class kind { room, presence };
    kind type = kind::room;
    std::string_view token;
    bool present = false;
    std::span<const std::uint8_t> payload;  // the action frame, for `kind::room`
};

/** The envelope of a frame, or nothing when the frame is malformed; Trystero drops the peer then. */
inline std::optional<envelope> decode_envelope(std::span<const std::uint8_t> frame) {
    if (frame.size() < 3 || frame.size() > max_room_frame_bytes) return std::nullopt;
    const std::size_t offset = frame[0] == 1 ? 1 : frame[0] == 2 ? 2 : 0;
    if (offset == 0 || frame.size() < offset + 2) return std::nullopt;
    const std::size_t token_size = (frame[offset] << 8) | frame[offset + 1];
    const std::size_t header_size = offset + 2 + token_size;
    if (token_size == 0 || token_size > max_room_token_bytes || frame.size() < header_size) return std::nullopt;
    envelope out;
    out.token = std::string_view(reinterpret_cast<const char*>(frame.data() + offset + 2), token_size);
    if (frame[0] == 1) {
        out.type = envelope::kind::room;
        out.payload = frame.subspan(header_size);
    } else {
        out.type = envelope::kind::presence;
        out.present = frame[1] == 1;
    }
    return out;
}

}  // namespace webcpp::trystero::protocol

#endif
