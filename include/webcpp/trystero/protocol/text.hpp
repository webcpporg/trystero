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

/** Bytes, in the resource a room or the caller gives. */
using bytes = std::pmr::vector<std::uint8_t>;

/** Appends lowercase hexadecimal of a byte sequence, Trystero's `toHex`. */
inline void append_hex(std::pmr::string& out, std::span<const std::uint8_t> data) {
    static constexpr std::string_view digits = "0123456789abcdef";
    out.reserve(out.size() + data.size() * 2);
    for (const auto b : data) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
}

inline std::pmr::string to_hex(std::span<const std::uint8_t> data, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(memory);
    append_hex(out, data);
    return out;
}

/** Bytes of a hexadecimal text, or nothing when it is not hexadecimal of even length. */
inline std::optional<bytes> from_hex(std::string_view text, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    if (text.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    bytes out(memory);
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = nibble(text[i]);
        const int low = nibble(text[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

/** Appends standard base64 with padding, what `btoa` writes over binary text. */
inline void append_base64(std::pmr::string& out, std::span<const std::uint8_t> in) {
    static constexpr std::string_view table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out.reserve(out.size() + (in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }
    if (const auto rest = in.size() - i; rest == 1) {
        const std::uint32_t v = in[i] << 16;
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += "==";
    } else if (rest == 2) {
        const std::uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += '=';
    }
}

inline std::pmr::string base64_encode(std::span<const std::uint8_t> in, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(memory);
    append_base64(out, in);
    return out;
}

/** Bytes of a padded base64 text, what `atob` accepts, or nothing when it is not base64. */
inline std::optional<bytes> base64_decode(std::string_view in, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    while (!in.empty() && in.back() == '=') in.remove_suffix(1);
    if (in.size() % 4 == 1) return std::nullopt;
    bytes out(memory);
    out.reserve(in.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : in) {
        const int v = value(c);
        if (v < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

/**
 * Sum of the UTF-16 code units of a UTF-8 text, what JavaScript's `charCodeAt` sees.
 * Tip: Trystero derives Nostr kinds and the relay shuffle seed from it, so non-ASCII ids must count as in JS.
 */
inline double utf16_code_unit_sum(std::string_view utf8) {
    double sum = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        std::uint32_t point = c;
        std::size_t length = 1;
        if (c >= 0xf0 && i + 3 < utf8.size()) {
            point = ((c & 0x07) << 18) | ((utf8[i + 1] & 0x3f) << 12) | ((utf8[i + 2] & 0x3f) << 6) | (utf8[i + 3] & 0x3f);
            length = 4;
        } else if (c >= 0xe0 && i + 2 < utf8.size()) {
            point = ((c & 0x0f) << 12) | ((utf8[i + 1] & 0x3f) << 6) | (utf8[i + 2] & 0x3f);
            length = 3;
        } else if (c >= 0xc0 && i + 1 < utf8.size()) {
            point = ((c & 0x1f) << 6) | (utf8[i + 1] & 0x3f);
            length = 2;
        }
        if (point >= 0x10000) {
            point -= 0x10000;
            sum += 0xd800 + (point >> 10);
            sum += 0xdc00 + (point & 0x3ff);
        } else {
            sum += point;
        }
        i += length;
    }
    return sum;
}

}  // namespace webcpp::trystero::protocol

#endif
