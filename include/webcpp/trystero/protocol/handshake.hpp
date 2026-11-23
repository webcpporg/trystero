// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The messages of Trystero 0.27.0's identity handshake (`handshake.ts`): the DTLS fingerprints of a
 description, the three steps sent as `\@_hsdata` payloads, the reading of a step without its key
 checks, and the transcript both sides sign.
*/
#ifndef WEBCPP_TRYSTERO_PROTOCOL_HANDSHAKE_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_HANDSHAKE_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/handshake_reason.hpp>
#include <webcpp/trystero/protocol/identity.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/document/parse.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace webcpp::trystero::protocol {

/** The shortest nonce a step may carry, in UTF-16 code units: 0.27.0's `n.length < 16` refusal. */
inline constexpr std::size_t min_nonce_units = 16;

namespace detail {

/**
 * Where the line after the one at `at` starts, as a multiline regular expression's `^` sees it:
 * after a line feed, a carriage return, U+2028 or U+2029.
 *
 * @return The position, or `npos` when the line at `at` is the last.
 */
inline std::size_t next_line(std::string_view text, std::size_t at) noexcept {
    for (std::size_t i = at; i < text.size(); ++i) {
        if (text[i] == '\n' || text[i] == '\r') {
            return i + 1;
        }
        auto rest = text;
        rest.remove_prefix(i);
        if (rest.starts_with("\xe2\x80\xa8") || rest.starts_with("\xe2\x80\xa9")) {
            return i + 3;
        }
    }
    return std::string_view::npos;
}

/** Whether two lists join with commas into the same text, 0.27.0's `a.join(',') !== b.join(',')`
 * negated. */
inline bool joined_equal(std::span<const std::pmr::string> a,
                         std::span<const std::pmr::string> b) noexcept {
    // Walks both joins character by character, without building them.
    std::size_t ai = 0;
    std::size_t bi = 0;
    std::size_t ac = 0;
    std::size_t bc = 0;
    const auto next = [](std::span<const std::pmr::string> list, std::size_t& item,
                         std::size_t& at) -> int {
        while (item < list.size()) {
            if (at < list[item].size()) {
                return static_cast<unsigned char>(list[item][at++]);
            }
            ++item;
            at = 0;
            if (item < list.size()) {
                return ',';
            }
        }
        return -1;
    };
    for (;;) {
        const int x = next(a, ai, ac);
        const int y = next(b, bi, bc);
        if (x != y) {
            return false;
        }
        if (x < 0) {
            return true;
        }
    }
}

}  // namespace detail

/**
 * The DTLS fingerprints of a description, 0.27.0's `extractDtlsFingerprints`: the value of each
 * line `a=fingerprint:<value>` (a multiline `^a=fingerprint:([^\\r\\n]+)$`, so a line also starts
 * after U+2028 or U+2029, and the value runs to the line's `\\r` or `\\n`), without the white space
 * JavaScript's `trim` removes, lowercased; the empty ones dropped, the others deduplicated and
 * sorted in UTF-16 order.
 *
 * Tip: the letters are lowercased in ASCII, where JavaScript's `toLowerCase` lowercases every
 * letter: a fingerprint is hexadecimal and a hash name ASCII.
 *
 * @param sdp The description.
 * @param memory The resource the list is allocated in.
 * @return The fingerprints; empty when there is none.
 */
[[nodiscard]] inline std::pmr::vector<std::pmr::string> dtls_fingerprints(
    std::string_view sdp, std::pmr::memory_resource* memory) {
    static constexpr std::string_view prefix = "a=fingerprint:";
    std::pmr::vector<std::pmr::string> out(memory);
    std::size_t at = 0;
    while (at != std::string_view::npos) {
        if (sdp.substr(at).starts_with(prefix)) {
            const std::size_t start = at + prefix.size();
            const std::size_t end = std::min(sdp.find_first_of("\r\n", start), sdp.size());
            if (end > start) {
                std::pmr::string value(detail::js_trim(sdp.substr(start, end - start)), memory);
                std::ranges::transform(value, value.begin(), [](char c) {
                    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                });
                if (!value.empty()) {
                    out.push_back(std::move(value));
                }
                // The match ends at the line's end: a U+2028 inside the value starts no line.
                at = end;
            }
        }
        at = detail::next_line(sdp, at);
    }
    std::ranges::sort(out, utf16_less);
    const auto duplicates = std::ranges::unique(out);
    out.erase(duplicates.begin(), duplicates.end());
    return out;
}

/** The members of a step of the handshake, 0.27.0's claims. */
struct handshake_claims {
    /** The step, `__trystero_id`: 1, 2 or 3. */
    int step = 0;
    /** The sender's nonce, `n`. */
    std::pmr::string nonce;
    /** The sender's user id, `uid`. */
    std::pmr::string user_id;
    /** The sender's endpoint key, compact, `pk`. */
    std::pmr::string endpoint_key;
    /** The sender's user key, compact, `uk`. */
    std::pmr::string user_key;
    /** The endpoint's signature of the transcript, base64url, `ps`: steps 2 and 3. */
    std::pmr::string endpoint_signature;
    /** The user's signature of the transcript, base64url, `us`: steps 2 and 3. */
    std::pmr::string user_signature;
    /** The sender's DTLS fingerprints, `dtls`. */
    std::pmr::vector<std::pmr::string> dtls;
};

/**
 * The JSON of a step, as 0.27.0's `sendClaims` and its step 3 write it: steps 1 and 2 are
 * `{"__trystero_id":<step>,"n":..,"dtls":[..],"uid":..,"pk":..,"uk":..}`, step 2 followed by
 * `"ps":..,"us":..`; step 3 is `{"__trystero_id":3,"ps":..,"us":..}`.
 *
 * @param claims The step's members.
 * @param memory The resource the text is allocated in.
 * @return The text, an `\@_hsdata` payload.
 */
[[nodiscard]] inline std::pmr::string write_handshake_step(const handshake_claims& claims,
                                                           std::pmr::memory_resource* memory) {
    std::pmr::string out(memory);
    out += '{';
    json::append_key(out, handshake_step_key);
    json::append_integer(out, claims.step);
    if (claims.step != 3) {
        json::append_key(out, "n");
        json::append_string_total(out, claims.nonce);
        json::append_key(out, "dtls");
        out += '[';
        for (std::size_t i = 0; i < claims.dtls.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            json::append_string_total(out, claims.dtls[i]);
        }
        out += ']';
        json::append_key(out, "uid");
        json::append_string_total(out, claims.user_id);
        json::append_key(out, "pk");
        json::append_string_total(out, claims.endpoint_key);
        json::append_key(out, "uk");
        json::append_string_total(out, claims.user_key);
    }
    if (claims.step != 1) {
        json::append_key(out, "ps");
        json::append_string_total(out, claims.endpoint_signature);
        json::append_key(out, "us");
        json::append_string_total(out, claims.user_signature);
    }
    out += '}';
    return out;
}

/** What @ref read_handshake_step reads of a step, its members or the reason it is refused. */
struct handshake_read {
    /** The members; nothing when the step is refused. */
    std::optional<handshake_claims> claims;
    /** A @ref handshake_reason when the step is refused; empty otherwise. */
    std::string_view failure;
};

/**
 * Reads a step as 0.27.0's `parseAndVerifyPeerIdentityClaims` reads it, without its key checks
 * (the identity cache's), and step 3 as its responder reads it.
 *
 * Steps 1 and 2: an object whose `__trystero_id` is the number `expected_step`, `n` a string of
 * @ref min_nonce_units UTF-16 code units or more, `dtls` an array (its members that are no strings
 * dropped), `uid` a nonempty string, `pk` and `uk` strings, and at step 2 `ps` and `us` strings;
 * else @ref handshake_reason::identity. Then the DTLS rule: when `expected_remote_dtls` is not
 * empty, the claimed list joined with commas must be it joined with commas, and when `local_dtls`
 * is not empty, the claimed list must not be; else @ref handshake_reason::dtls_mismatch.
 *
 * Step 3: an object whose `__trystero_id` is the number 3, `ps` and `us` strings; else
 * @ref handshake_reason::identity. The lists are not read.
 *
 * Tip: the text is read with the protocol's limits (@ref json::protocol_limits), nested 16 deep at
 * most: a step 0.27.0 writes nests 2 deep.
 *
 * @param json The `\@_hsdata` payload.
 * @param expected_step The step expected: 1, 2 or 3.
 * @param expected_remote_dtls The fingerprints of the remote description this side applied.
 * @param local_dtls The fingerprints of this side's description.
 * @param memory The resource the parse and the members are allocated in.
 * @return The members, or the reason the step is refused.
 */
[[nodiscard]] inline handshake_read read_handshake_step(
    std::string_view json, int expected_step,
    std::span<const std::pmr::string> expected_remote_dtls,
    std::span<const std::pmr::string> local_dtls, std::pmr::memory_resource* memory) {
    const auto refused = [] {
        return handshake_read{.claims = std::nullopt, .failure = handshake_reason::identity};
    };
    const auto parsed = webcpp::document::parse(json, json::protocol_limits(json.size()),
                                                std::pmr::polymorphic_allocator<std::byte>(memory));
    if (!parsed) {
        return refused();
    }
    const json::const_value root = parsed->root();
    const auto step = root[handshake_step_key];
    if (!root.is_object() || !step.is_number() || step.value_or(std::int64_t{0}) != expected_step) {
        return refused();
    }
    const auto ps = root["ps"];
    const auto us = root["us"];
    const bool signed_step = expected_step != 1;
    if (signed_step && (!ps.is_string() || !us.is_string())) {
        return refused();
    }
    handshake_claims claims{
        .step = expected_step,
        .nonce = std::pmr::string(memory),
        .user_id = std::pmr::string(memory),
        .endpoint_key = std::pmr::string(memory),
        .user_key = std::pmr::string(memory),
        .endpoint_signature = std::pmr::string(signed_step ? json::text_of(ps) : "", memory),
        .user_signature = std::pmr::string(signed_step ? json::text_of(us) : "", memory),
        .dtls = std::pmr::vector<std::pmr::string>(memory),
    };
    if (expected_step == 3) {
        return {.claims = std::move(claims), .failure = {}};
    }
    const auto n = root["n"];
    const auto dtls = root["dtls"];
    const auto uid = root["uid"];
    const auto pk = root["pk"];
    const auto uk = root["uk"];
    const auto units = n.length();
    if (!n.is_string() || !units || *units < min_nonce_units || !dtls.is_array() ||
        !uid.is_string() || json::text_of(uid).empty() || !pk.is_string() || !uk.is_string()) {
        return refused();
    }
    claims.nonce = json::text_of(n);
    claims.user_id = json::text_of(uid);
    claims.endpoint_key = json::text_of(pk);
    claims.user_key = json::text_of(uk);
    for (const auto item : dtls) {
        if (item.is_string()) {
            claims.dtls.emplace_back(json::text_of(item));
        }
    }
    if ((!expected_remote_dtls.empty() &&
         !detail::joined_equal(expected_remote_dtls, claims.dtls)) ||
        (!local_dtls.empty() && claims.dtls.empty())) {
        return {.claims = std::nullopt, .failure = handshake_reason::dtls_mismatch};
    }
    return {.claims = std::move(claims), .failure = {}};
}

/**
 * The transcript both sides sign, 0.27.0's `encodeTranscript`: the JSON array
 * `["trystero:id-hs:v2",app,room,initiator peer,responder peer,initiator nonce,responder nonce,
 * [initiator dtls],[responder dtls],initiator user,responder user]`, written as `JSON.stringify`
 * writes it.
 *
 * @param app_id The app's id.
 * @param room_id The room's id.
 * @param initiator_peer The initiator's peer id.
 * @param responder_peer The responder's peer id.
 * @param initiator_nonce The initiator's nonce.
 * @param responder_nonce The responder's nonce.
 * @param initiator_dtls The initiator's fingerprints.
 * @param responder_dtls The responder's fingerprints.
 * @param initiator_user The initiator's user id.
 * @param responder_user The responder's user id.
 * @param memory The resource the text is allocated in.
 * @return The transcript, whose UTF-8 bytes are signed.
 */
[[nodiscard]] inline std::pmr::string handshake_transcript(
    std::string_view app_id, std::string_view room_id, std::string_view initiator_peer,
    std::string_view responder_peer, std::string_view initiator_nonce,
    std::string_view responder_nonce, std::span<const std::pmr::string> initiator_dtls,
    std::span<const std::pmr::string> responder_dtls, std::string_view initiator_user,
    std::string_view responder_user, std::pmr::memory_resource* memory) {
    std::pmr::string out(memory);
    const auto list = [&out](std::span<const std::pmr::string> items) {
        out += ",[";
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            json::append_string_total(out, items[i]);
        }
        out += ']';
    };
    out += '[';
    json::append_string_total(out, domain_tag("id-hs", 2, memory));
    for (const auto part :
         {app_id, room_id, initiator_peer, responder_peer, initiator_nonce, responder_nonce}) {
        out += ',';
        json::append_string_total(out, part);
    }
    list(initiator_dtls);
    list(responder_dtls);
    for (const auto part : {initiator_user, responder_user}) {
        out += ',';
        json::append_string_total(out, part);
    }
    out += ']';
    return out;
}

}  // namespace webcpp::trystero::protocol

#endif
