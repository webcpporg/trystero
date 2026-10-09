// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_ACTION_WIRE_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_ACTION_WIRE_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/envelope.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace webcpp::trystero::protocol {

/**
 * The format of an action's payload, the high four bits of a frame's second byte: how Trystero's
 * `decodePayload` reads it.
 *
 * @see "The protocol", in the guide.
 */
enum class payload_format : std::uint8_t {
    /** UTF-8 text, a JavaScript string. */
    text = 0,
    /** JSON text, any other JavaScript value. */
    json = 1,
    /** Raw bytes, an `ArrayBuffer` or a typed array. */
    binary = 2,
};

/**
 * The kind of an action frame, the low four bits of its second byte, in Trystero's action wire,
 * version 2.
 *
 * A payload that fits one frame travels inline; a larger one is offered, accepted by the receiver,
 * then sent in chunks. Integers and float64 values are big-endian, as `DataView` writes them.
 *
 * | Frame              | Bytes                                                                    |
 * | inline (kind 0)    | 2, kind or format << 4, name (32 bytes, NUL padded), metadata length     |
 * |                    | (uint16), metadata, payload                                              |
 * | offer (kind 1)     | the inline header, transfer id (uint32), payload size (float64),         |
 * |                    | metadata                                                                 |
 * | chunk (kind 2)     | 2, 2, transfer id (uint32), offset (float64), bytes                      |
 * | accepted (kind 3)  | 2, 3, transfer id (uint32)                                               |
 * | rejected (kind 4)  | 2, 4, transfer id (uint32), reason (UTF-8, at most 200 bytes)            |
 * | cancelled (kind 5) | 2, 5, transfer id (uint32)                                               |
 *
 * @see "The protocol", in the guide.
 */
enum class frame_kind : std::uint8_t {
    /** A whole payload, Trystero's `inline`. */
    inline_payload = 0,
    /** The offer of a transfer, Trystero's `offer`. */
    offer = 1,
    /** A piece of a transfer's payload, Trystero's `chunk`. */
    chunk = 2,
    /** The receiver accepts an offer, Trystero's `accepted`. */
    accepted = 3,
    /** The receiver refuses an offer, Trystero's `rejected`. */
    rejected = 4,
    /** The sender gives a transfer up, Trystero's `cancelled`. */
    cancelled = 5,
};

/** The version of the action wire, every frame's first byte, Trystero's `version`. */
inline constexpr std::uint8_t action_wire_version = 2;
/** The size of an inline frame's header, Trystero's `inlineHeaderSize`. */
inline constexpr std::size_t inline_header_size = 36;
/** The size of an offer's header, Trystero's `offerHeaderSize`. */
inline constexpr std::size_t offer_header_size = 48;
/** The size of an accepted, rejected or cancelled frame's header, `controlHeaderSize`. */
inline constexpr std::size_t control_header_size = 6;
/** The size of a chunk's header, Trystero's `chunkHeaderSize`. */
inline constexpr std::size_t chunk_header_size = 14;
/** The most payload bytes one chunk carries, Trystero's `chunkSize`. */
inline constexpr std::size_t chunk_size = max_action_frame_bytes - chunk_header_size;
/** The longest action name, in UTF-8 bytes. */
inline constexpr std::size_t max_action_name_bytes = 32;
/** The longest reason a rejected frame carries, in UTF-8 bytes. */
inline constexpr std::size_t max_reject_reason_bytes = 200;

namespace detail {

/** Reads a big-endian uint32 at `at`. */
inline std::uint32_t get_u32(std::span<const std::uint8_t> in, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v = (v << 8U) | in[at + i];
    }
    return v;
}

/** Reads a big-endian float64 at `at`. */
inline double get_f64(std::span<const std::uint8_t> in, std::size_t at) {
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits = (bits << 8U) | in[at + i];
    }
    return std::bit_cast<double>(bits);
}

}  // namespace detail

/**
 * Whether a name can name an action, as Trystero's `makeAction` checks it: 1 to 32 UTF-8 bytes and
 * no NUL.
 *
 * @param name The name.
 * @return Whether it is valid.
 */
inline bool valid_action_name(std::string_view name) {
    return !name.empty() && name.size() <= max_action_name_bytes &&
           name.find('\0') == std::string_view::npos;
}

/**
 * Whether a payload with this metadata travels inline rather than as an offer and chunks.
 *
 * @param metadata_size The metadata's size in bytes.
 * @param payload_size The payload's size in bytes.
 * @return Whether the inline frame fits @ref max_action_frame_bytes.
 */
inline bool fits_inline(std::size_t metadata_size, std::size_t payload_size) {
    return inline_header_size + metadata_size + payload_size <= max_action_frame_bytes;
}

namespace detail {

/** Appends the header an inline frame and an offer share: version, kind and format, name. */
inline void append_header(bytes& out, frame_kind kind, std::string_view name, payload_format format,
                          std::string_view metadata, std::size_t header_size) {
    const auto at = out.size();
    out.resize(at + header_size, 0);
    out[at] = action_wire_version;
    out[at + 1] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(kind) |
                                            (static_cast<unsigned>(format) << 4U));
    std::memcpy(out.data() + at + 2, name.data(), name.size());
    out[at + 34] = static_cast<std::uint8_t>(metadata.size() >> 8U);
    out[at + 35] = static_cast<std::uint8_t>(metadata.size() & 0xffU);
}

/** Appends the header of a chunk or a transfer's answer: version, kind, transfer id. */
inline void append_control(bytes& out, frame_kind kind, std::uint32_t id) {
    out.push_back(action_wire_version);
    out.push_back(static_cast<std::uint8_t>(kind));
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(id >> (24U - (8 * i))));
    }
}

}  // namespace detail

/**
 * Appends an inline frame to `out`, which may already hold a room envelope's header.
 *
 * @param out The buffer to append to.
 * @param name The action's name.
 * @param format The payload's format.
 * @param metadata JSON text, or empty.
 * @param payload The payload.
 * @pre `valid_action_name(name)` and `fits_inline(metadata.size(), payload.size())`.
 */
inline void append_inline(bytes& out, std::string_view name, payload_format format,
                          std::string_view metadata, std::span<const std::uint8_t> payload) {
    out.reserve(out.size() + inline_header_size + metadata.size() + payload.size());
    detail::append_header(out, frame_kind::inline_payload, name, format, metadata,
                          inline_header_size);
    out.insert(out.end(), metadata.begin(), metadata.end());
    out.insert(out.end(), payload.begin(), payload.end());
}

/**
 * Appends an offer frame announcing `size` bytes under transfer `id`.
 *
 * @param out The buffer to append to.
 * @param name The action's name.
 * @param format The payload's format.
 * @param metadata JSON text, or empty.
 * @param id The transfer's id.
 * @param size The payload's size in bytes.
 * @pre `valid_action_name(name)`, and the metadata fits one frame after the offer's header.
 */
inline void append_offer(bytes& out, std::string_view name, payload_format format,
                         std::string_view metadata, std::uint32_t id, std::size_t size) {
    out.reserve(out.size() + offer_header_size + metadata.size());
    const auto at = out.size();
    detail::append_header(out, frame_kind::offer, name, format, metadata, offer_header_size);
    for (int i = 0; i < 4; ++i) {
        out[at + 36 + i] = static_cast<std::uint8_t>(id >> (24U - (8 * i)));
    }
    const auto bits = std::bit_cast<std::uint64_t>(static_cast<double>(size));
    for (int i = 0; i < 8; ++i) {
        out[at + 40 + i] = static_cast<std::uint8_t>(bits >> (56U - (8 * i)));
    }
    out.insert(out.end(), metadata.begin(), metadata.end());
}

/**
 * Appends a chunk of transfer `id`.
 *
 * @param out The buffer to append to.
 * @param id The transfer's id.
 * @param offset Where `data` starts in the payload.
 * @param data At most @ref chunk_size bytes of the payload.
 */
inline void append_chunk(bytes& out, std::uint32_t id, std::size_t offset,
                         std::span<const std::uint8_t> data) {
    out.reserve(out.size() + chunk_header_size + data.size());
    detail::append_control(out, frame_kind::chunk, id);
    const auto bits = std::bit_cast<std::uint64_t>(static_cast<double>(offset));
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(bits >> (56U - (8 * i))));
    }
    out.insert(out.end(), data.begin(), data.end());
}

/**
 * Appends the frame accepting transfer `id`.
 *
 * @param out The buffer to append to.
 * @param id The transfer's id.
 */
inline void append_accepted(bytes& out, std::uint32_t id) {
    detail::append_control(out, frame_kind::accepted, id);
}

/**
 * Appends the frame cancelling transfer `id`.
 *
 * @param out The buffer to append to.
 * @param id The transfer's id.
 */
inline void append_cancelled(bytes& out, std::uint32_t id) {
    detail::append_control(out, frame_kind::cancelled, id);
}

/**
 * Appends the frame rejecting transfer `id`.
 *
 * @param out The buffer to append to.
 * @param id The transfer's id.
 * @param reason Why, cut to @ref max_reject_reason_bytes.
 */
inline void append_rejected(bytes& out, std::uint32_t id, std::string_view reason) {
    if (reason.size() > max_reject_reason_bytes) {
        reason = reason.substr(0, max_reject_reason_bytes);
    }
    detail::append_control(out, frame_kind::rejected, id);
    out.insert(out.end(), reason.begin(), reason.end());
}

/**
 * An inline frame, in a buffer of its own.
 *
 * @param name The action's name.
 * @param format The payload's format.
 * @param metadata JSON text, or empty.
 * @param payload The payload.
 * @param memory The resource the frame is allocated in.
 * @return The frame, as @ref append_inline writes it.
 */
inline bytes encode_inline(std::string_view name, payload_format format, std::string_view metadata,
                           std::span<const std::uint8_t> payload,
                           std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_inline(out, name, format, metadata, payload);
    return out;
}

/**
 * An offer frame, in a buffer of its own.
 *
 * @param name The action's name.
 * @param format The payload's format.
 * @param metadata JSON text, or empty.
 * @param id The transfer's id.
 * @param size The payload's size in bytes.
 * @param memory The resource the frame is allocated in.
 * @return The frame, as @ref append_offer writes it.
 */
inline bytes encode_offer(std::string_view name, payload_format format, std::string_view metadata,
                          std::uint32_t id, std::size_t size,
                          std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_offer(out, name, format, metadata, id, size);
    return out;
}

/**
 * A chunk frame, in a buffer of its own.
 *
 * @param id The transfer's id.
 * @param offset Where `data` starts in the payload.
 * @param data At most @ref chunk_size bytes of the payload.
 * @param memory The resource the frame is allocated in.
 * @return The frame, as @ref append_chunk writes it.
 */
inline bytes encode_chunk(std::uint32_t id, std::size_t offset, std::span<const std::uint8_t> data,
                          std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_chunk(out, id, offset, data);
    return out;
}

/**
 * The frame accepting a transfer, in a buffer of its own.
 *
 * @param id The transfer's id.
 * @param memory The resource the frame is allocated in.
 * @return The frame.
 */
inline bytes encode_accepted(std::uint32_t id,
                             std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_accepted(out, id);
    return out;
}

/**
 * The frame cancelling a transfer, in a buffer of its own.
 *
 * @param id The transfer's id.
 * @param memory The resource the frame is allocated in.
 * @return The frame.
 */
inline bytes encode_cancelled(
    std::uint32_t id, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_cancelled(out, id);
    return out;
}

/**
 * The frame rejecting a transfer, in a buffer of its own.
 *
 * @param id The transfer's id.
 * @param reason Why, cut to @ref max_reject_reason_bytes.
 * @param memory The resource the frame is allocated in.
 * @return The frame.
 */
inline bytes encode_rejected(std::uint32_t id, std::string_view reason,
                             std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_rejected(out, id, reason);
    return out;
}

/**
 * A decoded inline frame or offer, as views into the frame.
 *
 * @see "The protocol", in the guide.
 */
struct proposal {
    /** `frame_kind::inline_payload` or `frame_kind::offer`. */
    frame_kind kind = frame_kind::inline_payload;
    /** The action's name, up to the first NUL of the 32-byte field. */
    std::string_view name;
    /** The payload's format. */
    payload_format format = payload_format::text;
    /** The metadata, JSON text, or empty. */
    std::string_view metadata;
    /** The transfer's id; offers only. */
    std::uint32_t id = 0;
    /** The payload's size; for an inline frame, the bytes that follow the metadata. */
    std::size_t size = 0;
    /** The payload; inline frames only. */
    std::span<const std::uint8_t> payload;
};

/** A decoded chunk, as a view into the frame. */
struct chunk {
    /** The transfer's id. */
    std::uint32_t id = 0;
    /** Where the data starts in the payload, as the float64 the frame carries; NaN when absent. */
    double offset = 0;
    /** The data. */
    std::span<const std::uint8_t> data;
};

/** A decoded accepted, rejected or cancelled frame, as a view into the frame. */
struct control_message {
    /** `frame_kind::accepted`, `frame_kind::rejected` or `frame_kind::cancelled`. */
    frame_kind kind = frame_kind::accepted;
    /** The transfer's id. */
    std::uint32_t id = 0;
    /** Why the transfer was rejected; rejected frames only. */
    std::string_view reason;
};

/** A decoded action frame: a proposal, a chunk or a transfer's answer. */
using decoded_action = std::variant<proposal, chunk, control_message>;

/** Why a frame cannot be decoded; Trystero drops the peer for any of these. */
enum class wire_error {
    /** Too short, too long, or a kind or a format that does not exist. */
    invalid_frame,
    /** A version other than @ref action_wire_version. */
    incompatible_version,
    /** A chunk or a transfer's answer of the wrong size. */
    invalid_control,
    /** An inline frame or an offer shorter than its header. */
    invalid_header,
    /** An empty name, metadata beyond the frame, or an offer of an impossible size or length. */
    invalid_proposal,
};

/** What @ref decode_action makes of a frame. */
struct decode_result {
    /** The decoded frame, or nothing when it cannot be decoded. */
    std::optional<decoded_action> frame;
    /** Why the frame cannot be decoded; meaningful only without a frame. */
    wire_error error = wire_error::invalid_frame;
};

namespace detail {

/** Decodes a control frame, a chunk or a transfer's answer, of the kind given. */
inline decode_result decode_control(std::span<const std::uint8_t> bytes_in, frame_kind k) {
    decode_result result;
    if (bytes_in.size() < control_header_size ||
        ((k == frame_kind::accepted || k == frame_kind::cancelled) &&
         bytes_in.size() != control_header_size) ||
        (k == frame_kind::rejected &&
         bytes_in.size() > control_header_size + max_reject_reason_bytes)) {
        result.error = wire_error::invalid_control;
        return result;
    }
    const auto id = get_u32(bytes_in, 2);
    if (k == frame_kind::chunk) {
        chunk c;
        c.id = id;
        c.offset = bytes_in.size() >= chunk_header_size ? get_f64(bytes_in, control_header_size)
                                                        : std::nan("");
        if (bytes_in.size() >= chunk_header_size) {
            c.data = bytes_in.subspan(chunk_header_size);
        }
        result.frame = c;
        return result;
    }
    control_message m;
    m.kind = k;
    m.id = id;
    if (k == frame_kind::rejected) {
        m.reason =
            std::string_view(reinterpret_cast<const char*>(bytes_in.data() + control_header_size),
                             bytes_in.size() - control_header_size);
    }
    result.frame = m;
    return result;
}

/** Decodes a proposal, a payload inline or the offer of a transfer, of the kind given. */
inline decode_result decode_proposal(std::span<const std::uint8_t> bytes_in, frame_kind kind,
                                     payload_format format) {
    decode_result result;
    const bool is_inline = kind == frame_kind::inline_payload;
    const std::size_t header_size = is_inline ? inline_header_size : offer_header_size;
    if (bytes_in.size() < header_size) {
        result.error = wire_error::invalid_header;
        return result;
    }
    proposal p;
    p.kind = kind;
    p.format = format;
    const auto* name = reinterpret_cast<const char*>(bytes_in.data() + 2);
    p.name = std::string_view(
        name, static_cast<std::size_t>(std::find(name, name + max_action_name_bytes, '\0') - name));
    const std::size_t metadata_size = (std::size_t{bytes_in[34]} << 8U) | bytes_in[35];
    const std::size_t payload_index = header_size + metadata_size;
    if (p.name.empty() || payload_index > bytes_in.size()) {
        result.error = wire_error::invalid_proposal;
        return result;
    }
    if (is_inline) {
        p.size = bytes_in.size() - payload_index;
        p.payload = bytes_in.subspan(payload_index);
    } else {
        const double size = get_f64(bytes_in, 40);
        if (size < 1 || size > 9007199254740991.0 || std::floor(size) != size ||
            payload_index != bytes_in.size()) {
            result.error = wire_error::invalid_proposal;
            return result;
        }
        p.id = get_u32(bytes_in, 36);
        p.size = static_cast<std::size_t>(size);
    }
    p.metadata = std::string_view(reinterpret_cast<const char*>(bytes_in.data() + header_size),
                                  metadata_size);
    result.frame = p;
    return result;
}

}  // namespace detail

/**
 * Decodes one action frame with exactly the checks Trystero's `handleData` makes.
 *
 * @param bytes_in The action frame, out of its room envelope.
 * @return The frame, as views into `bytes_in`, or why it cannot be decoded.
 */
inline decode_result decode_action(std::span<const std::uint8_t> bytes_in) {
    decode_result result;
    if (bytes_in.size() < 2 || bytes_in.size() > max_action_frame_bytes) {
        return result;
    }
    if (bytes_in[0] != action_wire_version) {
        result.error = wire_error::incompatible_version;
        return result;
    }
    const auto kind = static_cast<std::uint8_t>(bytes_in[1] & 15U);
    const auto format = static_cast<std::uint8_t>(bytes_in[1] >> 4U);
    if (kind > static_cast<std::uint8_t>(frame_kind::cancelled) ||
        format > static_cast<std::uint8_t>(payload_format::binary) ||
        (kind > static_cast<std::uint8_t>(frame_kind::offer) && format != 0)) {
        return result;
    }
    if (kind >= static_cast<std::uint8_t>(frame_kind::chunk)) {
        return detail::decode_control(bytes_in, static_cast<frame_kind>(kind));
    }
    return detail::decode_proposal(bytes_in, static_cast<frame_kind>(kind),
                                   static_cast<payload_format>(format));
}

}  // namespace webcpp::trystero::protocol

#endif
