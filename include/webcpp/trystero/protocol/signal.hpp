// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_SIGNAL_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_SIGNAL_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <boost/json/value.hpp>

#include <array>
#include <charconv>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace webcpp::trystero::protocol {

/**
 * The longest peer id a room accepts in a signal, in bytes; a signal with a longer one is
 * ignored.
 *
 * Every id Trystero makes is 20 characters (`selfId`, @ref peer_id_size); the bound leaves room for
 * an application's own ids and keeps a flood of long ids from costing more than a short one.
 */
inline constexpr std::size_t max_peer_id_bytes = 64;

/** The size of the IV Trystero's `encrypt` draws for AES-GCM: 16 bytes, not the usual 12. */
inline constexpr std::size_t seal_iv_size = 16;

/**
 * The longest text a plaintext seals to, as @ref format_sealed writes it.
 *
 * The IV's bytes in decimal and commas, "$", and the ciphertext with its 16-byte tag in base64.
 *
 * @param plaintext The plaintext's size, in bytes.
 * @return The size of the longest sealed text, in bytes, whatever the IV.
 */
constexpr std::size_t sealed_size(std::size_t plaintext) {
    return (seal_iv_size * 4) - 1 + 1 + (((plaintext + 16 + 2) / 3) * 4);
}

/** The longest offer id a signal may carry, in bytes; Trystero's are @ref offer_id_size. */
inline constexpr std::size_t max_offer_id_bytes = 64;

/**
 * The longest sealed offer or answer a signal may carry, in bytes.
 *
 * A data channel's SDP is a few KiB, with every candidate gathered in it a few more. Trystero
 * bounds none; this one bounds what a negotiation copies and keeps, and refuses no SDP a browser
 * writes for a data channel.
 */
inline constexpr std::size_t max_sealed_description_bytes = std::size_t{64} * 1024;

/**
 * The longest sealed candidate a signal may carry, in bytes.
 *
 * Trystero drops a candidate whose plaintext is longer than 8192 bytes
 * (`maxRemoteCandidateSdpLength`), and every candidate up to that size, sealed, fits.
 */
inline constexpr std::size_t max_sealed_candidate_bytes = std::size_t{16} * 1024;
static_assert(sealed_size(8192) <= max_sealed_candidate_bytes);

/**
 * The raw AES-GCM key of a room, what Trystero's `genKey` imports: the SHA-256 of
 * "<password>:<app>:<room>".
 *
 * @param password The room's password, empty when there is none.
 * @param app_id The application's id.
 * @param room_id The room's id.
 * @return The AES-256 key.
 */
inline std::array<std::uint8_t, 32> seal_key(std::string_view password, std::string_view app_id,
                                             std::string_view room_id) {
    return sha256_of({password, ":", app_id, ":", room_id});
}

/** A sealed signal's parts, as @ref parse_sealed reads them. */
struct sealed_parts {
    /** The IV, 16 bytes in what Trystero writes. */
    bytes iv;
    /** The ciphertext, followed by its 16-byte tag. */
    bytes ciphertext;
};

/**
 * The text form of a sealed signal, what Trystero's `encrypt` returns: the IV's bytes in decimal
 * joined by commas, "$", the ciphertext in base64.
 *
 * @param iv The IV.
 * @param ciphertext The ciphertext with its tag, as `crypto.subtle.encrypt` returns it.
 * @param memory The resource the text is allocated in.
 * @return The text.
 */
inline std::pmr::string format_sealed(
    std::span<const std::uint8_t> iv, std::span<const std::uint8_t> ciphertext,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(memory);
    out.reserve((iv.size() * 4) + 1 + ((ciphertext.size() + 2) / 3 * 4));
    for (std::size_t i = 0; i < iv.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        json::append_integer(out, iv[i]);
    }
    out += '$';
    append_base64(out, ciphertext);
    return out;
}

/**
 * The parts of a sealed signal's text, as Trystero's `decrypt` splits it.
 *
 * @param text The text.
 * @param memory The resource the parts are allocated in.
 * @return The parts, or nothing when the text has no "$", an IV byte that is not a decimal number
 * up to 255, or a ciphertext that is not base64.
 */
inline std::optional<sealed_parts> parse_sealed(
    std::string_view text, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    const auto dollar = text.find('$');
    if (dollar == std::string_view::npos) {
        return std::nullopt;
    }
    sealed_parts out{.iv = bytes(memory), .ciphertext = bytes(memory)};
    for (std::string_view ivs = text.substr(0, dollar); !ivs.empty();) {
        const auto comma = ivs.find(',');
        const auto item = ivs.substr(0, comma);
        unsigned value = 0;
        const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), value);
        if (ec != std::errc{} || end != item.data() + item.size() || value > 255) {
            return std::nullopt;
        }
        out.iv.push_back(static_cast<std::uint8_t>(value));
        ivs = comma == std::string_view::npos ? std::string_view{} : ivs.substr(comma + 1);
    }
    auto ciphertext = base64_decode(text.substr(dollar + 1), memory);
    if (!ciphertext) {
        return std::nullopt;
    }
    out.ciphertext = std::move(*ciphertext);
    return out;
}

/**
 * A message on a signaling topic, the JSON object Trystero's signal handler reads, as views.
 *
 * With none of offer, answer and candidate it is an announcement: the sender is in the room. Offer,
 * answer and candidate hold sealed text; a candidate's plaintext is the JSON of an
 * `RTCIceCandidateInit`.
 *
 * @see "The protocol", in the guide.
 */
struct signal_message {
    /** The sender's id, `peerId`: 1 to @ref max_peer_id_bytes bytes. */
    std::string_view peer_id = {};
    /** The offer the message belongs to, `offerId`; empty in an announcement. */
    std::string_view offer_id = {};
    /** The sealed SDP of an offer, `offer`. */
    std::string_view offer = {};
    /** The sealed SDP of an answer, `answer`. */
    std::string_view answer = {};
    /** A sealed ICE candidate, `candidate`. */
    std::string_view candidate = {};
    /**
     * Whether the sender's room is passive, `passive`: a Trystero room joined with `passive`
     * announces only once it has seen a peer that is not passive.
     */
    bool passive = false;

    /**
     * Whether the message is an announcement.
     *
     * @return Whether it holds none of offer, answer and candidate.
     */
    [[nodiscard]] bool is_announcement() const {
        return offer.empty() && answer.empty() && candidate.empty();
    }
};

/**
 * The message a parsed signaling payload holds, as views into `document`.
 *
 * @param document The parsed payload, which the caller keeps alive while it reads the views.
 * @return The message, or nothing when Trystero would ignore it (not an object, no peer id, or an
 * offer, answer or candidate that is not a non-empty string), or when a field is longer than its
 * bound: the peer id than @ref max_peer_id_bytes, the offer id than @ref max_offer_id_bytes, an
 * offer or answer than @ref max_sealed_description_bytes, a candidate than
 * @ref max_sealed_candidate_bytes.
 */
inline std::optional<signal_message> read_signal(const boost::json::value& document) {
    const auto* object = document.if_object();
    if (object == nullptr) {
        return std::nullopt;
    }
    signal_message out;
    const auto text = [object](std::string_view key, std::string_view& into,
                               bool must_be_nonempty) {
        const auto* v = object->if_contains(key);
        if (v == nullptr) {
            return true;
        }
        const auto* s = v->if_string();
        if (s == nullptr || (must_be_nonempty && s->empty())) {
            return !must_be_nonempty;
        }
        into = std::string_view(s->data(), s->size());
        return true;
    };
    if (!text("offer", out.offer, true) || !text("answer", out.answer, true) ||
        !text("candidate", out.candidate, true)) {
        return std::nullopt;
    }
    text("peerId", out.peer_id, false);
    text("offerId", out.offer_id, false);
    if (const auto* v = object->if_contains("passive"); v != nullptr && v->is_bool()) {
        out.passive = v->get_bool();
    }
    if (out.peer_id.empty() || out.peer_id.size() > max_peer_id_bytes ||
        out.offer_id.size() > max_offer_id_bytes ||
        out.offer.size() > max_sealed_description_bytes ||
        out.answer.size() > max_sealed_description_bytes ||
        out.candidate.size() > max_sealed_candidate_bytes) {
        return std::nullopt;
    }
    return out;
}

/**
 * The JSON text of a message, with its members in Trystero's order and the empty ones left out.
 *
 * @param m The message.
 * @param nonce A random text, which makes repeated announcements distinct events; empty for none.
 * @param memory The resource the text is allocated in.
 * @return The text.
 */
inline std::pmr::string write_signal(
    const signal_message& m, std::string_view nonce = {},
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(memory);
    out.reserve(64 + m.peer_id.size() + m.offer_id.size() + m.offer.size() + m.answer.size() +
                m.candidate.size() + nonce.size());
    out += '{';
    const auto field = [&out](std::string_view key, std::string_view value) {
        if (value.empty()) {
            return;
        }
        json::append_key(out, key);
        json::append_string(out, value);
    };
    field("peerId", m.peer_id);
    field("offerId", m.offer_id);
    field("offer", m.offer);
    field("answer", m.answer);
    field("candidate", m.candidate);
    if (m.passive) {
        json::append_key(out, "passive");
        out += "true";
    }
    field("nonce", nonce);
    out += '}';
    return out;
}

/**
 * The password handshake's proof, Trystero's `hashChallenge`: the hexadecimal SHA-256 of
 * "<challenge>:<password>:<app>:<room>".
 *
 * @param challenge The challenge the other peer sent.
 * @param password The room's password.
 * @param app_id The application's id.
 * @param room_id The room's id.
 * @param memory The resource the proof is allocated in.
 * @return The proof, 64 hexadecimal digits.
 */
inline std::pmr::string handshake_proof(
    std::string_view challenge, std::string_view password, std::string_view app_id,
    std::string_view room_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return to_hex(sha256_of({challenge, ":", password, ":", app_id, ":", room_id}), memory);
}

}  // namespace webcpp::trystero::protocol

#endif
