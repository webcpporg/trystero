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

#include <charconv>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

/**
 * Reading and writing the protocol's JSON texts, with every allocation in the resource the caller gives. A text the
 * protocol writes is a fixed shape of a few strings and numbers, so it is appended field by field rather than built as
 * a boost::json::value, which would copy every string into a node of its own before serializing it again.
 *
 * Tip: strings are escaped by Boost.JSON's serializer, so the bytes match what `boost::json::serialize` and
 * JavaScript's `JSON.stringify` write; a Nostr event id is a hash over exactly these bytes.
 */
namespace webcpp::trystero::protocol::json {

namespace detail {

/** Drains a serializer into `out`, in pieces of a buffer on the stack. */
inline void drain(boost::json::serializer& serializer, std::pmr::string& out) {
    char buffer[512];  // NOLINT(cppcoreguidelines-avoid-c-arrays)
    while (!serializer.done()) {
        const auto piece = serializer.read(buffer, sizeof buffer);
        out.append(piece.data(), piece.size());
    }
}

}  // namespace detail

/**
 * Appends `text` as a JSON string literal, quotes included.
 *
 * Tip: the serializer keeps where it stopped on a stack of its own when the text outgrows its buffer; that stack is
 * in `out`'s resource.
 */
inline void append_string(std::pmr::string& out, std::string_view text) {
    json_resource resource(out.get_allocator().resource());
    boost::json::serializer serializer{boost::json::storage_ptr(&resource)};
    serializer.reset(boost::json::string_view(text.data(), text.size()));
    detail::drain(serializer, out);
}

/** Appends `value` serialized, as `boost::json::serialize` writes it. */
inline void append_value(std::pmr::string& out, const boost::json::value& value) {
    json_resource resource(out.get_allocator().resource());
    boost::json::serializer serializer{boost::json::storage_ptr(&resource)};
    serializer.reset(&value);
    detail::drain(serializer, out);
}

inline void append_integer(std::pmr::string& out, std::int64_t value) {
    char buffer[24];  // NOLINT(cppcoreguidelines-avoid-c-arrays)
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof buffer, value);
    out.append(buffer, end);
}

/** Appends `"key":` with a leading comma unless it opens the object. */
inline void append_key(std::pmr::string& out, std::string_view key) {
    if (out.back() != '{') out += ',';
    append_string(out, key);
    out += ':';
}

/**
 * Parses `text` into a value of `storage`, or sets `ec`.
 *
 * Tip: `boost::json::parse` keeps its temporaries in the default resource once they outgrow a buffer on the stack; this
 * parser keeps them in `storage` too.
 */
inline boost::json::value parse(std::string_view text, const boost::json::storage_ptr& storage, boost::system::error_code& ec) {
    boost::json::parser parser(storage);
    parser.reset(storage);
    parser.write(text.data(), text.size(), ec);
    if (ec) return boost::json::value(storage);
    return parser.release();
}

}  // namespace webcpp::trystero::protocol::json

#endif
