// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_SIGNAL_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_SIGNAL_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/seal.hpp>
#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <cstddef>
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

/** The longest offer id a signal may carry, in bytes; Trystero's are @ref offer_id_size. */
inline constexpr std::size_t max_offer_id_bytes = 64;

/**
 * The longest sealed offer or answer a signal may carry, in base64url characters.
 *
 * A data channel's SDP is a few KiB, with every candidate gathered in it a few more. Trystero
 * bounds none; this one bounds what a negotiation copies and keeps, and refuses no SDP a browser
 * writes for a data channel.
 */
inline constexpr std::size_t max_sealed_description_bytes = std::size_t{64} * 1024;

/**
 * The longest sealed candidate a signal may carry, in base64url characters.
 *
 * Trystero drops a candidate whose plaintext is longer than 8192 bytes
 * (`maxRemoteCandidateSdpLength`), and every candidate up to that size, sealed by @ref seal, fits.
 */
inline constexpr std::size_t max_sealed_candidate_bytes = std::size_t{16} * 1024;
static_assert(sealed_size(8192) <= max_sealed_candidate_bytes);

/**
 * The longest signal a room reads, in bytes: a longer one is dropped before it is read.
 *
 * The longest signal measured is an answer, 762 bytes, among Trystero JS in Chrome and both
 * backends, which trickle their candidates. The bound is the longest signal the bounds of its
 * fields admit, a sealed offer or answer of @ref max_sealed_description_bytes with the longest ids,
 * and 1 KiB to spare, so that it refuses no signal they accept.
 */
inline constexpr std::size_t max_parsed_signal_bytes = max_sealed_description_bytes + 1024;
// The 64 bytes hold the 53 of the keys and the punctuation of an answer from a passive room.
static_assert(max_sealed_description_bytes + max_peer_id_bytes + max_offer_id_bytes + 64 <=
                  max_parsed_signal_bytes,
              "the longest signal the bounds of its fields admit is read");

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
     * Whether the sender answers the offer, which places `offerId` in the text.
     *
     * Trystero's answerer writes `offerId` after its answer or candidate, its offerer before its
     * offer or candidate. Written only; a reader leaves it false, the order being no part of the
     * meaning.
     */
    bool answering = false;

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
 * The message a parsed signaling payload holds, as views into its document.
 *
 * A key written twice is read as JavaScript reads it, the last value, which the parse keeps.
 *
 * @param document The root of the parsed payload, whose document the caller keeps alive and
 * unchanged while it reads the views.
 * @return The message, or nothing when Trystero would ignore it (not an object, one with a member
 * `peer`, no peer id, or an offer, answer or candidate that is not a non-empty string), or when a
 * field is longer than its bound: the peer id than @ref max_peer_id_bytes, the offer id than
 * @ref max_offer_id_bytes, an offer or answer than @ref max_sealed_description_bytes, a candidate
 * than @ref max_sealed_candidate_bytes.
 */
inline std::optional<signal_message> read_signal(const json::const_value& document) {
    // Trystero ignores a text that names a member `peer`, which its own rooms keep for a peer
    // object no text can hold.
    if (!document.is_object() || document.contains("peer")) {
        return std::nullopt;
    }
    signal_message out;
    const auto text = [&document](std::string_view key, std::string_view& into,
                                  bool must_be_nonempty) {
        const json::const_value v = document[key];
        if (v.is_undefined()) {
            return true;
        }
        if (!v.is_string() || (must_be_nonempty && json::text_of(v).empty())) {
            return !must_be_nonempty;
        }
        into = json::text_of(v);
        return true;
    };
    if (!text("offer", out.offer, true) || !text("answer", out.answer, true) ||
        !text("candidate", out.candidate, true)) {
        return std::nullopt;
    }
    text("peerId", out.peer_id, false);
    text("offerId", out.offer_id, false);
    out.passive = document["passive"].value_or(false);
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
 * @pre Every string is UTF-8, as every one a room writes is: the ids, the nonce and the sealed
 * texts are ASCII, an answered offer's id was read from JSON, and the self id is checked when the
 * app is made.
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
        json::detail::append_utf8(out, value);
    };
    field("peerId", m.peer_id);
    if (!m.answering) {
        field("offerId", m.offer_id);
    }
    field("offer", m.offer);
    field("answer", m.answer);
    field("candidate", m.candidate);
    if (m.answering) {
        field("offerId", m.offer_id);
    }
    if (m.passive) {
        json::append_key(out, "passive");
        out += "true";
    }
    field("nonce", nonce);
    out += '}';
    return out;
}

}  // namespace webcpp::trystero::protocol

#endif
