// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_TEXT_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_TEXT_HPP

#include <webcpp/trystero/config.hpp>

#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace webcpp::trystero::protocol {

/** Bytes, in the memory resource a room or the caller gives. */
using bytes = std::pmr::vector<std::uint8_t>;

/**
 * Appends the lowercase hexadecimal of a byte sequence, Trystero's `toHex`.
 *
 * @param out The text to append to.
 * @param data The bytes.
 */
inline void append_hex(std::pmr::string& out, std::span<const std::uint8_t> data) {
    static constexpr std::string_view digits = "0123456789abcdef";
    out.reserve(out.size() + (data.size() * 2));
    for (const auto b : data) {
        out += digits[b >> 4U];
        out += digits[b & 15U];
    }
}

/**
 * The lowercase hexadecimal of a byte sequence, Trystero's `toHex`.
 *
 * @param data The bytes.
 * @param memory The resource the text is allocated in.
 * @return Two digits per byte.
 */
inline std::pmr::string to_hex(
    std::span<const std::uint8_t> data,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(memory);
    append_hex(out, data);
    return out;
}

/**
 * The bytes of a hexadecimal text, in either case.
 *
 * @param text The text.
 * @param memory The resource the bytes are allocated in.
 * @return The bytes, or nothing when the text is not hexadecimal of even length.
 */
inline std::optional<bytes> from_hex(
    std::string_view text, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    // A digit's value, or `not_hex` for a character that is none.
    constexpr std::uint32_t not_hex = 16;
    const auto nibble = [](char c) -> std::uint32_t {
        if (c >= '0' && c <= '9') {
            return static_cast<std::uint32_t>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<std::uint32_t>(c - 'a' + 10);
        }
        if (c >= 'A' && c <= 'F') {
            return static_cast<std::uint32_t>(c - 'A' + 10);
        }
        return not_hex;
    };
    bytes out(memory);
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const std::uint32_t high = nibble(text[i]);
        const std::uint32_t low = nibble(text[i + 1]);
        if (high == not_hex || low == not_hex) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>((high << 4U) | low));
    }
    return out;
}

/**
 * Appends standard base64 with padding, what `btoa` writes over binary text.
 *
 * @param out The text to append to.
 * @param in The bytes.
 */
inline void append_base64(std::pmr::string& out, std::span<const std::uint8_t> in) {
    static constexpr std::string_view table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out.reserve(out.size() + ((in.size() + 2) / 3 * 4));
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t v = (std::uint32_t{in[i]} << 16U) | (std::uint32_t{in[i + 1]} << 8U) |
                                std::uint32_t{in[i + 2]};
        out += table[(v >> 18U) & 63U];
        out += table[(v >> 12U) & 63U];
        out += table[(v >> 6U) & 63U];
        out += table[v & 63U];
    }
    if (const auto rest = in.size() - i; rest == 1) {
        const std::uint32_t v = std::uint32_t{in[i]} << 16U;
        out += table[(v >> 18U) & 63U];
        out += table[(v >> 12U) & 63U];
        out += "==";
    } else if (rest == 2) {
        const std::uint32_t v = (std::uint32_t{in[i]} << 16U) | (std::uint32_t{in[i + 1]} << 8U);
        out += table[(v >> 18U) & 63U];
        out += table[(v >> 12U) & 63U];
        out += table[(v >> 6U) & 63U];
        out += '=';
    }
}

/**
 * Standard base64 with padding, what `btoa` writes over binary text.
 *
 * @param in The bytes.
 * @param memory The resource the text is allocated in.
 * @return The text.
 */
inline std::pmr::string base64_encode(
    std::span<const std::uint8_t> in,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(memory);
    append_base64(out, in);
    return out;
}

/**
 * The bytes of a base64 text, padded or not, as `atob` reads it.
 *
 * @param in The text.
 * @param memory The resource the bytes are allocated in.
 * @return The bytes, or nothing when the text is not base64.
 */
inline std::optional<bytes> base64_decode(
    std::string_view in, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    // A digit's value, or `not_base64` for a character that is none.
    constexpr std::uint32_t not_base64 = 64;
    const auto value = [](char c) -> std::uint32_t {
        if (c >= 'A' && c <= 'Z') {
            return static_cast<std::uint32_t>(c - 'A');
        }
        if (c >= 'a' && c <= 'z') {
            return static_cast<std::uint32_t>(c - 'a' + 26);
        }
        if (c >= '0' && c <= '9') {
            return static_cast<std::uint32_t>(c - '0' + 52);
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return not_base64;
    };
    while (!in.empty() && in.back() == '=') {
        in.remove_suffix(1);
    }
    if (in.size() % 4 == 1) {
        return std::nullopt;
    }
    bytes out(memory);
    out.reserve(in.size() * 3 / 4);
    std::uint32_t buffer = 0;
    std::uint32_t bits = 0;
    for (const char c : in) {
        const std::uint32_t v = value(c);
        if (v == not_base64) {
            return std::nullopt;
        }
        buffer = (buffer << 6U) | v;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xffU));
        }
    }
    return out;
}

/**
 * Appends unpadded base64url, Trystero's `toBase64Url`.
 *
 * @param out The text to append to.
 * @param in The bytes.
 */
inline void append_base64url(std::pmr::string& out, std::span<const std::uint8_t> in) {
    static constexpr std::string_view table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    out.reserve(out.size() + ((in.size() * 4 + 2) / 3));
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t v = (std::uint32_t{in[i]} << 16U) | (std::uint32_t{in[i + 1]} << 8U) |
                                std::uint32_t{in[i + 2]};
        out += table[(v >> 18U) & 63U];
        out += table[(v >> 12U) & 63U];
        out += table[(v >> 6U) & 63U];
        out += table[v & 63U];
    }
    if (const auto rest = in.size() - i; rest == 1) {
        const std::uint32_t v = std::uint32_t{in[i]} << 16U;
        out += table[(v >> 18U) & 63U];
        out += table[(v >> 12U) & 63U];
    } else if (rest == 2) {
        const std::uint32_t v = (std::uint32_t{in[i]} << 16U) | (std::uint32_t{in[i + 1]} << 8U);
        out += table[(v >> 18U) & 63U];
        out += table[(v >> 12U) & 63U];
        out += table[(v >> 6U) & 63U];
    }
}

/**
 * Unpadded base64url, Trystero's `toBase64Url`.
 *
 * @param in The bytes.
 * @param memory The resource the text is allocated in.
 * @return The text, `A-Za-z0-9_-` only, without padding.
 */
inline std::pmr::string to_base64url(
    std::span<const std::uint8_t> in,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(memory);
    append_base64url(out, in);
    return out;
}

/**
 * The bytes of a base64url text, read strictly as Trystero's `fromBase64Url` reads it.
 *
 * The text must be nonempty, of `A-Za-z0-9_-` only, without padding, of a length that is not 1
 * modulo 4, and canonical: the unused low bits of its last character are zero, so that encoding
 * the bytes gives the text back.
 *
 * @param text The text.
 * @param memory The resource the bytes are allocated in.
 * @return The bytes, or nothing when the text is not strict base64url.
 */
inline std::optional<bytes> from_base64url(
    std::string_view text, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    // A digit's value, or `not_base64` for a character that is none.
    constexpr std::uint32_t not_base64 = 64;
    const auto value = [](char c) -> std::uint32_t {
        if (c >= 'A' && c <= 'Z') {
            return static_cast<std::uint32_t>(c - 'A');
        }
        if (c >= 'a' && c <= 'z') {
            return static_cast<std::uint32_t>(c - 'a' + 26);
        }
        if (c >= '0' && c <= '9') {
            return static_cast<std::uint32_t>(c - '0' + 52);
        }
        if (c == '-') {
            return 62;
        }
        if (c == '_') {
            return 63;
        }
        return not_base64;
    };
    if (text.empty() || text.size() % 4 == 1) {
        return std::nullopt;
    }
    bytes out(memory);
    out.reserve(text.size() * 3 / 4);
    std::uint32_t buffer = 0;
    std::uint32_t bits = 0;
    for (const char c : text) {
        const std::uint32_t v = value(c);
        if (v == not_base64) {
            return std::nullopt;
        }
        buffer = ((buffer << 6U) | v) & 0xffffU;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xffU));
        }
    }
    // The bits left over are the unused low bits of the last character, which must be zero.
    if ((buffer & ((1U << bits) - 1U)) != 0) {
        return std::nullopt;
    }
    return out;
}

/**
 * The sum of the UTF-16 code units of a UTF-8 text, what JavaScript's `charCodeAt` sees.
 *
 * Tip: Trystero derives Nostr kinds and the relay shuffle seed from it, so non-ASCII ids must count
 * as in JS. The sum is a double, JavaScript's Number, as Trystero's is; the oracle's fixtures pin
 * it.
 *
 * @param utf8 The text, in UTF-8.
 * @return The sum of the code units of its UTF-16 form, a character above U+FFFF counting as its
 * two surrogates.
 */
inline double utf16_code_unit_sum(std::string_view utf8) {
    double sum = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        // The bytes as unsigned values: a char may be signed.
        const auto byte = [utf8](std::size_t at) -> std::uint32_t {
            return static_cast<std::uint8_t>(utf8[at]);
        };
        const std::uint32_t c = byte(i);
        std::uint32_t point = c;
        std::size_t length = 1;
        if (c >= 0xf0 && i + 3 < utf8.size()) {
            point = ((c & 0x07U) << 18U) | ((byte(i + 1) & 0x3fU) << 12U) |
                    ((byte(i + 2) & 0x3fU) << 6U) | (byte(i + 3) & 0x3fU);
            length = 4;
        } else if (c >= 0xe0 && i + 2 < utf8.size()) {
            point = ((c & 0x0fU) << 12U) | ((byte(i + 1) & 0x3fU) << 6U) | (byte(i + 2) & 0x3fU);
            length = 3;
        } else if (c >= 0xc0 && i + 1 < utf8.size()) {
            point = ((c & 0x1fU) << 6U) | (byte(i + 1) & 0x3fU);
            length = 2;
        }
        if (point >= 0x10000) {
            point -= 0x10000;
            sum += 0xd800 + (point >> 10U);
            sum += 0xdc00 + (point & 0x3ffU);
        } else {
            sum += point;
        }
        i += length;
    }
    return sum;
}

}  // namespace webcpp::trystero::protocol

#endif
