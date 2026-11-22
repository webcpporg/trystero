// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 ECDSA P-256 with SHA-256 as Trystero 0.27.0 signs and checks it: a signature is 64 bytes, r then
 s (IEEE P1363, WebCrypto's form), always written with a low S and refused with a high one.

 Tip: @ref sign_p256 and @ref verify_p256 are trystero's only calls of webcpp::crypto's P-256
 signing and verification; every other part of the library signs and verifies through them, so
 that a change of webcpp::crypto's signing interface is made here alone.
*/
#ifndef WEBCPP_TRYSTERO_PROTOCOL_ECDSA_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_ECDSA_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/crypto/curves/p256.hpp>

#include <boost/system/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace webcpp::trystero::protocol {

/** A P-256 signature: r then s, 32 bytes each, big-endian (IEEE P1363, WebCrypto's form). */
using p256_signature = std::array<std::uint8_t, 64>;

/** The order n of P-256's group, big-endian: a secret scalar is in [1, n - 1]. */
inline constexpr std::array<std::uint8_t, 32> p256_order{
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};

/**
 * Whether 32 bytes are a P-256 secret scalar, 0.27.0's `isValidP256PrivateScalar`: not zero, and
 * below the group order.
 *
 * The comparison reads every byte whatever their values: the bytes are a secret.
 *
 * @param scalar The scalar, big-endian.
 * @return Whether it is in [1, n - 1].
 */
[[nodiscard]] inline bool is_p256_scalar(std::span<const std::uint8_t, 32> scalar) noexcept {
    // A subtraction scalar - n from the last byte up: its final borrow says scalar < n.
    unsigned borrow = 0;
    unsigned any = 0;
    for (std::size_t i = scalar.size(); i != 0; --i) {
        const unsigned difference = unsigned{scalar[i - 1]} - unsigned{p256_order[i - 1]} - borrow;
        borrow = (difference >> 8U) & 1U;
        any |= scalar[i - 1];
    }
    return (borrow == 1U) && (any != 0U);
}

/**
 * Whether a signature is in the form 0.27.0 accepts, its `isLowSP256Signature`: 64 bytes, r in
 * [1, n - 1], and s in [1, (n - 1) / 2].
 *
 * It does not verify the signature: @ref verify_p256 does.
 *
 * @param signature The signature.
 * @return Whether it has 0.27.0's form.
 */
[[nodiscard]] inline bool is_low_s(std::span<const std::uint8_t> signature) noexcept {
    if (signature.size() != std::tuple_size_v<p256_signature>) {
        return false;
    }
    return webcpp::crypto::curves::p256::check_low_s(signature.first<64>()).has_value();
}

/**
 * Signs a payload as 0.27.0's `ecdsaSign` signs it: ECDSA of the SHA-256 of the payload, its S
 * made low.
 *
 * Tip: webcpp::crypto's nonce is RFC 6979's, so the same key and payload give the same signature,
 * where WebCrypto's are random; 0.27.0 verifies both alike.
 *
 * @param key The signer's key pair.
 * @param payload The bytes signed.
 * @return The signature, low S; webcpp::crypto's error when the key pair cannot sign, which a key
 * pair made by webcpp::crypto and not moved from never is.
 */
[[nodiscard]] inline boost::system::result<p256_signature> sign_p256(
    const webcpp::crypto::curves::p256::keypair& key, std::span<const std::uint8_t> payload) {
    const auto digest =
        sha256(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
    const auto signature = key.sign_ecdsa(digest);
    if (!signature) {
        return signature.error();
    }
    return webcpp::crypto::curves::p256::normalize_ecdsa(*signature);
}

/**
 * Verifies a signature as 0.27.0's `verifyWithP256` does, without its cache: a signature of 64
 * bytes, in low-S form (@ref is_low_s), of the SHA-256 of the payload, by `key`.
 *
 * @param signature The signature.
 * @param payload The bytes signed.
 * @param key The signer's public key.
 * @return Whether the signature is the key's, of the payload, with a low S.
 */
[[nodiscard]] inline bool verify_p256(std::span<const std::uint8_t> signature,
                                      std::span<const std::uint8_t> payload,
                                      const webcpp::crypto::curves::p256::public_key& key) {
    if (!is_low_s(signature)) {
        return false;
    }
    const auto digest =
        sha256(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
    return webcpp::crypto::curves::p256::verify_ecdsa(signature.first<64>(), digest, key)
        .has_value();
}

}  // namespace webcpp::trystero::protocol

#endif
