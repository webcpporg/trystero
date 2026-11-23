// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_JSON_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_JSON_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/quote.hpp>
#include <webcpp/document/storage.hpp>
#include <webcpp/document/stringify.hpp>
#include <webcpp/document/traits.hpp>
#include <webcpp/document/value.hpp>

#include <boost/assert.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

/**
 * Reading and writing the protocol's JSON texts on webcpp::document, with every value in the
 * resource the caller gives. A text is read by `webcpp::document::parse` into a
 * `webcpp::document::pmr::document` of that resource, whose image and working state are allocated
 * there; a text the protocol writes is a fixed shape of a few strings and numbers, so it is
 * appended field by field, its strings quoted as `JSON.stringify` quotes them, rather than built as
 * a document, which would copy every string into a node of its own before writing it again.
 */
namespace webcpp::trystero::protocol::json {

/** The traits of every document trystero reads and writes: document's defaults, UTF-8. */
using traits = webcpp::document::default_traits;

/** The read-only handle of a value of a `webcpp::document::pmr::document`. */
using const_value = webcpp::document::basic_const_value<traits, webcpp::document::pmr::owned>;

/** The handle of a value of a `webcpp::document::pmr::document`, which reads and writes. */
using value = webcpp::document::basic_value<traits, webcpp::document::pmr::owned>;

/** The deepest a protocol text nests: Trystero's nest 4 deep at most, a relay's event and its tags.
 */
inline constexpr std::uint64_t max_protocol_depth = 16;

/**
 * The limits of a protocol text's parse: at most `max_bytes` of text, nested at most
 * @ref max_protocol_depth deep, into an image that grows to at most twice that and 64 KiB more.
 *
 * Tip: a text Trystero writes holds a few dozen values at most, each node some 200 bytes of image
 * beside its string, so the image of the longest is under its text and 16 KiB. A text that needs
 * more holds thousands of values, which no Trystero message has; it is refused once its image has
 * grown to the bound, so that no text costs a room more than a multiple of its bound to read. The
 * depth bounds what the parse's recursion takes of the stack, far below document's 256.
 *
 * @param max_bytes The longest text the caller reads.
 * @return The options.
 */
constexpr webcpp::document::parse_options protocol_limits(std::uint64_t max_bytes) noexcept {
    webcpp::document::parse_options o;
    o.max_bytes = max_bytes;
    o.max_depth = max_protocol_depth;
    o.max_document_bytes = (2 * max_bytes) + (std::uint64_t{64} << 10U);
    return o;
}

/**
 * The most image a byte of JSON text takes in a document: a value of one character with its comma
 * or bracket, two bytes, is a node of 192 bytes, and no text takes more (measured on every shape:
 * numbers, literals, empty strings, arrays and objects, nested to 255).
 */
inline constexpr std::uint64_t image_bytes_per_text_byte = 96;

/**
 * The limits of a JSON payload's parse: its own length, nested as deep as document's default,
 * into an image that grows to at most 96 times the text and 64 KiB, which every valid text of that
 * length fits, or `most`, the smaller.
 *
 * @param text_bytes The payload's length.
 * @param most The largest image a payload may take, `room_options::max_json_document_bytes`.
 * @return The options.
 */
constexpr webcpp::document::parse_options payload_limits(std::uint64_t text_bytes,
                                                         std::uint64_t most) noexcept {
    constexpr std::uint64_t spare = std::uint64_t{64} << 10U;
    webcpp::document::parse_options o;
    o.max_bytes = text_bytes;
    o.max_document_bytes = most <= spare || text_bytes >= (most - spare) / image_bytes_per_text_byte
                               ? most
                               : (image_bytes_per_text_byte * text_bytes) + spare;
    return o;
}

/**
 * The text a value holds.
 *
 * @param v The value.
 * @return The text, valid while its document is unchanged; empty when it holds no string.
 */
inline std::string_view text_of(const const_value& v) noexcept {
    return v.value_or(std::string_view());
}

namespace detail {

/** An output of `webcpp::document::quote` that keeps nothing: what checks that a text quotes. */
struct discard {
    /** Takes a piece of the literal, and drops it. */
    void append(const char* /*data*/, std::size_t /*size*/) noexcept {}
};

}  // namespace detail

/**
 * Whether `text` can be written as a JSON string: whether it is UTF-8, as every JavaScript string
 * is once encoded.
 *
 * @param text The text.
 * @return Whether @ref append_string writes it.
 */
inline bool quotable(std::string_view text) {
    detail::discard nothing;
    return webcpp::document::quote(text, nothing).has_value();
}

/**
 * Appends `text` as a JSON string literal, quotes included, as `JSON.stringify` writes it, with
 * webcpp::document's quoting, which allocates only through `out`.
 *
 * @param out The text to append to, unchanged when `text` is refused.
 * @param text The string.
 * @return Nothing, or `errc::type` when `text` is not UTF-8 (@ref quotable), which no JSON string
 * holds: a local candidate whose text is refused is not sent.
 */
[[nodiscard]] inline webcpp::document::result<void> append_string(std::pmr::string& out,
                                                                  std::string_view text) {
    return webcpp::document::quote(text, out);
}

/**
 * Appends `text` as the string `JSON.stringify` writes, whatever its bytes: quoted, `"` and `\\`
 * escaped, `\\b`, `\\f`, `\\n`, `\\r` and `\\t` by their letters, the other controls below 0x20
 * as lowercase `\\u00XX`, and every other byte as it is (DEL and U+2028 too, as `JSON.stringify`
 * writes them).
 *
 * It is the library's one writer of a string that is not refused.
 *
 * Tip: unlike @ref append_string it never refuses a text, so that a topic built of an app id or a
 * room id, which nothing checks to be UTF-8, never silently loses a part; a byte that is not UTF-8
 * is copied as it is. On UTF-8 it writes what @ref append_string writes.
 *
 * @param out The text to append to.
 * @param text The text.
 */
inline void append_string_total(std::pmr::string& out, std::string_view text) {
    static constexpr std::string_view digits = "0123456789abcdef";
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "\\u00";
                    out += digits[static_cast<unsigned char>(c) >> 4U];
                    out += digits[static_cast<unsigned char>(c) & 15U];
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

namespace detail {

/**
 * Appends `text`, UTF-8 by the caller's precondition, as a JSON string: an id, a topic, a key, a
 * nonce or a sealed text, all ASCII, a text read from JSON, or the self id, checked when the app
 * is made.
 */
inline void append_utf8(std::pmr::string& out, std::string_view text) {
    append_string_total(out, text);
}

}  // namespace detail

/**
 * Appends a value as `JSON.stringify` writes it.
 *
 * @tparam Storage The storage of the value's document.
 * @param out The text to append to. The writer hands it pieces of at most 8 bytes, which no
 * standard library's string holds outside its own object, so that only `out` allocates the text.
 * @param v The value; `undefined` writes nothing.
 */
template <class Storage>
// A handle is taken by value, as document passes every handle, and moved on.
void append_value(std::pmr::string& out, webcpp::document::basic_const_value<traits, Storage> v) {
    constexpr std::uint64_t piece = 8;
    // A sink that returns nothing never fails, so neither does the writing.
    static_cast<void>(webcpp::document::stringify_to(
        std::move(v), [&out](std::string_view text) { out.append(text); }, 0, piece));
}

/**
 * Appends an integer in decimal, as JavaScript writes a safe integer.
 *
 * @param out The text to append to.
 * @param value The integer.
 */
inline void append_integer(std::pmr::string& out, std::int64_t value) {
    std::array<char, 24> buffer{};
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.append(buffer.data(), end);
}

/**
 * Appends `"key":` with a leading comma unless it opens the object.
 *
 * @param out The text of an object being written, which ends with its `{` or a member's value.
 * @param key The member's name.
 * @pre `key` is UTF-8, as every name the protocol writes is.
 */
inline void append_key(std::pmr::string& out, std::string_view key) {
    if (out.back() != '{') {
        out += ',';
    }
    detail::append_utf8(out, key);
    out += ':';
}

}  // namespace webcpp::trystero::protocol::json

#endif
