// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_SIGNAL_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_SIGNAL_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/ids.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/seal.hpp>
#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace webcpp::trystero::protocol {

/**
 * The longest offer id a signal may carry, in characters: 0.27.0's `maxSignalOfferIdChars`.
 *
 * Trystero's own are @ref offer_id_size; @ref is_signal_offer_id is the whole rule.
 */
inline constexpr std::size_t max_offer_id_chars = 64;

/**
 * The longest sealed offer or answer a signal may carry, in UTF-16 code units as JavaScript counts
 * a string's length: 0.27.0's `maxSignalSdpChars`. A sealed text is ASCII, one byte a unit.
 */
inline constexpr std::size_t max_sealed_description_chars = std::size_t{64} * 1024;

/**
 * The longest sealed candidate a signal may carry, in UTF-16 code units as JavaScript counts a
 * string's length: 0.27.0's `maxSignalCandidateChars`. A sealed text is ASCII, one byte a unit.
 *
 * It holds a candidate's JSON of up to 3044 bytes, sealed by @ref seal; 0.27.0 sends a longer one
 * as it is, and its receivers, as a room, ignore the signal.
 */
inline constexpr std::size_t max_sealed_candidate_chars = 4096;
static_assert(sealed_size(3044) == max_sealed_candidate_chars);
static_assert(sealed_size(3045) > max_sealed_candidate_chars);

/**
 * The longest signal a room reads, in bytes: a longer one is dropped before it is read.
 *
 * The longest signal measured is an answer, 762 bytes, among Trystero JS in Chrome and both
 * backends, which trickle their candidates. The bound is the longest signal the bounds of its
 * fields admit, a sealed offer or answer of @ref max_sealed_description_chars with the longest ids,
 * and 1 KiB to spare, so that it refuses no signal Trystero writes.
 *
 * Tip: 0.27.0 parses a text of up to 128 Ki characters (`maxSignalMessageChars`). A room drops a
 * longer text than this bound unread, where 0.27.0 parses it; only members Trystero never writes,
 * or a sealed text that is not ASCII and so never opens, make one. The smaller bound bounds what a
 * relay has a room parse.
 */
inline constexpr std::size_t max_parsed_signal_bytes = max_sealed_description_chars + 1024;
// The 64 bytes hold the keys and the punctuation of an offer or an answer, 38 at most.
static_assert(max_sealed_description_chars + peer_id_size + max_offer_id_chars + 64 <=
                  max_parsed_signal_bytes,
              "the longest signal the bounds of its fields admit is read");

/**
 * Whether a text is a peer id a signal may carry, 0.27.0's `isValidSignalPeerId`: @ref peer_id_size
 * characters of @ref base62_alphabet (`^[0-9A-Za-z]{20}$`).
 *
 * @param id The text.
 * @return Whether it is one.
 */
[[nodiscard]] constexpr bool is_signal_peer_id(std::string_view id) noexcept {
    return id.size() == peer_id_size && std::ranges::all_of(id, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
           });
}

/**
 * Whether a text is an offer id a signal may carry, 0.27.0's `isValidSignalOfferId`: 1 to
 * @ref max_offer_id_chars characters, each a letter or a digit of ASCII or one of `_.~:!-`
 * (`^[0-9A-Za-z_.~:!-]{1,64}$`).
 *
 * @param id The text.
 * @return Whether it is one.
 */
[[nodiscard]] constexpr bool is_signal_offer_id(std::string_view id) noexcept {
    return !id.empty() && id.size() <= max_offer_id_chars && std::ranges::all_of(id, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               std::string_view("_.~:!-").find(c) != std::string_view::npos;
    });
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
    /** The sender's id, `peerId`, one for which @ref is_signal_peer_id holds. */
    std::string_view peer_id = {};
    /**
     * The offer the message belongs to, `offerId`, one for which @ref is_signal_offer_id holds;
     * empty in an announcement that carries none.
     */
    std::string_view offer_id = {};
    /** The sealed SDP of an offer, `offer`. */
    std::string_view offer = {};
    /** The sealed SDP of an answer, `answer`. */
    std::string_view answer = {};
    /** A sealed ICE candidate, `candidate`. */
    std::string_view candidate = {};

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
 * The message a parsed signaling payload holds, as views into its document, read as 0.27.0's
 * `parseSignalEnvelope` reads it: what it refuses, a room ignores.
 *
 * A key written twice is read as JavaScript reads it, the last value, which the parse keeps.
 *
 * @param document The root of the parsed payload, whose document the caller keeps alive and
 * unchanged while it reads the views.
 * @return The message, or nothing when 0.27.0 refuses it or acts on no peer: not an object; one
 * with a member `peer`; no `peerId`, or one for which @ref is_signal_peer_id does not hold; an
 * `offerId` for which @ref is_signal_offer_id does not hold (a value that is no string included),
 * or none with an offer, answer or candidate; an offer, answer or candidate that is not a non-empty
 * string, an offer or answer longer than @ref max_sealed_description_chars, a candidate longer than
 * @ref max_sealed_candidate_chars.
 */
inline std::optional<signal_message> read_signal(const json::const_value& document) {
    // Trystero ignores a text that names a member `peer`, which its own rooms keep for a peer
    // object no text can hold.
    if (!document.is_object() || document.contains("peer")) {
        return std::nullopt;
    }
    signal_message out;
    bool content = false;
    // An offer, answer or candidate, when present, is a non-empty string within its bound, counted
    // in UTF-16 code units as JavaScript's length counts it.
    const auto sealed = [&document, &content](std::string_view key, std::string_view& into,
                                              std::size_t bound) {
        const json::const_value v = document[key];
        if (v.is_undefined()) {
            return true;
        }
        content = true;
        const auto units = v.length();
        if (!v.is_string() || !units || *units == 0 || *units > bound) {
            return false;
        }
        into = json::text_of(v);
        return true;
    };
    if (!sealed("offer", out.offer, max_sealed_description_chars) ||
        !sealed("answer", out.answer, max_sealed_description_chars) ||
        !sealed("candidate", out.candidate, max_sealed_candidate_chars)) {
        return std::nullopt;
    }
    // A peer id that is not one refuses the signal, and so does none, on which 0.27.0 acts no
    // further. An offer id, when present, or when the signal carries an offer, answer or
    // candidate, is one: JSON holds no `undefined`, so a member `offerId` of any other value
    // refuses.
    const json::const_value peer_id = document["peerId"];
    const json::const_value offer_id = document["offerId"];
    if (!peer_id.is_string() || !is_signal_peer_id(json::text_of(peer_id))) {
        return std::nullopt;
    }
    out.peer_id = json::text_of(peer_id);
    if (content || !offer_id.is_undefined()) {
        if (!offer_id.is_string() || !is_signal_offer_id(json::text_of(offer_id))) {
            return std::nullopt;
        }
        out.offer_id = json::text_of(offer_id);
    }
    return out;
}

/**
 * The JSON text of a message, as 0.27.0's signal handler writes it: `peerId`, then `offerId`, then
 * the one of `offer`, `answer` and `candidate` it carries, the empty ones left out, and `nonce`
 * last, as the Nostr strategy adds it to an announcement.
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
    field("offerId", m.offer_id);
    field("offer", m.offer);
    field("answer", m.answer);
    field("candidate", m.candidate);
    field("nonce", nonce);
    out += '}';
    return out;
}

}  // namespace webcpp::trystero::protocol

#endif
