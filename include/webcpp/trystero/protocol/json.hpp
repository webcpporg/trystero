// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_JSON_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_JSON_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/json_resource.hpp>

#include <boost/json/parser.hpp>
#include <boost/json/serializer.hpp>
#include <boost/json/storage_ptr.hpp>
#include <boost/json/value.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

/**
 * Reading and writing the protocol's JSON texts, with every allocation in the resource the caller
 * gives. A text the protocol writes is a fixed shape of a few strings and numbers, so it is
 * appended field by field rather than built as a boost::json::value, which would copy every string
 * into a node of its own before serializing it again.
 *
 * Tip: strings are escaped by Boost.JSON's serializer, so the bytes match what
 * `boost::json::serialize` and JavaScript's `JSON.stringify` write; a Nostr event id is a hash over
 * exactly these bytes.
 */
namespace webcpp::trystero::protocol::json {

namespace detail {

/** Drains a serializer into `out`, in pieces of a buffer on the stack. */
inline void drain(boost::json::serializer& serializer, std::pmr::string& out) {
    std::array<char, 512> buffer{};
    while (!serializer.done()) {
        const auto piece = serializer.read(buffer.data(), buffer.size());
        out.append(piece.data(), piece.size());
    }
}

}  // namespace detail

/**
 * Appends `text` as a JSON string literal, quotes included, as `JSON.stringify` writes it.
 *
 * Tip: the serializer keeps where it stopped on a stack of its own when the text outgrows its
 * buffer; that stack is in `out`'s resource.
 *
 * @param out The text to append to.
 * @param text The string, in UTF-8.
 */
inline void append_string(std::pmr::string& out, std::string_view text) {
    json_resource resource(out.get_allocator().resource());
    boost::json::serializer serializer{boost::json::storage_ptr(&resource)};
    serializer.reset(boost::json::string_view(text.data(), text.size()));
    detail::drain(serializer, out);
}

/**
 * Appends `value` serialized, as `boost::json::serialize` writes it.
 *
 * @param out The text to append to; the serializer's state is in its resource.
 * @param value The value.
 */
inline void append_value(std::pmr::string& out, const boost::json::value& value) {
    json_resource resource(out.get_allocator().resource());
    boost::json::serializer serializer{boost::json::storage_ptr(&resource)};
    serializer.reset(&value);
    detail::drain(serializer, out);
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

/**
 * Parses `text` into a value of `storage`, or sets `ec`.
 *
 * Tip: `boost::json::parse` keeps its temporaries in the default resource once they outgrow a
 * buffer on the stack; this parser keeps them in `storage` too.
 *
 * @param text The JSON text.
 * @param storage The storage the value and the parser's temporaries are allocated in.
 * @param ec Set when the text is not one JSON value.
 * @return The value, or a null value of `storage` when `ec` is set.
 */
inline boost::json::value parse(std::string_view text, const boost::json::storage_ptr& storage,
                                boost::system::error_code& ec) {
    boost::json::parser parser(storage);
    parser.reset(storage);
    parser.write(text.data(), text.size(), ec);
    if (ec) {
        return boost::json::value(storage);
    }
    return parser.release();
}

}  // namespace webcpp::trystero::protocol::json

#endif
