// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_TOPICS_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_TOPICS_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/text.hpp>

#include <webcpp/hash/digest.hpp>

#include <boost/assert.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

namespace webcpp::trystero::protocol {

/** The 62 characters Trystero draws peer ids, offer ids and nonces from, its `charSet`. */
inline constexpr std::string_view id_alphabet =
    "0123456789AaBbCcDdEeFfGgHhIiJjKkLlMmNnOoPpQqRrSsTtUuVvWwXxYyZz";

/** The length of a Trystero peer id, its `selfId`. */
inline constexpr std::size_t peer_id_size = 20;
/** The length of the id of a Trystero offer, which its answer and candidates name. */
inline constexpr std::size_t offer_id_size = 12;
/** The length of the nonce that makes repeated announcements distinct Nostr events. */
inline constexpr std::size_t nonce_size = 8;

/**
 * An id of one character of `id_alphabet` for each random byte, Trystero's `genId`.
 *
 * Each byte picks the character at its remainder in 62, as Trystero picks one character of its
 * `charSet` for each draw, so an id of `n` characters takes `n` random bytes.
 *
 * Tip: 256 is not a multiple of 62, so each of the first eight characters comes from 5 of the 256
 * bytes and each other from 4, where Trystero draws each of the 62 alike: a character carries
 * about 5.95 bits of its byte's 8, a little less than log2(62).
 *
 * @param random The random bytes, one for each character.
 * @param memory The resource the id is allocated in.
 * @return The id, as long as `random`.
 */
inline std::pmr::string random_id(
    std::span<const std::uint8_t> random,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(random.size(), '0', memory);
    std::ranges::transform(random, out.begin(),
                           [](std::uint8_t b) { return id_alphabet[b % id_alphabet.size()]; });
    return out;
}

/**
 * The SHA-256 of the concatenation of `parts`, fed to the hash one part at a time.
 *
 * Tip: every digest the protocol takes is of a few ids joined by separators; hashing the parts in
 * place builds none.
 *
 * Tip: SHA-256 over webcpp::hash's backends cannot fail: the hasher starts, takes pieces of any
 * size and finishes into 32 bytes. That is this function's precondition; were it broken, a debug
 * build asserts and a release build returns zeros.
 *
 * @param parts The texts, in order.
 * @return The digest.
 */
inline std::array<std::uint8_t, 32> sha256_of(std::initializer_list<std::string_view> parts) {
    std::array<std::uint8_t, 32> out{};
    // SHA-256 over webcpp::hash's default backend has no failure to report; if one came, `out`
    // stays zeroed and the assertion says so in a debug build.
    auto hashing = webcpp::hash::hasher<webcpp::hash::digests::sha2_256>::make();
    BOOST_ASSERT(hashing.has_value());
    if (!hashing) {
        return out;
    }
    for (const auto part : parts) {
        const auto fed = hashing->update(
            std::span(reinterpret_cast<const std::uint8_t*>(part.data()), part.size()));
        BOOST_ASSERT(fed.has_value());
        if (!fed) {
            return out;
        }
    }
    const auto done = hashing->finish(out);
    BOOST_ASSERT(done.has_value());
    if (!done) {
        out.fill(0);
    }
    return out;
}

/**
 * The SHA-256 of a text.
 *
 * @param text The text.
 * @return The digest.
 */
inline std::array<std::uint8_t, 32> sha256(std::string_view text) {
    return sha256_of({text});
}

/** Trystero's library name, `libName`, the first part of every topic's path. */
inline constexpr std::string_view lib_name = "Trystero";
/** Trystero's library id, `libId`, the first part of every domain tag. */
inline constexpr std::string_view lib_id = "trystero";

/**
 * Trystero's `domainTag`: the text that separates one use of a key or a hash from another.
 *
 * @param name The use's name.
 * @param version The use's version.
 * @param memory The resource the tag is allocated in.
 * @return "trystero:<name>:v<version>".
 */
inline std::pmr::string domain_tag(
    std::string_view name, unsigned version,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(memory);
    out += lib_id;
    out += ':';
    out += name;
    out += ":v";
    out += std::to_string(version);
    return out;
}

/**
 * Trystero's `topicPath`: the JSON array of the parts, `["a","b"]`, which no choice of parts can
 * make equal to another's.
 *
 * @param parts The texts, in UTF-8.
 * @param memory The resource the path is allocated in.
 * @return The path.
 */
inline std::pmr::string topic_path(
    std::initializer_list<std::string_view> parts,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(memory);
    out += '[';
    bool first = true;
    for (const auto part : parts) {
        if (!first) {
            out += ',';
        }
        first = false;
        json::append_string_total(out, part);
    }
    out += ']';
    return out;
}

/**
 * Trystero's `hashTopic`: the base64url of the SHA-256 of a text's UTF-8 bytes.
 *
 * @param text The text, in UTF-8.
 * @param memory The resource the topic is allocated in.
 * @return The topic's name, 43 characters.
 */
inline std::pmr::string hash_topic(
    std::string_view text, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return to_base64url(sha256(text), memory);
}

/**
 * The text whose hash is the topic every member of a room announces on: the path of the library's
 * name, the app's id and the room's id.
 *
 * @param app_id The application's id.
 * @param room_id The room's id.
 * @param memory The resource the text is allocated in.
 * @return The path.
 */
inline std::pmr::string root_topic_text(
    std::string_view app_id, std::string_view room_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return topic_path({lib_name, app_id, room_id}, memory);
}

/**
 * The topic every member of a room announces on, the hash of @ref root_topic_text.
 *
 * @param app_id The application's id.
 * @param room_id The room's id.
 * @param memory The resource the topic is allocated in.
 * @return The topic's name.
 */
inline std::pmr::string root_topic(
    std::string_view app_id, std::string_view room_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return hash_topic(root_topic_text(app_id, room_id, memory), memory);
}

/**
 * The topic a peer receives its offers, answers and candidates on: the hash of the path of the
 * root topic's text and the peer's id.
 *
 * @param app_id The application's id.
 * @param room_id The room's id.
 * @param peer_id The peer's id.
 * @param memory The resource the topic is allocated in.
 * @return The topic's name.
 */
inline std::pmr::string peer_topic(
    std::string_view app_id, std::string_view room_id, std::string_view peer_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return hash_topic(topic_path({root_topic_text(app_id, room_id, memory), peer_id}, memory),
                      memory);
}

/**
 * The token that names a room inside a data channel's envelope: the hexadecimal SHA-256 of
 * "Trystero:<app>:<room>".
 *
 * @param app_id The application's id.
 * @param room_id The room's id.
 * @param memory The resource the token is allocated in.
 * @return The token, 64 hexadecimal digits.
 */
inline std::pmr::string room_token(
    std::string_view app_id, std::string_view room_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return to_hex(sha256_of({"Trystero:", app_id, ":", room_id}), memory);
}

/**
 * Trystero's `hash32`: FNV-1a over the UTF-8 bytes, then MurmurHash3's final mix.
 *
 * @param utf8 The text, in UTF-8.
 * @return The 32-bit hash.
 */
inline std::uint32_t hash32(std::string_view utf8) {
    std::uint32_t h = 0x811c9dc5U;
    for (const char c : utf8) {
        h = (h ^ static_cast<std::uint8_t>(c)) * 0x01000193U;
    }
    h ^= h >> 16U;
    h *= 0x85ebca6bU;
    h ^= h >> 13U;
    h *= 0xc2b2ae35U;
    h ^= h >> 16U;
    return h;
}

/**
 * Trystero's `strToNum`: @ref hash32 of the text, modulo `limit`.
 *
 * @param utf8 The text, in UTF-8.
 * @pre `limit != 0`.
 * @param limit The modulus; the default is JavaScript's `Number.MAX_SAFE_INTEGER`.
 * @return The number, in [0, `limit`).
 */
inline std::uint64_t str_to_num(std::string_view utf8, std::uint64_t limit = 9007199254740991ULL) {
    BOOST_ASSERT(limit != 0);
    return hash32(utf8) % limit;
}

}  // namespace webcpp::trystero::protocol

#endif
