// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the crypto layer of the port to Trystero 0.27.0 against the vectors recorded from
// 0.27.0's own code (fixtures/expected/trystero-0.27.0.json, written by oracle/record_0_27.mjs).
// The fixture's path is the test's one argument, as webcpp.run passes its input file. The
// fixture is read with webcpp::document; section(name) gives a case the named section of it.

#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/value.hpp>

#include <boost/core/lightweight_test.hpp>

#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cases.hpp"

namespace wd = webcpp::document;
namespace tp = webcpp::trystero::protocol;

namespace {

/** A value of the fixture: a read-only handle of an owned document. */
using fixture_value = wd::basic_const_value<>;

/** The fixture, which main loads before it runs a case. */
std::optional<wd::document>& fixture_document() {
    static std::optional<wd::document> document;
    return document;
}

/** The fixture, read as JSON.parse reads it. */
wd::result<wd::document> load(const char* path) {
    const std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return wd::parse(text.str());
}

/** The named top-level section of the fixture: an array of vectors, or the value itself. */
fixture_value section(std::string_view name) {
    return fixture_document()->root()[name];
}

void fixture_is_trystero_0_27_0() {
    BOOST_TEST_EQ(section("trystero").value_or(std::string_view()), std::string_view("0.27.0"));
}

/** The text of a fixture value. */
std::string_view str(const fixture_value& v) {
    return v.value_or(std::string_view());
}

/** The bytes of a fixture's hexadecimal text. */
tp::bytes bytes_of(const fixture_value& v) {
    auto decoded = tp::from_hex(str(v));
    BOOST_TEST(decoded.has_value());
    return decoded.value_or(tp::bytes());
}

/** An integer of a fixture. */
std::uint64_t integer(const fixture_value& v) {
    return static_cast<std::uint64_t>(v.value_or(std::int64_t{-1}));
}

void base64url_matches() {
    for (const auto entry : section("base64url")) {
        const auto data = bytes_of(entry["bytes"]);
        BOOST_TEST_EQ(tp::to_base64url(data), str(entry["text"]));
        if (data.empty()) {
            // Trystero's encoder writes "" for no bytes; the strict reader refuses "".
            BOOST_TEST(!tp::from_base64url(str(entry["text"])).has_value());
            continue;
        }
        const auto read = tp::from_base64url(str(entry["text"]));
        BOOST_TEST(read.has_value());
        BOOST_TEST(read.value_or(tp::bytes()) == data);
    }
}

void base64url_is_strict() {
    for (const std::string_view text :
         {"", "A", "AB==", "AB+/", "AR", "A B", "AB=", "AB\n", "AAAAA", "A-_", "AB_+"}) {
        BOOST_TEST(!tp::from_base64url(text).has_value());
    }
    // Canonical texts of every length are read, and read back as they were written.
    for (const std::string_view text : {"AA", "AAA", "AAAA", "__4", "-_-_", "AQID"}) {
        const auto read = tp::from_base64url(text);
        BOOST_TEST(read.has_value());
        if (read) {
            BOOST_TEST_EQ(tp::to_base64url(*read), text);
        }
    }
}

void topic_path_matches() {
    for (const auto entry : section("topic_path")) {
        std::vector<std::string_view> parts;
        for (const auto part : entry["parts"]) {
            parts.push_back(str(part));
        }
        // The fixture holds paths of two parts and of three.
        if (parts.size() == 3) {
            BOOST_TEST_EQ(tp::topic_path({parts[0], parts[1], parts[2]}), str(entry["path"]));
        } else if (BOOST_TEST_EQ(parts.size(), 2U)) {
            BOOST_TEST_EQ(tp::topic_path({parts[0], parts[1]}), str(entry["path"]));
        }
    }
    // The escapes JSON.stringify writes, beyond the fixture's.
    BOOST_TEST_EQ(tp::topic_path({"\b\f\n\r\t\x01\x1f", "a/b"}),
                  std::string_view(R"(["\b\f\n\r\t\u0001\u001f","a/b"])"));
}

void hash_topic_matches() {
    for (const auto entry : section("hash_topic")) {
        const auto topic = tp::hash_topic(str(entry["text"]));
        BOOST_TEST_EQ(topic, str(entry["topic"]));
        BOOST_TEST_EQ(topic.size(), 43U);
    }
}

void room_topics_match() {
    for (const auto entry : section("room_topics")) {
        const auto app = str(entry["app"]);
        const auto room = str(entry["room"]);
        BOOST_TEST_EQ(tp::root_topic_text(app, room), str(entry["root_text"]));
        BOOST_TEST_EQ(tp::root_topic(app, room), str(entry["root_topic"]));
        BOOST_TEST_EQ(tp::peer_topic(app, room, str(entry["peer_id"])), str(entry["peer_topic"]));
    }
}

void hash32_matches() {
    for (const auto entry : section("hash32")) {
        BOOST_TEST_EQ(tp::hash32(str(entry["text"])), integer(entry["value"]));
    }
}

void str_to_num_matches() {
    for (const auto entry : section("str_to_num")) {
        BOOST_TEST_EQ(tp::str_to_num(str(entry["text"]), 10000), integer(entry["value"]));
        BOOST_TEST_EQ(tp::str_to_num(str(entry["text"])), tp::hash32(str(entry["text"])));
    }
}

void domain_tags_match() {
    for (const auto entry : section("domain_tags")) {
        BOOST_TEST_EQ(
            tp::domain_tag(str(entry["name"]), static_cast<unsigned>(integer(entry["version"]))),
            str(entry["tag"]));
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!BOOST_TEST_EQ(argc, 2)) {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
            << "usage: crypto_layer_test <fixtures/expected/trystero-0.27.0.json>\n";
        return boost::report_errors();
    }
    auto loaded = load(argv[1]);
    if (!BOOST_TEST(loaded.has_value())) {
        return boost::report_errors();
    }
    fixture_document().emplace(std::move(*loaded));
    webcpp::test::run_case(fixture_is_trystero_0_27_0);
    webcpp::test::run_case(base64url_matches);
    webcpp::test::run_case(base64url_is_strict);
    webcpp::test::run_case(topic_path_matches);
    webcpp::test::run_case(hash_topic_matches);
    webcpp::test::run_case(room_topics_match);
    webcpp::test::run_case(hash32_matches);
    webcpp::test::run_case(str_to_num_matches);
    webcpp::test::run_case(domain_tags_match);
    return boost::report_errors();
}
