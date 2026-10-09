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

/** AES-GCM IVs in Trystero are 16 bytes, not the usual 12. */
inline constexpr std::size_t seal_iv_size = 16;

/** The raw AES-GCM key of a room: SHA-256 of "<password>:<app>:<room>", the password empty when
 * there is none. */
inline std::array<std::uint8_t, 32> seal_key(std::string_view password, std::string_view app_id,
                                             std::string_view room_id) {
    return sha256_of({password, ":", app_id, ":", room_id});
}

/** A sealed signal's parts: the IV and the ciphertext with its 16-byte tag. */
struct sealed_parts {
    bytes iv;
    bytes ciphertext;
};

/** The text form of a sealed signal: the IV's bytes in decimal joined by commas, "$", the
 * ciphertext in base64. */
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
 * A message on a signaling topic, as views. With none of offer, answer and candidate it is an
 * announcement: the sender is in the room. Offer, answer and candidate hold sealed text; a
 * candidate's plaintext is the JSON of an `RTCIceCandidateInit`.
 */
struct signal_message {
    std::string_view peer_id = {};
    std::string_view offer_id = {};
    std::string_view offer = {};
    std::string_view answer = {};
    std::string_view candidate = {};
    bool passive = false;

    [[nodiscard]] bool is_announcement() const {
        return offer.empty() && answer.empty() && candidate.empty();
    }
};

/**
 * The message a parsed signaling payload holds, as views into `document`, or nothing when Trystero
 * would ignore it: not an object, no peer id, or an offer, answer or candidate that is not a
 * non-empty string.
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
    if (out.peer_id.empty()) {
        return std::nullopt;
    }
    return out;
}

/** The JSON text of a message; the optional `nonce` makes repeated announcements distinct events.
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

/** The password handshake's proof: hex SHA-256 of "<challenge>:<password>:<app>:<room>". */
inline std::pmr::string handshake_proof(
    std::string_view challenge, std::string_view password, std::string_view app_id,
    std::string_view room_id,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return to_hex(sha256_of({challenge, ":", password, ":", app_id, ":", room_id}), memory);
}

}  // namespace webcpp::trystero::protocol

#endif
