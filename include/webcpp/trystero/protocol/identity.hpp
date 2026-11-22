// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The texts of Trystero 0.27.0's user identity: the user key a program keeps, `trystero_uk_` and the
 base64url of its JSON, or the JSON itself; and the payload of a user's proof that an endpoint is
 its own, which 0.27.0 defines but its core never sends.
*/
#ifndef WEBCPP_TRYSTERO_PROTOCOL_IDENTITY_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_IDENTITY_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/detail/wipe.hpp>
#include <webcpp/trystero/protocol/ecdsa.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/crypto/secret.hpp>
#include <webcpp/document/parse.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace webcpp::trystero::protocol {

/** The prefix of an exported user key, 0.27.0's `userKeyPrefix`: `libId` and `_uk_`. */
inline constexpr std::string_view user_key_prefix = "trystero_uk_";

/** A user's P-256 key as its text gives it: the secret scalar and the public point. */
struct user_key_material {
    /** The secret scalar d, big-endian, in [1, n - 1]. */
    webcpp::crypto::secret<32> d;
    /** The public point, SEC 1's uncompressed `04 || x || y`; not checked to be on the curve, nor
     * to be d's. */
    std::array<std::uint8_t, 65> point{};
};

namespace detail {

/**
 * The length of the white space of JavaScript's `String.prototype.trim` that starts `text`, in
 * UTF-8: tab, line feed, vertical tab, form feed, carriage return, space, U+00A0, U+1680, U+2000
 * to U+200A, U+2028, U+2029, U+202F, U+205F, U+3000 and U+FEFF.
 *
 * @param text The text.
 * @param at_end Whether the white space ends `text` rather than starts it.
 * @return The length of one such character, 0 when there is none.
 */
inline std::size_t js_space(std::string_view text, bool at_end) noexcept {
    const auto piece = [&](std::size_t n) {
        return text.size() < n ? std::string_view()
               : at_end        ? text.substr(text.size() - n)
                               : text.substr(0, n);
    };
    if (const auto one = piece(1); !one.empty()) {
        switch (one[0]) {
            case '\t':
            case '\n':
            case '\v':
            case '\f':
            case '\r':
            case ' ': return 1;
            default: break;
        }
    }
    if (piece(2) == "\xc2\xa0") {
        return 2;
    }
    const auto three = piece(3);
    if (three.size() != 3) {
        return 0;
    }
    const auto b1 = static_cast<unsigned char>(three[1]);
    const auto b2 = static_cast<unsigned char>(three[2]);
    const bool space = three == "\xe1\x9a\x80" || three == "\xe3\x80\x80" ||
                       three == "\xef\xbb\xbf" || three == "\xe2\x81\x9f" ||
                       (three[0] == '\xe2' && b1 == 0x80 &&
                        ((b2 >= 0x80 && b2 <= 0x8a) || b2 == 0xa8 || b2 == 0xa9 || b2 == 0xaf));
    return space ? 3 : 0;
}

/**
 * `text` without the white space of JavaScript's `String.prototype.trim` around it.
 *
 * @param text The text, UTF-8.
 * @return The view of what remains.
 */
inline std::string_view js_trim(std::string_view text) noexcept {
    while (const auto n = js_space(text, false)) {
        text.remove_prefix(n);
    }
    while (const auto n = js_space(text, true)) {
        text.remove_suffix(n);
    }
    return text;
}

/**
 * Whether a JSON member that may be absent is absent or false to JavaScript's `&&`: undefined,
 * null, false, 0 or the empty string.
 *
 * @param v The member, undefined when absent.
 * @return Whether JavaScript reads it as false.
 */
inline bool js_falsy(const json::const_value& v) {
    if (v.is_undefined() || v.is_null()) {
        return true;
    }
    if (v.is_boolean()) {
        return !v.value_or(true);
    }
    if (v.is_number()) {
        // Only a zero reads as the integer 0; a number that is no integer reads as the fallback.
        return v.value_or(std::int64_t{1}) == 0;
    }
    if (v.is_string()) {
        return json::text_of(v).empty();
    }
    return false;
}

}  // namespace detail

/**
 * Reads a user key as 0.27.0's `tryParsePrivateJwk` reads a text: the text without the white space
 * `String.prototype.trim` removes; when it starts with @ref user_key_prefix, the rest is strict
 * base64url of the JSON, read as `TextDecoder` reads it (a leading byte order mark dropped); when
 * it starts with `{`, it is the JSON. The JSON is an object whose `d`, `x` and `y` are nonempty
 * strings, `d` the strict base64url of 32 bytes that are a valid scalar (@ref is_p256_scalar),
 * `x` and `y` of 32 bytes each; its `kty`, unless absent or falsy, is `"EC"`, and its `crv`
 * `"P-256"`.
 *
 * Every buffer that held `d`, its text, the decoded JSON, the parse's document and the scalar's
 * bytes, is wiped before the call returns.
 *
 * Tip: 0.27.0's `fromBase64Url` is loose where this reading is strict (design D18): a padded
 * user key, which 0.27.0 never writes, is refused here.
 *
 * @param text The key's text, as a program kept it.
 * @param memory The resource the intermediate buffers are allocated in.
 * @return The key's scalar and point, or nothing when the text is not a user key.
 */
[[nodiscard]] inline std::optional<user_key_material> parse_user_key(
    std::string_view text, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    // First, so that it ends last: every buffer below is wiped when it is freed.
    webcpp::trystero::detail::wiping_resource wiping(memory);
    const auto trimmed = detail::js_trim(text);
    std::optional<bytes> decoded;
    std::string_view json_text;
    if (trimmed.starts_with(user_key_prefix)) {
        decoded = from_base64url(trimmed.substr(user_key_prefix.size()), &wiping);
        if (!decoded) {
            return std::nullopt;
        }
        json_text =
            std::string_view(reinterpret_cast<const char*>(decoded->data()), decoded->size());
        if (json_text.starts_with("\xef\xbb\xbf")) {
            json_text.remove_prefix(3);  // TextDecoder drops a byte order mark
        }
    } else if (trimmed.starts_with('{')) {
        json_text = trimmed;
    } else {
        return std::nullopt;
    }
    const auto parsed =
        webcpp::document::parse(json_text, webcpp::document::parse_options{},
                                std::pmr::polymorphic_allocator<std::byte>(&wiping));
    if (!parsed) {
        return std::nullopt;
    }
    const json::const_value root = parsed->root();
    const auto d = root["d"];
    const auto x = root["x"];
    const auto y = root["y"];
    if (!root.is_object() || !d.is_string() || !x.is_string() || !y.is_string()) {
        return std::nullopt;
    }
    const auto kty = root["kty"];
    const auto crv = root["crv"];
    if ((!detail::js_falsy(kty) && !(kty.is_string() && json::text_of(kty) == "EC")) ||
        (!detail::js_falsy(crv) && !(crv.is_string() && json::text_of(crv) == "P-256"))) {
        return std::nullopt;
    }
    // Empty texts are refused here too: strict base64url has no empty text.
    const auto scalar = from_base64url(json::text_of(d), &wiping);
    const auto x_bytes = from_base64url(json::text_of(x), &wiping);
    const auto y_bytes = from_base64url(json::text_of(y), &wiping);
    if (!scalar || scalar->size() != 32 || !x_bytes || x_bytes->size() != 32 || !y_bytes ||
        y_bytes->size() != 32) {
        return std::nullopt;
    }
    if (!is_p256_scalar(std::span<const std::uint8_t, 32>(scalar->data(), 32))) {
        return std::nullopt;
    }
    user_key_material out;
    std::ranges::copy(*scalar, out.d.writable().begin());
    out.point[0] = 0x04;
    std::ranges::copy(*x_bytes, out.point.begin() + 1);
    std::ranges::copy(*y_bytes, out.point.begin() + 33);
    return out;
}

/**
 * The text of a user key, as 0.27.0's `buildExtractableUserIdentity` writes it: @ref
 * user_key_prefix and the base64url of the JSON `{"d":..,"x":..,"y":..}`, each member the
 * base64url of its 32 bytes.
 *
 * The JSON, which holds the scalar, is wiped before the call returns; the text returned holds it
 * too, and is the caller's to keep or wipe.
 *
 * @param key The key.
 * @param memory The resource the text is allocated in.
 * @return The text, 214 characters.
 */
[[nodiscard]] inline std::pmr::string write_user_key(const user_key_material& key,
                                                     std::pmr::memory_resource* memory) {
    // {"d":"<43>","x":"<43>","y":"<43>"}: 151 bytes, whose base64url is 202 characters. Each
    // buffer is reserved whole, so that no copy of the scalar is left in a block freed by growth.
    constexpr std::size_t json_size = 151;
    constexpr std::size_t text_size = 214;
    webcpp::trystero::detail::wiping_resource wiping(memory);
    std::pmr::string json_text(&wiping);
    json_text.reserve(json_size);
    json_text += R"({"d":")";
    append_base64url(json_text, key.d.bytes());
    json_text += R"(","x":")";
    append_base64url(json_text, std::span(key.point).subspan(1, 32));
    json_text += R"(","y":")";
    append_base64url(json_text, std::span(key.point).subspan(33, 32));
    json_text += R"("})";
    std::pmr::string out(memory);
    out.reserve(text_size);
    out += user_key_prefix;
    append_base64url(
        out, std::span(reinterpret_cast<const std::uint8_t*>(json_text.data()), json_text.size()));
    return out;
}

/**
 * The payload a user signs to prove an endpoint its own, 0.27.0's `userEndpointProofPayload`: the
 * JSON `["trystero:user-proof:v3",app,room,peer,compact,seq]`, its strings written as
 * `JSON.stringify` writes them.
 *
 * Tip: 0.27.0's core neither sends nor checks a user-proof (design D21): the handshake's two
 * signatures bind the user to the endpoint. The payload is kept, with @ref sign_p256 and
 * @ref verify_p256, as 0.27.0 defines it.
 *
 * @param app_id The app's id.
 * @param room_id The room's id.
 * @param peer_id The endpoint's peer id.
 * @param peer_compact The endpoint's compact public key.
 * @param seq The proof's sequence number, at most `Number.MAX_SAFE_INTEGER`.
 * @param memory The resource the payload is allocated in.
 * @return The payload, UTF-8.
 */
[[nodiscard]] inline std::pmr::string user_proof_payload(
    std::string_view app_id, std::string_view room_id, std::string_view peer_id,
    std::string_view peer_compact, std::uint64_t seq, std::pmr::memory_resource* memory) {
    std::pmr::string out(memory);
    out += '[';
    json::append_string_total(out, domain_tag("user-proof", 3, memory));
    for (const auto part : {app_id, room_id, peer_id, peer_compact}) {
        out += ',';
        json::append_string_total(out, part);
    }
    out += ',';
    json::append_integer(out, static_cast<std::int64_t>(seq));
    out += ']';
    return out;
}

}  // namespace webcpp::trystero::protocol

#endif
