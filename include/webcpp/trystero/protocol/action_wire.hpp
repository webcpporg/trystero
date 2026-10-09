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
 * Trystero's action wire, version 2. A payload that fits one frame travels inline; a larger one is offered, accepted
 * by the receiver, then sent in chunks. Integers and float64 values are big-endian, as `DataView` writes them.
 *
 * | Frame              | Bytes                                                                                     |
 * | inline (kind 0)    | 2, kind or format << 4, name (32 bytes, NUL padded), metadata length (uint16), metadata, payload |
 * | offer (kind 1)     | the inline header, transfer id (uint32), payload size (float64), metadata                 |
 * | chunk (kind 2)     | 2, 2, transfer id (uint32), offset (float64), bytes                                        |
 * | accepted (kind 3)  | 2, 3, transfer id (uint32)                                                                 |
 * | rejected (kind 4)  | 2, 4, transfer id (uint32), reason (UTF-8, at most 200 bytes)                              |
 * | cancelled (kind 5) | 2, 5, transfer id (uint32)                                                                 |
 */
enum class payload_format : std::uint8_t { text = 0, json = 1, binary = 2 };

enum class frame_kind : std::uint8_t { inline_payload = 0, offer = 1, chunk = 2, accepted = 3, rejected = 4, cancelled = 5 };

inline constexpr std::uint8_t action_wire_version = 2;
inline constexpr std::size_t inline_header_size = 36;
inline constexpr std::size_t offer_header_size = 48;
inline constexpr std::size_t control_header_size = 6;
inline constexpr std::size_t chunk_header_size = 14;
inline constexpr std::size_t chunk_size = max_action_frame_bytes - chunk_header_size;
inline constexpr std::size_t max_action_name_bytes = 32;
inline constexpr std::size_t max_reject_reason_bytes = 200;

namespace detail {

inline std::uint32_t get_u32(std::span<const std::uint8_t> in, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | in[at + i];
    return v;
}

inline double get_f64(std::span<const std::uint8_t> in, std::size_t at) {
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits = (bits << 8) | in[at + i];
    return std::bit_cast<double>(bits);
}

}  // namespace detail

/** Whether a name can name an action: 1 to 32 UTF-8 bytes and no NUL. */
inline bool valid_action_name(std::string_view name) {
    return !name.empty() && name.size() <= max_action_name_bytes && name.find('\0') == std::string_view::npos;
}

/** Whether a payload with this metadata travels inline rather than as an offer and chunks. */
inline bool fits_inline(std::size_t metadata_size, std::size_t payload_size) {
    return inline_header_size + metadata_size + payload_size <= max_action_frame_bytes;
}

namespace detail {

inline void append_header(bytes& out, frame_kind kind, std::string_view name, payload_format format, std::string_view metadata,
                          std::size_t header_size) {
    const auto at = out.size();
    out.resize(at + header_size, 0);
    out[at] = action_wire_version;
    out[at + 1] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(kind) | (static_cast<std::uint8_t>(format) << 4));
    std::memcpy(out.data() + at + 2, name.data(), name.size());
    out[at + 34] = static_cast<std::uint8_t>(metadata.size() >> 8);
    out[at + 35] = static_cast<std::uint8_t>(metadata.size() & 0xff);
}

inline void append_control(bytes& out, frame_kind kind, std::uint32_t id) {
    out.push_back(action_wire_version);
    out.push_back(static_cast<std::uint8_t>(kind));
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(id >> (24 - 8 * i)));
}

}  // namespace detail

/**
 * Appends an inline frame to `out`, which may already hold a room envelope's header. `metadata` is JSON text or
 * empty. The caller checks the name and `fits_inline` first.
 */
inline void append_inline(bytes& out, std::string_view name, payload_format format, std::string_view metadata,
                          std::span<const std::uint8_t> payload) {
    out.reserve(out.size() + inline_header_size + metadata.size() + payload.size());
    detail::append_header(out, frame_kind::inline_payload, name, format, metadata, inline_header_size);
    out.insert(out.end(), metadata.begin(), metadata.end());
    out.insert(out.end(), payload.begin(), payload.end());
}

/** Appends an offer frame announcing `size` bytes under transfer `id`. */
inline void append_offer(bytes& out, std::string_view name, payload_format format, std::string_view metadata, std::uint32_t id, std::size_t size) {
    out.reserve(out.size() + offer_header_size + metadata.size());
    const auto at = out.size();
    detail::append_header(out, frame_kind::offer, name, format, metadata, offer_header_size);
    for (int i = 0; i < 4; ++i) out[at + 36 + i] = static_cast<std::uint8_t>(id >> (24 - 8 * i));
    const auto bits = std::bit_cast<std::uint64_t>(static_cast<double>(size));
    for (int i = 0; i < 8; ++i) out[at + 40 + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    out.insert(out.end(), metadata.begin(), metadata.end());
}

inline void append_chunk(bytes& out, std::uint32_t id, std::size_t offset, std::span<const std::uint8_t> data) {
    out.reserve(out.size() + chunk_header_size + data.size());
    detail::append_control(out, frame_kind::chunk, id);
    const auto bits = std::bit_cast<std::uint64_t>(static_cast<double>(offset));
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(bits >> (56 - 8 * i)));
    out.insert(out.end(), data.begin(), data.end());
}

inline void append_accepted(bytes& out, std::uint32_t id) { detail::append_control(out, frame_kind::accepted, id); }

inline void append_cancelled(bytes& out, std::uint32_t id) { detail::append_control(out, frame_kind::cancelled, id); }

inline void append_rejected(bytes& out, std::uint32_t id, std::string_view reason) {
    if (reason.size() > max_reject_reason_bytes) reason = reason.substr(0, max_reject_reason_bytes);
    detail::append_control(out, frame_kind::rejected, id);
    out.insert(out.end(), reason.begin(), reason.end());
}

/** The same frames, each in a buffer of its own. */
inline bytes encode_inline(std::string_view name, payload_format format, std::string_view metadata, std::span<const std::uint8_t> payload, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_inline(out, name, format, metadata, payload);
    return out;
}

inline bytes encode_offer(std::string_view name, payload_format format, std::string_view metadata, std::uint32_t id, std::size_t size, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_offer(out, name, format, metadata, id, size);
    return out;
}

inline bytes encode_chunk(std::uint32_t id, std::size_t offset, std::span<const std::uint8_t> data, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_chunk(out, id, offset, data);
    return out;
}

inline bytes encode_accepted(std::uint32_t id, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_accepted(out, id);
    return out;
}

inline bytes encode_cancelled(std::uint32_t id, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_cancelled(out, id);
    return out;
}

inline bytes encode_rejected(std::uint32_t id, std::string_view reason, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    bytes out(memory);
    append_rejected(out, id, reason);
    return out;
}

/** A decoded inline frame or offer. Views point into the frame. */
struct proposal {
    frame_kind kind = frame_kind::inline_payload;
    std::string_view name;  // up to the first NUL of the 32-byte field
    payload_format format = payload_format::text;
    std::string_view metadata;           // JSON text, or empty
    std::uint32_t id = 0;                // offers only
    std::size_t size = 0;                // payload size; for inline frames, the bytes that follow
    std::span<const std::uint8_t> payload;  // inline frames only
};

struct chunk {
    std::uint32_t id = 0;
    double offset = 0;
    std::span<const std::uint8_t> data;
};

struct control_message {
    frame_kind kind = frame_kind::accepted;
    std::uint32_t id = 0;
    std::string_view reason;  // rejected only
};

using decoded_action = std::variant<proposal, chunk, control_message>;

/** Why a frame cannot be decoded. Trystero drops the peer for any of these. */
enum class wire_error { invalid_frame, incompatible_version, invalid_control, invalid_header, invalid_proposal };

struct decode_result {
    std::optional<decoded_action> frame;
    wire_error error = wire_error::invalid_frame;
};

/** Decodes one action frame with exactly the checks Trystero's `handleData` makes. */
inline decode_result decode_action(std::span<const std::uint8_t> bytes_in) {
    decode_result result;
    if (bytes_in.size() < 2 || bytes_in.size() > max_action_frame_bytes) return result;
    if (bytes_in[0] != action_wire_version) {
        result.error = wire_error::incompatible_version;
        return result;
    }
    const auto kind = static_cast<std::uint8_t>(bytes_in[1] & 15);
    const auto format = static_cast<std::uint8_t>(bytes_in[1] >> 4);
    if (kind > static_cast<std::uint8_t>(frame_kind::cancelled) || format > static_cast<std::uint8_t>(payload_format::binary) ||
        (kind > static_cast<std::uint8_t>(frame_kind::offer) && format != 0)) {
        return result;
    }
    if (kind >= static_cast<std::uint8_t>(frame_kind::chunk)) {
        const auto k = static_cast<frame_kind>(kind);
        if (bytes_in.size() < control_header_size || ((k == frame_kind::accepted || k == frame_kind::cancelled) && bytes_in.size() != control_header_size) ||
            (k == frame_kind::rejected && bytes_in.size() > control_header_size + max_reject_reason_bytes)) {
            result.error = wire_error::invalid_control;
            return result;
        }
        const auto id = detail::get_u32(bytes_in, 2);
        if (k == frame_kind::chunk) {
            chunk c;
            c.id = id;
            c.offset = bytes_in.size() >= chunk_header_size ? detail::get_f64(bytes_in, control_header_size) : std::nan("");
            if (bytes_in.size() >= chunk_header_size) c.data = bytes_in.subspan(chunk_header_size);
            result.frame = c;
        } else {
            control_message m;
            m.kind = k;
            m.id = id;
            if (k == frame_kind::rejected) {
                m.reason = std::string_view(reinterpret_cast<const char*>(bytes_in.data() + control_header_size), bytes_in.size() - control_header_size);
            }
            result.frame = m;
        }
        return result;
    }
    const bool is_inline = kind == static_cast<std::uint8_t>(frame_kind::inline_payload);
    const std::size_t header_size = is_inline ? inline_header_size : offer_header_size;
    if (bytes_in.size() < header_size) {
        result.error = wire_error::invalid_header;
        return result;
    }
    proposal p;
    p.kind = static_cast<frame_kind>(kind);
    p.format = static_cast<payload_format>(format);
    const auto* name = reinterpret_cast<const char*>(bytes_in.data() + 2);
    p.name = std::string_view(name, static_cast<std::size_t>(std::find(name, name + max_action_name_bytes, '\0') - name));
    const std::size_t metadata_size = (bytes_in[34] << 8) | bytes_in[35];
    const std::size_t payload_index = header_size + metadata_size;
    if (p.name.empty() || payload_index > bytes_in.size()) {
        result.error = wire_error::invalid_proposal;
        return result;
    }
    if (is_inline) {
        p.size = bytes_in.size() - payload_index;
        p.payload = bytes_in.subspan(payload_index);
    } else {
        const double size = detail::get_f64(bytes_in, 40);
        if (!(size >= 1 && size <= 9007199254740991.0 && std::floor(size) == size) || payload_index != bytes_in.size()) {
            result.error = wire_error::invalid_proposal;
            return result;
        }
        p.id = detail::get_u32(bytes_in, 36);
        p.size = static_cast<std::size_t>(size);
    }
    p.metadata = std::string_view(reinterpret_cast<const char*>(bytes_in.data() + header_size), metadata_size);
    result.frame = p;
    return result;
}

}  // namespace webcpp::trystero::protocol

#endif
