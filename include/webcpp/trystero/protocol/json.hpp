// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_JSON_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_JSON_HPP

#include <webcpp/trystero/config.hpp>

#include <boost/json/serializer.hpp>

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

/**
 * Writing the protocol's JSON texts into one buffer. A text the protocol writes is a fixed shape of a few strings and
 * numbers, so it is appended field by field rather than built as a boost::json::value, which would copy every string
 * into a node of its own before serializing it again.
 *
 * Tip: strings are escaped by Boost.JSON's serializer, so the bytes match what `boost::json::serialize` and
 * JavaScript's `JSON.stringify` write; a Nostr event id is a hash over exactly these bytes.
 */
namespace webcpp::trystero::protocol::json {

/** Appends `text` as a JSON string literal, quotes included. */
inline void append_string(std::string& out, std::string_view text) {
    boost::json::serializer serializer;
    serializer.reset(boost::json::string_view(text.data(), text.size()));
    char buffer[512];
    while (!serializer.done()) {
        const auto chunk = serializer.read(buffer, sizeof buffer);
        out.append(chunk.data(), chunk.size());
    }
}

inline void append_integer(std::string& out, std::int64_t value) {
    char buffer[24];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof buffer, value);
    out.append(buffer, end);
}

/** Appends `"key":` with a leading comma unless it opens the object. */
inline void append_key(std::string& out, std::string_view key) {
    if (out.back() != '{') out += ',';
    append_string(out, key);
    out += ':';
}

}  // namespace webcpp::trystero::protocol::json

#endif
