// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_ROOM_KEYS_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_ROOM_KEYS_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/crypto/equal.hpp>
#include <webcpp/crypto/secret.hpp>
#include <webcpp/hash/hkdf.hpp>
#include <webcpp/hash/hmac.hpp>
#include <webcpp/trystero/detail/wipe.hpp>

#include <boost/assert.hpp>
#include <boost/system/result.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

namespace webcpp::trystero::protocol {

/** The iterations of the PBKDF2 that turns a room's password into its master key, 0.27.0's. */
inline constexpr std::uint64_t pbkdf2_iterations = 600'000;

/** The largest sequence number of a room authentication token, `Number.MAX_SAFE_INTEGER`. */
inline constexpr std::uint64_t max_room_auth_seq = (std::uint64_t{1} << 53U) - 1;

/**
 * The keys of a room that has a password: the AES-256-GCM key its signals are sealed under and the
 * HMAC-SHA-256 key its authentication tokens are signed with.
 */
struct room_keys {
    /** The key the room's signals are sealed under. */
    webcpp::crypto::secret<32> aes;
    /** The key the room's authentication tokens are signed with. */
    webcpp::crypto::secret<32> hmac;
};

namespace detail {

/** The bytes of a text, as a span. */
inline std::span<const std::uint8_t> bytes_of_text(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

}  // namespace detail

/**
 * The AES key of a room without a password: the SHA-256 of
 * `JSON.stringify(["trystero:room-aes:v3", appId, roomId])`.
 *
 * @param app_id The app id, in UTF-8.
 * @param room_id The room id, in UTF-8.
 * @param memory The resource the hashed text is allocated in.
 * @return The key.
 */
inline webcpp::crypto::secret<32> room_aes_key_without_password(
    std::string_view app_id, std::string_view room_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    webcpp::crypto::secret<32> key;
    auto digest = sha256(topic_path({domain_tag("room-aes", 3, memory), app_id, room_id}, memory));
    std::ranges::copy(digest, key.writable().begin());
    // webcpp::crypto has no public wipe of an array yet; its detail one is the library's own.
    webcpp::trystero::detail::wipe_object(digest);
    return key;
}

/**
 * The salt of the PBKDF2 that turns a room's password into its master key: the SHA-256 of
 * `JSON.stringify(["trystero:pbkdf2-master:v3", appId, roomId])`.
 *
 * @param app_id The app id, in UTF-8.
 * @param room_id The room id, in UTF-8.
 * @param memory The resource the hashed text is allocated in.
 * @return The salt.
 */
inline std::array<std::uint8_t, 32> pbkdf2_salt(
    std::string_view app_id, std::string_view room_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return sha256(topic_path({domain_tag("pbkdf2-master", 3, memory), app_id, room_id}, memory));
}

/**
 * The AES and HMAC keys of a room that has a password, from its PBKDF2 master: two HKDF-SHA-256
 * derivations of 32 bytes over a salt of 32 zero bytes, with the infos `trystero:room-aes:v3` and
 * `trystero:room-hmac:v3`, which is what WebCrypto's `deriveKey` of a 256-bit key yields.
 *
 * @param master The 32 bytes of PBKDF2 over the password, @ref pbkdf2_salt and
 * @ref pbkdf2_iterations.
 * @param memory The resource the infos are allocated in.
 * @return The keys, or the error of the derivation.
 */
inline boost::system::result<room_keys> derive_room_keys(
    std::span<const std::uint8_t, 32> master,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    constexpr std::array<std::uint8_t, 32> no_salt{};
    room_keys keys;
    const auto aes_info = domain_tag("room-aes", 3, memory);
    auto derived = webcpp::hash::hkdf<webcpp::hash::hmac_sha2_256>(
        keys.aes.writable(), no_salt, master, detail::bytes_of_text(aes_info));
    if (!derived) {
        return derived.error();
    }
    const auto hmac_info = domain_tag("room-hmac", 3, memory);
    derived = webcpp::hash::hkdf<webcpp::hash::hmac_sha2_256>(keys.hmac.writable(), no_salt, master,
                                                              detail::bytes_of_text(hmac_info));
    if (!derived) {
        return derived.error();
    }
    return keys;
}

/**
 * The text a room authentication token is the MAC of:
 * `JSON.stringify(["trystero:ov-auth:v3", appId, roomId, peerId, seq])`.
 *
 * @param app_id The app id, in UTF-8.
 * @param room_id The room id, in UTF-8.
 * @param peer_id The id of the peer the token is for.
 * @param seq The token's sequence number.
 * @param memory The resource the text is allocated in.
 * @pre `seq` is at most @ref max_room_auth_seq: a number a peer sends is checked by
 * @ref verify_room_auth, which refuses a larger one, before it reaches here.
 * @return The text.
 */
inline std::pmr::string room_auth_payload(
    std::string_view app_id, std::string_view room_id, std::string_view peer_id, std::uint64_t seq,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    BOOST_ASSERT(seq <= max_room_auth_seq);
    std::pmr::string out(memory);
    out += '[';
    json::append_string_total(out, domain_tag("ov-auth", 3, memory));
    for (const auto part : {app_id, room_id, peer_id}) {
        out += ',';
        json::append_string_total(out, part);
    }
    out += ',';
    json::append_integer(out, static_cast<std::int64_t>(seq));
    out += ']';
    return out;
}

namespace detail {

/** The HMAC-SHA-256 of a room authentication payload. */
inline std::array<std::uint8_t, 32> room_auth_mac(const webcpp::crypto::secret<32>& hmac_key,
                                                  std::string_view app_id, std::string_view room_id,
                                                  std::string_view peer_id, std::uint64_t seq,
                                                  std::pmr::memory_resource* memory) {
    const auto payload = room_auth_payload(app_id, room_id, peer_id, seq, memory);
    const auto key = hmac_key.bytes();
    webcpp::hash::hmac_sha2_256 mac(key.data(), key.size());
    mac.update(payload.data(), payload.size());
    const auto result = mac.result();
    std::array<std::uint8_t, 32> out{};
    std::ranges::copy(result, out.begin());
    return out;
}

}  // namespace detail

/**
 * A room authentication token: the base64url of the HMAC-SHA-256 of @ref room_auth_payload.
 *
 * @param hmac_key The room's HMAC key.
 * @param app_id The app id, in UTF-8.
 * @param room_id The room id, in UTF-8.
 * @param peer_id The id of the peer the token is for.
 * @param seq The token's sequence number, at most @ref max_room_auth_seq.
 * @param memory The resource the token and its working text are allocated in.
 * @pre `seq` is at most @ref max_room_auth_seq.
 * @return The token, 43 characters.
 */
inline std::pmr::string sign_room_auth(
    const webcpp::crypto::secret<32>& hmac_key, std::string_view app_id, std::string_view room_id,
    std::string_view peer_id, std::uint64_t seq,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return to_base64url(detail::room_auth_mac(hmac_key, app_id, room_id, peer_id, seq, memory),
                        memory);
}

/**
 * Whether a token is the one @ref sign_room_auth makes: it must read as strict base64url of 32
 * bytes, and equal the recomputed MAC in constant time.
 *
 * @param hmac_key The room's HMAC key.
 * @param app_id The app id, in UTF-8.
 * @param room_id The room id, in UTF-8.
 * @param peer_id The id of the peer the token claims to be for.
 * @param seq The sequence number the token claims.
 * @param token The token received.
 * @param memory The resource the working text is allocated in.
 * @return Whether the token is genuine; false, as 0.27.0 answers, when `seq` is beyond
 * @ref max_room_auth_seq.
 */
inline bool verify_room_auth(const webcpp::crypto::secret<32>& hmac_key, std::string_view app_id,
                             std::string_view room_id, std::string_view peer_id, std::uint64_t seq,
                             std::string_view token,
                             std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    if (seq > max_room_auth_seq) {
        return false;
    }
    // 32 bytes are 43 characters unpadded; any other length is refused before it is decoded, so
    // that the decoding below fits its arena.
    constexpr std::size_t token_size = 43;
    if (token.size() != token_size) {
        return false;
    }
    std::array<std::uint8_t, 128> storage{};
    std::pmr::monotonic_buffer_resource arena(storage.data(), storage.size(),
                                              std::pmr::null_memory_resource());
    const auto given = from_base64url(token, &arena);
    if (!given || given->size() != 32) {
        return false;
    }
    const auto expected = detail::room_auth_mac(hmac_key, app_id, room_id, peer_id, seq, memory);
    return webcpp::crypto::equal(std::span<const std::uint8_t>(*given), expected);
}

}  // namespace webcpp::trystero::protocol

#endif
