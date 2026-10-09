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
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace webcpp::trystero::protocol {

/** The largest action frame, Trystero's `maxActionFrameBytes`: 16 KiB. */
inline constexpr std::size_t max_action_frame_bytes = std::size_t{16} * 1024;
/** The longest room token a room frame may carry, in bytes. */
inline constexpr std::size_t max_room_token_bytes = 128;
/** The largest room frame, Trystero's `maxRoomFrameBytes`: an action frame in its envelope. */
inline constexpr std::size_t max_room_frame_bytes =
    max_action_frame_bytes + 3 + max_room_token_bytes;

/**
 * Starts a room frame in an empty buffer: the header for `token`, with room reserved for
 * `payload_size` more bytes.
 *
 * The action frame is then appended in place, so the frame is allocated and written once.
 *
 * @param token The room's token, @ref room_token's.
 * @param payload_size The size of the action frame that will follow.
 * @param memory The resource the frame is allocated in.
 * @return The frame's header.
 */
inline bytes begin_room_frame(
    std::string_view token, std::size_t payload_size,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    out.reserve(3 + token.size() + payload_size);
    out.push_back(1);
    out.push_back(static_cast<std::uint8_t>((token.size() >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(token.size() & 0xffU));
    out.insert(out.end(), token.begin(), token.end());
    return out;
}

/**
 * A room frame, Trystero's `wrapRoomFrame`: an action frame in the envelope of its room.
 *
 * @param token The room's token, @ref room_token's.
 * @param payload The action frame.
 * @param memory The resource the frame is allocated in.
 * @return The frame.
 */
inline bytes room_frame(std::string_view token, std::span<const std::uint8_t> payload,
                        std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    auto out = begin_room_frame(token, payload.size(), memory);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/**
 * A presence frame, Trystero's `wrapRoomPresenceFrame`: whether this peer is in a room.
 *
 * @param token The room's token, @ref room_token's.
 * @param present Whether the peer joins (true) or leaves (false) the room.
 * @param memory The resource the frame is allocated in.
 * @return The frame.
 */
inline bytes presence_frame(std::string_view token, bool present,
                            std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    out.reserve(4 + token.size());
    out.push_back(2);
    out.push_back(present ? 1 : 0);
    out.push_back(static_cast<std::uint8_t>((token.size() >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(token.size() & 0xffU));
    out.insert(out.end(), token.begin(), token.end());
    return out;
}

/**
 * A decoded room envelope, as views into the frame: every data channel message names the room it
 * belongs to, so one connection can serve several rooms of the same app.
 *
 * | Frame    | Bytes                                                        |
 * | room     | 1, token length (uint16 BE), token, action frame             |
 * | presence | 2, present (0 or 1), token length (uint16 BE), token         |
 *
 * @see "The protocol", in the guide.
 */
struct envelope {
    /** The kind of a frame, its first byte. */
    enum class kind {
        /** A room frame, 1: an action frame for the room. */
        room,
        /** A presence frame, 2: the peer joins or leaves the room. */
        presence,
    };
    /** The frame's kind. */
    kind type = kind::room;
    /** The token of the room the frame is for. */
    std::string_view token;
    /** Whether the peer is in the room, for `kind::presence`. */
    bool present = false;
    /** The action frame, for `kind::room`. */
    std::span<const std::uint8_t> payload;
};

/**
 * Decodes the envelope of a frame, with the checks of Trystero's `unwrapFrame`.
 *
 * @param frame A data channel message.
 * @return The envelope, or nothing when the frame is malformed; Trystero drops the peer then.
 */
inline std::optional<envelope> decode_envelope(std::span<const std::uint8_t> frame) {
    if (frame.size() < 3 || frame.size() > max_room_frame_bytes) {
        return std::nullopt;
    }
    // The token's length follows the kind of a room frame (1), and a presence frame's (2) byte.
    const std::size_t offset = frame[0] == 1 || frame[0] == 2 ? frame[0] : 0;
    if (offset == 0 || frame.size() < offset + 2) {
        return std::nullopt;
    }
    const std::size_t token_size = (std::size_t{frame[offset]} << 8U) | frame[offset + 1];
    const std::size_t header_size = offset + 2 + token_size;
    if (token_size == 0 || token_size > max_room_token_bytes || frame.size() < header_size) {
        return std::nullopt;
    }
    envelope out;
    out.token =
        std::string_view(reinterpret_cast<const char*>(frame.data() + offset + 2), token_size);
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
