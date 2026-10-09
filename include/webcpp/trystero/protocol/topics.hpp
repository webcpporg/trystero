// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_TOPICS_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_TOPICS_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/text.hpp>

#include <boost/hash2/sha1.hpp>
#include <boost/hash2/sha2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <random>
#include <string>
#include <string_view>

namespace webcpp::trystero::protocol {

/** The 62 characters Trystero draws peer ids, offer ids and nonces from, in its order. */
inline constexpr std::string_view id_alphabet =
    "0123456789AaBbCcDdEeFfGgHhIiJjKkLlMmNnOoPpQqRrSsTtUuVvWwXxYyZz";

/** Trystero's peer ids are 20 characters, its offer ids 12, announcement nonces 8. */
inline constexpr std::size_t peer_id_size = 20;
inline constexpr std::size_t offer_id_size = 12;
inline constexpr std::size_t nonce_size = 8;

/** A random id of `n` characters of `id_alphabet`, Trystero's `genId`. */
template <class Generator>
std::pmr::string make_id(std::size_t n, Generator& generator,
                         std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    // A distribution draws through a non-const operator(), which misc-const-correctness does not
    // see through the dependent generator.
    // NOLINTNEXTLINE(misc-const-correctness)
    std::uniform_int_distribution<std::size_t> pick(0, id_alphabet.size() - 1);
    std::pmr::string out(n, '0', memory);
    for (auto& c : out) {
        c = id_alphabet[pick(generator)];
    }
    return out;
}

/**
 * SHA-256 of the concatenation of `parts`, fed to the hash one part at a time.
 * Tip: every digest the protocol takes is of a few ids joined by separators; hashing the parts in
 * place builds none.
 */
inline std::array<std::uint8_t, 32> sha256_of(std::initializer_list<std::string_view> parts) {
    boost::hash2::sha2_256 h;
    for (const auto part : parts) {
        h.update(part.data(), part.size());
    }
    const auto digest = h.result();
    std::array<std::uint8_t, 32> out{};
    std::ranges::copy(digest, out.begin());
    return out;
}

inline std::array<std::uint8_t, 32> sha256(std::string_view text) {
    return sha256_of({text});
}

/**
 * Trystero's `sha1` of the concatenation of `parts`: the SHA-1 digest with every byte written in
 * base 36 without padding, concatenated. Tip: the text is not fixed width; it is a topic name and
 * is only ever compared, never decoded.
 */
inline std::pmr::string sha1_topic_of(
    std::initializer_list<std::string_view> parts,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    boost::hash2::sha1_160 h;
    for (const auto part : parts) {
        h.update(part.data(), part.size());
    }
    std::pmr::string out(memory);
    out.reserve(40);
    for (std::uint8_t b : h.result()) {
        std::array<char, 2> digits{};
        std::size_t n = 0;
        do {
            digits[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[b % 36];
            b /= 36;
        } while (b != 0);
        while (n != 0) {
            out += digits[--n];
        }
    }
    return out;
}

inline std::pmr::string sha1_topic(
    std::string_view text, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return sha1_topic_of({text}, memory);
}

/** The topic every member of a room announces on: Trystero's sha1 of "Trystero@<app>@<room>". */
inline std::pmr::string root_topic(
    std::string_view app_id, std::string_view room_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return sha1_topic_of({"Trystero@", app_id, "@", room_id}, memory);
}

/** The topic a peer receives its offers, answers and candidates on: the root path, "@", the peer
 * id. */
inline std::pmr::string peer_topic(
    std::string_view app_id, std::string_view room_id, std::string_view peer_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return sha1_topic_of({"Trystero@", app_id, "@", room_id, "@", peer_id}, memory);
}

/** The token that names a room inside a data channel's envelope: hex SHA-256 of
 * "Trystero:<app>:<room>". */
inline std::pmr::string room_token(
    std::string_view app_id, std::string_view room_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return to_hex(sha256_of({"Trystero:", app_id, ":", room_id}), memory);
}

/** Trystero's `strToNum`: the sum of the text's UTF-16 code units, modulo `limit`. */
inline double str_to_num(std::string_view text, double limit = 9007199254740991.0) {
    return std::fmod(utf16_code_unit_sum(text), limit);
}

}  // namespace webcpp::trystero::protocol

#endif
