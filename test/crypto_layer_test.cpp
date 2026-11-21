// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the crypto layer of the port to Trystero 0.27.0 against the vectors recorded from
// 0.27.0's own code (fixtures/expected/trystero-0.27.0.json, written by oracle/record_0_27.mjs).
// The fixture's path is the test's one argument, as webcpp.run passes its input file. The
// fixture is read with webcpp::document; section(name) gives a case the named section of it.

#include <webcpp/trystero/protocol/ids.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/value.hpp>

#include <boost/core/lightweight_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <span>
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

// The one writer of a string that is not refused: JSON.stringify's output on UTF-8, with DEL and
// U+2028 raw; and, on a byte that is not UTF-8, the byte as it is.
void the_total_string_writer_is_json_stringifys() {
    for (const std::string_view text :
         {"", "plain", "a\"b\\c", "\b\f\n\r\t", "\x01\x1f", "del\x7f", "ls\xe2\x80\xa8ps",
          "caf\xc3\xa9 \xf0\x9f\x98\x80", "\x7f\xe2\x80\xa9"}) {
        std::pmr::string total;
        tp::json::append_string_total(total, text);
        std::pmr::string checked;
        BOOST_TEST(tp::json::append_string(checked, text).has_value());
        BOOST_TEST_EQ(total, checked);
    }
    // DEL and U+2028 are written raw, as JSON.stringify writes them.
    std::pmr::string raw;
    tp::json::append_string_total(raw, "\x7f\xe2\x80\xa8");
    BOOST_TEST_EQ(raw, std::string_view("\"\x7f\xe2\x80\xa8\""));
    // A byte that is not UTF-8 is kept, where append_string refuses the text.
    std::pmr::string bad;
    tp::json::append_string_total(bad, "a\xffz");
    BOOST_TEST_EQ(bad, std::string_view("\"a\xffz\""));
    std::pmr::string refused;
    BOOST_TEST(!tp::json::append_string(refused, "a\xffz").has_value());
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

// The shuffle's order for each seed, the seed being str_to_num of the app id, and the relays an
// app derives: the first five of the order, as indices into the 30 defaults.
void shuffle_matches() {
    BOOST_TEST_EQ(tp::default_nostr_relays.size(), 30U);
    BOOST_TEST_EQ(tp::default_nostr_relays.front(), "wss://bucket.coracle.social");
    BOOST_TEST_EQ(tp::default_nostr_relays.back(), "wss://yabu.me");
    for (const auto entry : section("shuffle")) {
        const auto seed = static_cast<std::uint32_t>(integer(entry["seed"]));
        BOOST_TEST_EQ(tp::str_to_num(str(entry["app"])), seed);
        std::vector<std::size_t> expected;
        for (const auto i : entry["order"]) {
            expected.push_back(static_cast<std::size_t>(integer(i)));
        }
        std::array<std::size_t, 30> order{};
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        tp::seeded_shuffle(std::span<std::size_t>(order), seed);
        BOOST_TEST_ALL_EQ(order.begin(), order.end(), expected.begin(), expected.end());

        const auto derived = tp::derive_relays(str(entry["app"]), tp::default_nostr_relays, 5);
        BOOST_TEST_EQ(derived.size(), 5U);
        for (std::size_t i = 0; i < derived.size() && i < expected.size(); ++i) {
            BOOST_TEST_EQ(derived[i], tp::default_nostr_relays[expected[i]]);
        }
    }
    // A zero seed is one, as Trystero's `|| 1`.
    std::array<int, 8> zero{0, 1, 2, 3, 4, 5, 6, 7};
    std::array<int, 8> one = zero;
    tp::seeded_shuffle(std::span<int>(zero), 0);
    tp::seeded_shuffle(std::span<int>(one), 1);
    BOOST_TEST(zero == one);
}

// genId over the fixture's random cycle, each round drawing gen_id_draw_size bytes from where the
// last one stopped, as the recorder's withRandom does.
void gen_id_matches() {
    for (const auto entry : section("gen_id")) {
        const auto cycle = bytes_of(entry["random_cycle"]);
        std::size_t at = 0;
        const auto n = static_cast<std::size_t>(integer(entry["n"]));
        const auto id = tp::gen_id(n, [&](std::span<std::uint8_t> out) {
            BOOST_TEST_EQ(out.size(), tp::gen_id_draw_size(n));
            for (auto& byte : out) {
                byte = cycle[at++ % cycle.size()];
            }
        });
        BOOST_TEST_EQ(id, str(entry["id"]));
    }
    BOOST_TEST_EQ(tp::gen_id_draw_size(8), 14U);
    BOOST_TEST_EQ(tp::gen_id_draw_size(20), 27U);
    BOOST_TEST(tp::gen_id(0, [](std::span<std::uint8_t>) { BOOST_TEST(false); }).empty());
    // Bytes of 248 and above are skipped: only the last one of three rounds is kept.
    int round = 0;
    const auto skipped = tp::gen_id(1, [&](std::span<std::uint8_t> out) {
        std::ranges::fill(out, round++ < 2 ? std::uint8_t{248} : std::uint8_t{61});
    });
    BOOST_TEST_EQ(skipped, "z");
    BOOST_TEST_EQ(round, 3);
}

void identity_ids_match() {
    for (const auto entry : section("identity_ids")) {
        const auto compact = str(entry["compact_pub"]);
        const auto point = tp::parse_compact_public_key(compact);
        BOOST_TEST(point.has_value());
        if (point) {
            BOOST_TEST_EQ((*point)[0], 0x04);
            BOOST_TEST_EQ(tp::compact_public_key(*point), compact);
        }
        BOOST_TEST_EQ(tp::derive_peer_id(compact), str(entry["peer_id"]));
        BOOST_TEST_EQ(tp::derive_user_id(compact), str(entry["user_id"]));
        BOOST_TEST_EQ(tp::derive_peer_id(compact).size(), tp::peer_id_size);
        BOOST_TEST_EQ(tp::derive_user_id(compact).size(), tp::user_id_size);
    }
    // encode_base62 keeps the low digits: 62^2 + 5 is "105" in base 62 and "05" in two digits.
    const std::array<std::uint8_t, 2> number{0x0f, 0x09};  // 3844 + 5 = 3849
    BOOST_TEST_EQ(tp::encode_base62(number, 3), "105");
    BOOST_TEST_EQ(tp::encode_base62(number, 2), "05");
    BOOST_TEST_EQ(tp::encode_base62(number, 4), "0105");
    BOOST_TEST_EQ(tp::encode_base62(std::span<const std::uint8_t>{}, 3), "000");
}

void uuid_v4_has_its_form() {
    const auto text = [](const std::array<char, tp::uuid_text_size>& uuid) {
        return std::string_view(uuid.data(), uuid.size());
    };
    std::array<std::uint8_t, 16> random{};
    random.fill(0xff);
    BOOST_TEST_EQ(text(tp::uuid_v4(random)), "ffffffff-ffff-4fff-bfff-ffffffffffff");
    random.fill(0);
    BOOST_TEST_EQ(text(tp::uuid_v4(random)), "00000000-0000-4000-8000-000000000000");
}

void compact_key_is_strict() {
    const auto& entries = section("identity_ids");
    const std::string compact(str(entries[0]["compact_pub"]));
    BOOST_TEST(tp::parse_compact_public_key(compact).has_value());
    BOOST_TEST(!tp::parse_compact_public_key(compact.substr(0, 86)));
    BOOST_TEST(!tp::parse_compact_public_key(compact + "A"));
    BOOST_TEST(!tp::parse_compact_public_key(""));
    auto moved = compact;  // the dot one place off, the length unchanged
    moved[43] = 'A';
    moved[42] = '.';
    BOOST_TEST(!tp::parse_compact_public_key(moved));
    auto no_dot = compact;
    no_dot[43] = 'A';
    BOOST_TEST(!tp::parse_compact_public_key(no_dot));
    auto padded = compact;  // a half of 44 characters, padded, with the whole 87 long
    padded[42] = '=';
    BOOST_TEST(!tp::parse_compact_public_key(padded));
    auto outside = compact;
    outside[5] = '+';
    BOOST_TEST(!tp::parse_compact_public_key(outside));
    auto non_canonical = compact;  // the last character's unused bits set
    non_canonical.back() = 'W';
    BOOST_TEST(!tp::parse_compact_public_key(non_canonical));
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
    webcpp::test::run_case(the_total_string_writer_is_json_stringifys);
    webcpp::test::run_case(hash_topic_matches);
    webcpp::test::run_case(room_topics_match);
    webcpp::test::run_case(hash32_matches);
    webcpp::test::run_case(str_to_num_matches);
    webcpp::test::run_case(domain_tags_match);
    webcpp::test::run_case(shuffle_matches);
    webcpp::test::run_case(gen_id_matches);
    webcpp::test::run_case(identity_ids_match);
    webcpp::test::run_case(uuid_v4_has_its_form);
    webcpp::test::run_case(compact_key_is_strict);
    return boost::report_errors();
}
