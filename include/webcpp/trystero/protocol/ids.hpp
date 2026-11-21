// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_IDS_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_IDS_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::protocol {

/** The 62 characters of Trystero 0.27.0's ids, its `base62Alphabet`, in its order. */
inline constexpr std::string_view base62_alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/** The length of a Trystero peer id, its `selfId` and the id @ref derive_peer_id makes. */
inline constexpr std::size_t peer_id_size = 20;
/** The length of the id of a user, which @ref derive_user_id makes from a compact public key. */
inline constexpr std::size_t user_id_size = 22;
/** The length of the id of a Trystero offer, which its answer and candidates name. */
inline constexpr std::size_t offer_id_size = 12;
/** The length of the nonce that makes repeated announcements distinct Nostr events. */
inline constexpr std::size_t nonce_size = 8;
/** The length of the id of a Nostr subscription. */
inline constexpr std::size_t subscription_id_size = 64;
/** The length of a UUID's text, `8-4-4-4-12` hexadecimal digits and four hyphens. */
inline constexpr std::size_t uuid_text_size = 36;

/**
 * How many random bytes one round of @ref gen_id draws for an id of `n` characters, Trystero's
 * `Math.ceil(n * 1.15) + 4`, computed in integers.
 *
 * @param n The id's length.
 * @return `ceil(n * 1.15) + 4`.
 */
constexpr std::size_t gen_id_draw_size(std::size_t n) {
    return (n * 115 + 99) / 100 + 4;
}

/**
 * Trystero 0.27.0's `genId`: an unbiased base62 id of `n` characters.
 *
 * Each round fills @ref gen_id_draw_size bytes through `draw`; a byte below 248 (the largest
 * multiple of 62 that fits a byte) appends the character at its remainder in 62, a larger byte is
 * skipped, so each of the 62 characters is equally likely. Rounds repeat until the id is complete.
 *
 * @tparam Draw A callable with `void(std::span<std::uint8_t>)` that fills its argument with
 * random bytes.
 * @param n The id's length.
 * @param draw The source of random bytes.
 * @param memory The resource the id is allocated in.
 * @return The id, `n` characters of @ref base62_alphabet.
 */
template <class Draw>
std::pmr::string gen_id(std::size_t n, Draw&& draw,
                        std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    constexpr std::uint8_t unbiased_limit = 248;
    std::pmr::string out(memory);
    if (n == 0) {
        return out;
    }
    out.reserve(n);
    std::pmr::vector<std::uint8_t> random(gen_id_draw_size(n), memory);
    while (out.size() < n) {
        draw(std::span<std::uint8_t>(random));
        for (std::size_t i = 0; i < random.size() && out.size() < n; ++i) {
            if (random[i] < unbiased_limit) {
                out += base62_alphabet[random[i] % base62_alphabet.size()];
            }
        }
    }
    return out;
}

/**
 * The low `length` base-62 digits of a big-endian number, most significant first: Trystero's
 * `encodeBase62`, which takes the number modulo 62^`length`.
 *
 * @param big_endian The number's bytes, most significant first.
 * @param length How many digits.
 * @param memory The resource the text is allocated in.
 * @return `length` characters of @ref base62_alphabet.
 */
inline std::pmr::string encode_base62(
    std::span<const std::uint8_t> big_endian, std::size_t length,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::vector<std::uint8_t> number(big_endian.begin(), big_endian.end(), memory);
    std::pmr::string out(length, '0', memory);
    for (std::size_t digit = length; digit != 0; --digit) {
        std::uint32_t remainder = 0;
        for (auto& byte : number) {
            const std::uint32_t current = (remainder << 8U) | byte;
            byte = static_cast<std::uint8_t>(current / 62U);
            remainder = current % 62U;
        }
        out[digit - 1] = base62_alphabet[remainder];
    }
    return out;
}

/**
 * A version 4 UUID (RFC 9562) of 16 random bytes, as `crypto.randomUUID()` writes it.
 *
 * @param random The random bytes; the version and variant bits are set in the copy used.
 * @return The text, lowercase `8-4-4-4-12`, 36 characters.
 */
inline std::array<char, uuid_text_size> uuid_v4(std::array<std::uint8_t, 16> random) {
    random[6] = static_cast<std::uint8_t>((random[6] & 0x0fU) | 0x40U);
    random[8] = static_cast<std::uint8_t>((random[8] & 0x3fU) | 0x80U);
    constexpr std::string_view digits = "0123456789abcdef";
    std::array<char, uuid_text_size> out{};
    std::size_t at = 0;
    for (std::size_t i = 0; i < random.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[at++] = '-';
        }
        out[at++] = digits[random[i] >> 4U];
        out[at++] = digits[random[i] & 0x0fU];
    }
    return out;
}

/**
 * The compact form of a public key: `base64url(x) + "." + base64url(y)`, 87 characters.
 *
 * @param uncompressed The key's SEC1 uncompressed point, `04 || x || y`.
 * @param memory The resource the text is allocated in.
 * @return The compact text.
 */
inline std::pmr::string compact_public_key(
    std::span<const std::uint8_t, 65> uncompressed,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out = to_base64url(uncompressed.subspan(1, 32), memory);
    out += '.';
    append_base64url(out, uncompressed.subspan(33, 32));
    return out;
}

/**
 * The point of a compact public key: 87 characters, a `.` at 43, and strict base64url of 32 bytes
 * on each side. The point is not checked to be on the curve.
 *
 * @param compact The compact text.
 * @return The SEC1 uncompressed point, `04 || x || y`, or nothing when the text is not compact.
 */
inline std::optional<std::array<std::uint8_t, 65>> parse_compact_public_key(
    std::string_view compact) {
    constexpr std::size_t half = 43;
    if (compact.size() != 2 * half + 1 || compact[half] != '.') {
        return std::nullopt;
    }
    std::array<std::uint8_t, 65> out{};
    out[0] = 0x04;
    for (std::size_t side = 0; side < 2; ++side) {
        const auto read = from_base64url(compact.substr(side * (half + 1), half));
        if (!read || read->size() != 32) {
            return std::nullopt;
        }
        std::ranges::copy(*read, out.begin() + 1 + static_cast<std::ptrdiff_t>(side * 32));
    }
    return out;
}

/** Internal helper of the two derivations below: the id of `length` base62 characters from the
 * SHA-256 of the tag of `name`, `:` and `compact`.
 */
inline std::pmr::string identity_id_of(std::string_view name, std::string_view compact,
                                       std::size_t length, std::pmr::memory_resource* memory) {
    const auto tag = domain_tag(name, 1, memory);
    return encode_base62(sha256_of({tag, ":", compact}), length, memory);
}

/**
 * A peer's id, derived from its compact public key: the low 20 base-62 digits of the SHA-256 of
 * `"trystero:pid:v1:" + compact`.
 *
 * @param compact The key's compact text, see @ref compact_public_key.
 * @param memory The resource the id is allocated in.
 * @return The id, @ref peer_id_size characters.
 */
inline std::pmr::string derive_peer_id(
    std::string_view compact,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return identity_id_of("pid", compact, peer_id_size, memory);
}

/**
 * A user's id, derived from its compact public key: the low 22 base-62 digits of the SHA-256 of
 * `"trystero:uid:v1:" + compact`.
 *
 * @param compact The key's compact text, see @ref compact_public_key.
 * @param memory The resource the id is allocated in.
 * @return The id, @ref user_id_size characters.
 */
inline std::pmr::string derive_user_id(
    std::string_view compact,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return identity_id_of("uid", compact, user_id_size, memory);
}

}  // namespace webcpp::trystero::protocol

#endif
