// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 A user's identity, Trystero 0.27.0's `UserIdentity`: a P-256 key whose public key derives the
 user's id, which one or several apps present as their user; made, imported and exported as
 0.27.0's `createUserIdentity`, `importUserIdentity` and `exportUserIdentity` make, import and
 export it.
*/
#ifndef WEBCPP_TRYSTERO_IDENTITY_HPP
#define WEBCPP_TRYSTERO_IDENTITY_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/protocol/ecdsa.hpp>
#include <webcpp/trystero/protocol/identity.hpp>
#include <webcpp/trystero/protocol/ids.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/crypto/curves/p256.hpp>

#include <boost/assert.hpp>
#include <boost/system/result.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace webcpp::trystero {

/**
 * Where this peer's user identity comes from, Trystero's `IdentityPersistence` (design D17).
 */
enum class identity_persistence {
    /** Made with the app and lost with it, 0.27.0's `'none'`. */
    ephemeral,
    /** Given to the app by the program, which made or imported it, 0.27.0's `'custom'`. */
    imported,
};

class user_identity;

namespace detail {

/** A P-256 key pair and its public point, as trystero's identities hold them. */
struct p256_key {
    /** The key pair. */
    webcpp::crypto::curves::p256::keypair key;
    /** Its public point, SEC 1's uncompressed form. */
    std::array<std::uint8_t, 65> point;
};

/**
 * The P-256 key pair of a scalar, or one drawn from the system's generator, with its point.
 *
 * Tip: this, `import_user_identity` and @ref protocol::sign_p256 and @ref protocol::verify_p256
 * are trystero's only calls of webcpp::crypto's P-256 keys, so that a change of its interface is
 * made in these two headers alone.
 *
 * @param scalar The secret scalar, 32 bytes big-endian; empty draws one.
 * @return The key and its point; webcpp::crypto's error when the scalar is no valid key or the
 * system has no generator.
 */
[[nodiscard]] inline boost::system::result<p256_key> make_p256_key(
    std::span<const std::uint8_t> scalar) {
    using keypair = webcpp::crypto::curves::p256::keypair;
    auto key = scalar.empty() ? keypair::generate() : keypair::make(scalar);
    if (!key) {
        return key.error();
    }
    const auto public_key = key->public_key();
    if (!public_key) {
        return public_key.error();
    }
    return p256_key{.key = std::move(*key), .point = public_key->uncompressed()};
}

/** An app's endpoint key, 0.27.0's endpoint identity: its key pair, its compact public key and
 * the peer id it derives. */
struct endpoint_identity {
    /** The key pair. */
    webcpp::crypto::curves::p256::keypair key;
    /** Its compact public key. */
    std::pmr::string compact;
    /** The peer id, derived from the compact key. */
    std::pmr::string peer_id;
};

/**
 * Makes an endpoint key, 0.27.0's `createEndpointIdentity`: of a scalar, or drawn from the
 * system's generator.
 *
 * @param scalar The secret scalar, 32 bytes big-endian; empty draws one.
 * @param memory The resource the compact key and the peer id are allocated in.
 * @return The endpoint key; nothing when the scalar is no valid key or the system has no
 * generator.
 */
[[nodiscard]] inline std::optional<endpoint_identity> make_endpoint_identity(
    std::span<const std::uint8_t> scalar, std::pmr::memory_resource* memory) {
    auto made = make_p256_key(scalar);
    if (!made) {
        return std::nullopt;
    }
    auto compact = protocol::compact_public_key(made->point, memory);
    auto peer_id = protocol::derive_peer_id(compact, memory);
    return endpoint_identity{
        .key = std::move(made->key),
        .compact = std::move(compact),
        .peer_id = std::move(peer_id),
    };
}

/** What a @ref user_identity holds: its key pair, its point and the texts derived from it. */
struct user_key {
    /** The key pair. */
    webcpp::crypto::curves::p256::keypair key;
    /** Its public point, SEC 1's uncompressed form. */
    std::array<std::uint8_t, 65> point;
    /** Its compact public key. */
    std::pmr::string compact;
    /** The user's id, derived from the compact key. */
    std::pmr::string user_id;
};

/** Reaches what a @ref user_identity holds, for the functions that make and export one. */
struct user_identity_access {
    /** The key `identity` holds. */
    [[nodiscard]] static const user_key& key(const user_identity& identity) noexcept;

    /**
     * The identity of a key pair and its point: the key, with its compact key and its user's id,
     * allocated in `memory` through a `std::pmr::polymorphic_allocator`.
     *
     * @param key The key pair.
     * @param point Its public point.
     * @param memory The resource the key is allocated in.
     * @return The identity.
     */
    [[nodiscard]] static user_identity make(webcpp::crypto::curves::p256::keypair key,
                                            const std::array<std::uint8_t, 65>& point,
                                            std::pmr::memory_resource* memory);

    /**
     * The identity of a scalar, or of one drawn from the system's generator, as
     * `create_user_identity` makes it: the library's own tests fix their users' keys with it.
     *
     * @param scalar The secret scalar, 32 bytes big-endian; empty draws one.
     * @param memory The resource the key is allocated in, which outlives every copy.
     * @return The identity; webcpp::crypto's error when the scalar is no valid key or the system
     * has no generator.
     */
    [[nodiscard]] static boost::system::result<user_identity> make(
        std::span<const std::uint8_t> scalar, std::pmr::memory_resource* memory);
};

}  // namespace detail

/**
 * A user's identity, 0.27.0's `UserIdentity`: a P-256 key pair, the user's id its public key
 * derives, and the signatures it makes.
 *
 * It is a handle: copies share one immutable key, allocated in the resource it was made with, which
 * must outlive every copy. Several apps given copies are one user's endpoints, as several pages of
 * one imported identity are in 0.27.0.
 *
 * @see "Peers and users", in the guide.
 */
class user_identity {
public:
    /**
     * A copy of `other`, which shares its key.
     *
     * Tip: an identity has no move of its own, so that none is ever empty: a move copies.
     *
     * @param other The identity copied.
     */
    user_identity(const user_identity& other) = default;

    /**
     * Makes this a copy of `other`, sharing its key.
     *
     * @param other The identity copied.
     * @return This identity.
     */
    user_identity& operator=(const user_identity& other) = default;

    /** Releases this copy's share of the key; the last copy frees it. */
    ~user_identity() = default;

    /**
     * The user's id, 0.27.0's `userId`: 22 base62 characters derived from the public key.
     *
     * @return The id, valid while a copy of this identity lives.
     */
    [[nodiscard]] std::string_view user_id() const noexcept { return key_->user_id; }

    /**
     * The user's public key in its compact form, `base64url(x) + "." + base64url(y)`.
     *
     * @return The key, 87 characters, valid while a copy of this identity lives.
     */
    [[nodiscard]] std::string_view public_key() const noexcept { return key_->compact; }

    /**
     * Signs a payload as the user, 0.27.0's `sign`: ECDSA P-256 of its SHA-256, low S.
     *
     * @param payload The bytes signed.
     * @return The signature, r then s; webcpp::crypto's error when the key cannot sign, which it
     * never is once made.
     */
    [[nodiscard]] boost::system::result<protocol::p256_signature> sign(
        std::span<const std::uint8_t> payload) const {
        return protocol::sign_p256(key_->key, payload);
    }

private:
    explicit user_identity(std::shared_ptr<const detail::user_key> key) : key_(std::move(key)) {}

    friend struct detail::user_identity_access;

    std::shared_ptr<const detail::user_key> key_;
};

inline const detail::user_key& detail::user_identity_access::key(
    const user_identity& identity) noexcept {
    return *identity.key_;
}

inline user_identity detail::user_identity_access::make(webcpp::crypto::curves::p256::keypair key,
                                                        const std::array<std::uint8_t, 65>& point,
                                                        std::pmr::memory_resource* memory) {
    auto compact = protocol::compact_public_key(point, memory);
    auto user_id = protocol::derive_user_id(compact, memory);
    return user_identity(std::allocate_shared<user_key>(std::pmr::polymorphic_allocator<>(memory),
                                                        user_key{
                                                            .key = std::move(key),
                                                            .point = point,
                                                            .compact = std::move(compact),
                                                            .user_id = std::move(user_id),
                                                        }));
}

inline boost::system::result<user_identity> detail::user_identity_access::make(
    std::span<const std::uint8_t> scalar, std::pmr::memory_resource* memory) {
    auto made = make_p256_key(scalar);
    if (!made) {
        return made.error();
    }
    return make(std::move(made->key), made->point, memory);
}

/**
 * Makes a new user identity, 0.27.0's `createUserIdentity`: a P-256 key pair drawn from the
 * system's generator.
 *
 * @param memory The resource the key is allocated in, which outlives every copy of the identity.
 * @return The identity; webcpp::crypto's `entropy_unavailable` when the system has no generator.
 */
[[nodiscard]] inline boost::system::result<user_identity> create_user_identity(
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return detail::user_identity_access::make(std::span<const std::uint8_t>(), memory);
}

/**
 * Imports a user identity from its text, 0.27.0's `importUserIdentity`: the text
 * @ref export_user_identity writes, or the JSON of a private P-256 key (a JWK), read as
 * `protocol::parse_user_key` reads them; the key must then sign the bytes of
 * `trystero:identity-self-test:v1` with a signature its point verifies.
 *
 * The scalar is wiped from every buffer of trystero's that held it; the program's text is its
 * own.
 *
 * @param text The text.
 * @param memory The resource the key and the import's buffers are allocated in, which outlives
 * every copy of the identity.
 * @return The identity; `error::invalid_user_identity` for a text that is not a user key, a point
 * that is not on the curve and a scalar that is not the point's.
 */
[[nodiscard]] inline boost::system::result<user_identity> import_user_identity(
    std::string_view text, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    namespace p256 = webcpp::crypto::curves::p256;
    const auto material = protocol::parse_user_key(text, memory);
    if (!material) {
        return error::invalid_user_identity;
    }
    auto key = p256::keypair::make(material->d.bytes());
    const auto point = p256::public_key::make(material->point);
    if (!key || !point) {
        return error::invalid_user_identity;
    }
    // 0.27.0's self-test: the key signs its payload, and the given point verifies the signature.
    const auto payload = protocol::domain_tag("identity-self-test", 1, memory);
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                              payload.size());
    const auto signature = protocol::sign_p256(*key, bytes);
    if (!signature || !protocol::verify_p256(*signature, bytes, *point)) {
        return error::invalid_user_identity;
    }
    return detail::user_identity_access::make(std::move(*key), material->point, memory);
}

/**
 * The text of a user identity, 0.27.0's `exportUserIdentity`: @ref protocol::user_key_prefix and
 * the base64url of the JSON `{"d":..,"x":..,"y":..}`, which @ref import_user_identity reads.
 *
 * The text holds the secret key: a program keeps it as it keeps a password, and wipes it when
 * done.
 *
 * @param identity The identity.
 * @param memory The resource the text is allocated in.
 * @return The text, 214 characters.
 */
[[nodiscard]] inline std::pmr::string export_user_identity(
    const user_identity& identity,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    const auto& key = detail::user_identity_access::key(identity);
    auto scalar = key.key.secret_scalar();
    // A key pair made by webcpp::crypto and never moved from has its scalar.
    BOOST_ASSERT(scalar.has_value());
    protocol::user_key_material material;
    if (scalar) {
        material.d = std::move(*scalar);
    }
    material.point = key.point;
    return protocol::write_user_key(material, memory);
}

}  // namespace webcpp::trystero

#endif
