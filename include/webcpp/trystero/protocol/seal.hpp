// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_SEAL_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_SEAL_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/text.hpp>

#include <webcpp/crypto/ciphers/aes256_gcm.hpp>
#include <webcpp/crypto/secret.hpp>
#include <webcpp/trystero/detail/wipe.hpp>

#include <boost/system/result.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace webcpp::trystero::protocol {

/** The size of the IV of a sealed signal, AES-GCM's 12 bytes, 0.27.0's `gcmIvBytes`. */
inline constexpr std::size_t gcm_iv_size = 12;

/** The size of the tag of a sealed signal, 16 bytes, 0.27.0's `gcmTagBytes`. */
inline constexpr std::size_t gcm_tag_size = 16;

/**
 * The size of the text a plaintext seals to: the unpadded base64url of the IV, the ciphertext and
 * the tag.
 *
 * @param plaintext The plaintext's size, in bytes.
 * @return The size of the sealed text, in bytes.
 */
constexpr std::size_t sealed_size(std::size_t plaintext) {
    const std::size_t packed = gcm_iv_size + plaintext + gcm_tag_size;
    return ((4 * packed) + 2) / 3;
}

namespace detail {

/** The bytes of a text, as a span. */
inline std::span<const std::uint8_t> seal_bytes_of(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

}  // namespace detail

/**
 * Seals a signal's plaintext as 0.27.0's `encrypt` does: AES-256-GCM, and the base64url of the IV,
 * the ciphertext and the tag.
 *
 * An absent AAD and an empty one are the same to GCM; the parameter keeps Trystero's distinction
 * readable at the call.
 *
 * @param key The room's AES key.
 * @param iv The 12 bytes of the IV, never repeated under `key`.
 * @param plaintext The text to seal.
 * @param aad The additional authenticated data, @ref signal_aad for a signal; absent for none.
 * @param memory The resource the sealed text and the working buffer are allocated in.
 * @return The sealed text, @ref sealed_size characters, or the error of the cipher.
 */
[[nodiscard]] inline boost::system::result<std::pmr::string> seal(
    const webcpp::crypto::secret<32>& key, std::span<const std::uint8_t, gcm_iv_size> iv,
    std::string_view plaintext, std::optional<std::string_view> aad,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    auto cipher = webcpp::crypto::ciphers::aes256_gcm::make(key.bytes());
    if (!cipher) {
        return cipher.error();
    }
    // The working buffer holds the IV, then the plaintext, sealed in place, then the tag.
    bytes packed(memory);
    packed.reserve(gcm_iv_size + plaintext.size() + gcm_tag_size);
    packed.insert(packed.end(), iv.begin(), iv.end());
    const auto body = detail::seal_bytes_of(plaintext);
    packed.insert(packed.end(), body.begin(), body.end());
    const auto tag =
        cipher->seal(iv, aad ? detail::seal_bytes_of(*aad) : std::span<const std::uint8_t>{},
                     std::span<std::uint8_t>(packed).subspan(gcm_iv_size, plaintext.size()));
    if (!tag) {
        webcpp::trystero::detail::wipe(packed.data(), packed.size());
        return tag.error();
    }
    packed.insert(packed.end(), tag->begin(), tag->end());
    auto out = to_base64url(packed, memory);
    webcpp::trystero::detail::wipe(packed.data(), packed.size());
    return out;
}

/**
 * Opens a sealed signal as 0.27.0's `decrypt` does, strictly.
 *
 * @param key The room's AES key.
 * @param sealed The sealed text.
 * @param aad The additional authenticated data the seal used; absent for none.
 * @param memory The resource the plaintext and the working buffer are allocated in.
 * @return The plaintext; `error::decrypt_failed` for a text that is not strict base64url (what
 * @ref from_base64url refuses), one of fewer than 28 bytes, a tag that does not match the key and
 * the AAD, or a plaintext that is not UTF-8 (design D12); or the error of the cipher, when it
 * cannot be made.
 */
[[nodiscard]] inline boost::system::result<std::pmr::string> open(
    const webcpp::crypto::secret<32>& key, std::string_view sealed,
    std::optional<std::string_view> aad,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    auto packed = from_base64url(sealed, memory);
    if (!packed || packed->size() < gcm_iv_size + gcm_tag_size) {
        return error::decrypt_failed;
    }
    auto cipher = webcpp::crypto::ciphers::aes256_gcm::make(key.bytes());
    if (!cipher) {
        return cipher.error();
    }
    const std::span<std::uint8_t> all(*packed);
    const auto body = all.subspan(gcm_iv_size, all.size() - gcm_iv_size - gcm_tag_size);
    const auto opened = cipher->open(
        all.first(gcm_iv_size), aad ? detail::seal_bytes_of(*aad) : std::span<const std::uint8_t>{},
        body, all.last(gcm_tag_size));
    if (!opened) {
        webcpp::trystero::detail::wipe(packed->data(), packed->size());
        return error::decrypt_failed;
    }
    std::pmr::string out(reinterpret_cast<const char*>(body.data()), body.size(), memory);
    webcpp::trystero::detail::wipe(packed->data(), packed->size());
    if (!json::quotable(out)) {
        webcpp::trystero::detail::wipe(out.data(), out.size());
        return error::decrypt_failed;
    }
    return out;
}

/**
 * The AAD of a signal's sealed field: `type + "|" + peerId + "|" + offerId`.
 *
 * @param type "offer", "answer" or "candidate".
 * @param peer_id The id of the peer that sent the signal.
 * @param offer_id The id of the offer the signal belongs to.
 * @param memory The resource the text is allocated in.
 * @return The AAD.
 */
inline std::pmr::string signal_aad(
    std::string_view type, std::string_view peer_id, std::string_view offer_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(memory);
    out.reserve(type.size() + peer_id.size() + offer_id.size() + 2);
    out += type;
    out += '|';
    out += peer_id;
    out += '|';
    out += offer_id;
    return out;
}

}  // namespace webcpp::trystero::protocol

#endif
