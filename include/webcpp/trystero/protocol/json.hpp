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

/**
 * The limits of a JSON text's parse: at most `max_bytes` of text, into an image that grows to at
 * most twice that and 64 KiB more.
 *
 * Tip: a text Trystero writes holds a few dozen values at most, each node some 200 bytes of image
 * beside its string, so the image of the longest is under its text and 16 KiB. A text that needs
 * more holds thousands of values, which no Trystero message has; it is refused once its image has
 * grown to the bound, so that no text costs a room more than a multiple of its bound to read.
 *
 * @param max_bytes The longest text the caller reads.
 * @return The options.
 */
constexpr webcpp::document::parse_options protocol_limits(std::uint64_t max_bytes) noexcept {
    webcpp::document::parse_options o;
    o.max_bytes = max_bytes;
    o.max_document_bytes = (2 * max_bytes) + (std::uint64_t{64} << 10U);
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
 * @param out The text to append to.
 * @param text The string, in UTF-8 (@ref quotable): every string the protocol writes is, the self
 * id checked when the app is made, a candidate's fields when it is written, and every other one is
 * ASCII or was read from JSON.
 */
inline void append_string(std::pmr::string& out, std::string_view text) {
    [[maybe_unused]] const webcpp::document::result<void> quoted =
        webcpp::document::quote(text, out);
    BOOST_ASSERT(quoted.has_value());
}

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
 */
inline void append_key(std::pmr::string& out, std::string_view key) {
    if (out.back() != '{') {
        out += ',';
    }
    append_string(out, key);
    out += ':';
}

}  // namespace webcpp::trystero::protocol::json

#endif
