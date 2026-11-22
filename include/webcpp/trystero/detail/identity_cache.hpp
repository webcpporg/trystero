// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_IDENTITY_CACHE_HPP
#define WEBCPP_TRYSTERO_DETAIL_IDENTITY_CACHE_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/ecdsa.hpp>
#include <webcpp/trystero/protocol/ids.hpp>

#include <webcpp/crypto/curves/p256.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/**
 * A peer's or a user's public key as 0.27.0's `getVerifiedPubKeyRecord` resolves it: the key,
 * read from its compact text and on the curve, with the peer id and the user id it derives.
 */
struct key_record {
    /** The key, ready to verify signatures. */
    webcpp::crypto::curves::p256::public_key key;
    /** Its compact text, `base64url(x) + "." + base64url(y)`. */
    std::pmr::string compact;
    /** The peer id the key derives, `protocol::derive_peer_id`. */
    std::pmr::string peer_id;
    /** The user id the key derives, `protocol::derive_user_id`. */
    std::pmr::string user_id;
};

/**
 * The verification an @ref identity_cache makes in production: `protocol::verify_p256`.
 *
 * A cache is a template of a verifier so that a test can count and substitute the verifications
 * (a seam of the tests, not of the public API): the bounds and the rule that a cached answer is
 * given without verifying are then proven without thousands of real signatures.
 */
struct protocol_verifier {
    /** Whether a signature is the key's, of the payload, `protocol::verify_p256`. */
    [[nodiscard]] bool operator()(std::span<const std::uint8_t> signature,
                                  std::span<const std::uint8_t> payload,
                                  const webcpp::crypto::curves::p256::public_key& key) const {
        return protocol::verify_p256(signature, payload, key);
    }
};

/**
 * The key records and the verified signatures of an app, 0.27.0's `resolvedPubKeyCache` and
 * `verifiedSigPayloads` (design D18): at most @ref max_records records and at most
 * @ref max_signatures signatures whose payloads total at most @ref max_signature_bytes, the least
 * recently used leaving first, every one in the app's resource.
 *
 * Tip: 0.27.0 keeps both caches for the whole page and resolves them asynchronously; the port
 * keeps them per app and resolves them at once (design D18), so rooms of one app share them and two
 * apps never do.
 */
template <class Verifier = protocol_verifier>
class basic_identity_cache {
public:
    /** How many key records the cache keeps, 0.27.0's `createBoundedCache` default. */
    static constexpr std::size_t max_records = 4096;
    /** How many verified signatures the cache keeps, 0.27.0's `maxSigCacheEntries`. */
    static constexpr std::size_t max_signatures = 8192;
    /** How many payload bytes the verified signatures keep in all, 0.27.0's
     * `maxSigCacheTotalBytes`. */
    static constexpr std::size_t max_signature_bytes = 2 * 1024 * 1024;
    /** The longest payload whose signature is kept, 0.27.0's `maxCachedSigPayloadBytes`. */
    static constexpr std::size_t max_cached_payload_bytes = 4096;

    /**
     * An empty cache.
     *
     * @param memory The resource of the records and the signatures: the app's.
     * @param verifier What verifies a signature that is not cached, called with a signature of low
     * S only.
     */
    explicit basic_identity_cache(std::pmr::memory_resource* memory, Verifier verifier = {})
        : verifier_(std::move(verifier)),
          memory_(memory),
          records_(memory),
          record_order_(memory),
          signatures_(memory),
          signature_order_(memory) {}

    /** The maps hold views of their lists' nodes: no copy. */
    basic_identity_cache(const basic_identity_cache&) = delete;
    /** Not assignable either. */
    basic_identity_cache& operator=(const basic_identity_cache&) = delete;

    /** Frees every record and signature. */
    ~basic_identity_cache() = default;

    /**
     * The record of a compact key, 0.27.0's `getVerifiedPubKeyRecord`: the cached one, made the
     * most recently used, or a new one when the text is compact
     * (`protocol::parse_compact_public_key`) and its point is on the curve, which evicts the least
     * recently used record once
     * @ref max_records are kept. A refused text is not remembered.
     *
     * @param compact The key's compact text.
     * @return The record, valid until a later call of this cache adds a record; nullptr when the
     * text or its point is refused.
     */
    [[nodiscard]] const key_record* record(std::string_view compact) {
        if (const auto it = records_.find(compact); it != records_.end()) {
            record_order_.splice(record_order_.begin(), record_order_, it->second);
            return &*it->second;
        }
        const auto point = protocol::parse_compact_public_key(compact, memory_);
        if (!point) {
            return nullptr;
        }
        auto key = protocol::p256_public_key(*point);
        if (!key) {
            return nullptr;
        }
        if (record_order_.size() >= max_records) {
            records_.erase(record_order_.back().compact);
            record_order_.pop_back();
        }
        record_order_.push_front(key_record{
            .key = std::move(*key),
            .compact = std::pmr::string(compact, memory_),
            .peer_id = protocol::derive_peer_id(compact, memory_),
            .user_id = protocol::derive_user_id(compact, memory_),
        });
        records_.emplace(record_order_.front().compact, record_order_.begin());
        return &record_order_.front();
    }

    /**
     * Whether a signature is the key's, of the payload, 0.27.0's `verifyWithP256`: 64 bytes, low S
     * (refused before any verification, by the cache itself), by the key of a record
     * (@ref record).
     *
     * A payload of at most @ref max_cached_payload_bytes is first looked up: when the key and the
     * signature were verified before, the answer is whether the payload is the one they signed,
     * without verifying, and they become the most recently used. Otherwise it is verified, and a
     * signature that verifies is kept with a copy of its payload, the least recently used leaving
     * while @ref max_signatures are kept or the payloads would exceed @ref max_signature_bytes. A
     * longer payload is verified each time and never kept.
     *
     * @param compact The signer's compact key.
     * @param signature The signature, r then s.
     * @param payload The bytes signed.
     * @return Whether the signature is the key's, of `payload`.
     */
    [[nodiscard]] bool verify(std::string_view compact, std::span<const std::uint8_t> signature,
                              std::span<const std::uint8_t> payload) {
        if (signature.size() != std::tuple_size_v<protocol::p256_signature>) {
            return false;
        }
        const key_record* const rec = record(compact);
        if (rec == nullptr) {
            return false;
        }
        // Only the signature cache is touched from here to the end, so `rec` stays valid; a call of
        // record() in between could evict it.
        if (payload.size() > max_cached_payload_bytes) {
            return protocol::is_low_s(signature) && verifier_(signature, payload, rec->key);
        }
        const signature_key key = key_of(rec->compact, signature);
        const std::string_view key_text(key.data(), key.size());
        if (const auto it = signatures_.find(key_text); it != signatures_.end()) {
            signature_order_.splice(signature_order_.begin(), signature_order_, it->second);
            return std::ranges::equal(it->second->payload, payload);
        }
        if (!protocol::is_low_s(signature) || !verifier_(signature, payload, rec->key)) {
            return false;
        }
        while (signature_order_.size() >= max_signatures ||
               (!signature_order_.empty() &&
                signature_bytes_ + payload.size() > max_signature_bytes)) {
            signature_bytes_ -= signature_order_.back().payload.size();
            signatures_.erase(std::string_view(signature_order_.back().key.data(),
                                               signature_order_.back().key.size()));
            signature_order_.pop_back();
        }
        signature_order_.push_front(signature_entry{
            .key = key,
            .payload = std::pmr::vector<std::uint8_t>(payload.begin(), payload.end(), memory_),
        });
        signatures_.emplace(std::string_view(signature_order_.front().key.data(),
                                             signature_order_.front().key.size()),
                            signature_order_.begin());
        signature_bytes_ += payload.size();
        return true;
    }

    /**
     * Whether a peer id is the one a key derives, 0.27.0's `verifyPeerIdKey`: an empty id is
     * refused before any lookup.
     *
     * @param peer_id The id claimed.
     * @param compact The key's compact text.
     * @return Whether the key's record exists and derives `peer_id`.
     */
    [[nodiscard]] bool verify_peer_id(std::string_view peer_id, std::string_view compact) {
        if (peer_id.empty()) {
            return false;
        }
        const key_record* const rec = record(compact);
        return rec != nullptr && rec->peer_id == peer_id;
    }

    /**
     * Whether a user id is the one a key derives, 0.27.0's `verifyUserIdKey`: an id that is not
     * `protocol::user_id_size` base62 characters (its `userIdRegex`) is refused before any lookup.
     *
     * @param user_id The id claimed.
     * @param compact The key's compact text.
     * @return Whether the key's record exists and derives `user_id`.
     */
    [[nodiscard]] bool verify_user_id(std::string_view user_id, std::string_view compact) {
        if (user_id.size() != protocol::user_id_size ||
            user_id.find_first_not_of(protocol::base62_alphabet) != std::string_view::npos) {
            return false;
        }
        const key_record* const rec = record(compact);
        return rec != nullptr && rec->user_id == user_id;
    }

    /** How many key records the cache keeps now. */
    [[nodiscard]] std::size_t records() const noexcept { return record_order_.size(); }

    /** Whether the cache keeps the record of `compact`, which this leaves where it is in the
     * order of use. */
    [[nodiscard]] bool holds_record(std::string_view compact) const {
        return records_.contains(compact);
    }

    /** How many verified signatures the cache keeps now. */
    [[nodiscard]] std::size_t signatures() const noexcept { return signature_order_.size(); }

    /** How many payload bytes the verified signatures keep now. */
    [[nodiscard]] std::size_t signature_bytes() const noexcept { return signature_bytes_; }

    /** Whether the cache keeps `signature` as verified by `compact`, which this leaves where it is
     * in the order of use. */
    [[nodiscard]] bool holds_signature(std::string_view compact,
                                       std::span<const std::uint8_t> signature) const {
        if (compact.size() != compact_size ||
            signature.size() != std::tuple_size_v<protocol::p256_signature>) {
            return false;
        }
        const signature_key key = key_of(compact, signature);
        return signatures_.contains(std::string_view(key.data(), key.size()));
    }

private:
    /** The length of a compact key's text. */
    static constexpr std::size_t compact_size = 87;
    /** A signature's key in the cache: the signer's compact text, then the signature's bytes. */
    using signature_key =
        std::array<char, compact_size + std::tuple_size_v<protocol::p256_signature>>;

    /** A verified signature: its key and a copy of the payload it signs. */
    struct signature_entry {
        signature_key key;
        std::pmr::vector<std::uint8_t> payload;
    };

    /** The key of a compact text of @ref compact_size characters and a signature of 64 bytes. */
    static signature_key key_of(std::string_view compact, std::span<const std::uint8_t> signature) {
        signature_key key{};
        std::ranges::copy(compact, key.begin());
        std::ranges::transform(signature, key.begin() + compact_size,
                               [](std::uint8_t byte) { return static_cast<char>(byte); });
        return key;
    }

    Verifier verifier_;
    std::pmr::memory_resource* memory_;
    // By compact text, each key a view of its record's text in the order list, which owns it.
    std::pmr::map<std::string_view, typename std::pmr::list<key_record>::iterator, std::less<>>
        records_;
    // The records, the most recently used first.
    std::pmr::list<key_record> record_order_;
    // By key, each a view of its entry's key in the order list, which owns it.
    std::pmr::map<std::string_view, typename std::pmr::list<signature_entry>::iterator, std::less<>>
        signatures_;
    // The verified signatures, the most recently used first.
    std::pmr::list<signature_entry> signature_order_;
    std::size_t signature_bytes_ = 0;
};

/** The cache of an app: its verifications are `protocol::verify_p256`. */
using identity_cache = basic_identity_cache<>;

}  // namespace webcpp::trystero::detail

#endif
