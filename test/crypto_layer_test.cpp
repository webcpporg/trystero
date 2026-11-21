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
#include <webcpp/trystero/protocol/room_keys.hpp>
#include <webcpp/trystero/protocol/seal.hpp>
#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/value.hpp>
#include <webcpp/hash/hmac.hpp>
#include <webcpp/hash/pbkdf2.hpp>

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
    // A redundancy above the list's size keeps the whole shuffled list, as a slice does.
    BOOST_TEST_EQ(tp::derive_relays("app", tp::default_nostr_relays, 40).size(), 30U);
    BOOST_TEST_EQ(tp::derive_relays("app", tp::default_nostr_relays, 0).size(), 0U);
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
    for (std::size_t i = 0; i < random.size(); ++i) {
        random[i] = static_cast<std::uint8_t>(i);
    }
    BOOST_TEST_EQ(text(tp::uuid_v4(random)), "00010203-0405-4607-8809-0a0b0c0d0e0f");
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

/** The vector of `room_keys_password` for an app, a room and a password. */
fixture_value password_vector(std::string_view app, std::string_view room,
                              std::string_view password) {
    for (const auto entry : section("room_keys_password")) {
        if (str(entry["app"]) == app && str(entry["room"]) == room &&
            str(entry["password"]) == password) {
            return entry;
        }
    }
    BOOST_TEST(false);
    return section("room_keys_password");
}

void room_key_without_password_matches() {
    for (const auto entry : section("room_key_no_password")) {
        const auto key = tp::room_aes_key_without_password(str(entry["app"]), str(entry["room"]));
        BOOST_TEST(std::ranges::equal(key.bytes(), bytes_of(entry["key"])));
    }
}

void room_keys_from_master_match() {
    for (const auto entry : section("room_keys_password")) {
        const auto salt = bytes_of(entry["salt"]);
        const auto made = tp::pbkdf2_salt(str(entry["app"]), str(entry["room"]));
        BOOST_TEST(std::ranges::equal(made, salt));
        const auto master = bytes_of(entry["master"]);
        BOOST_TEST_EQ(master.size(), 32U);
        const auto keys = tp::derive_room_keys(std::span<const std::uint8_t, 32>(master));
        BOOST_TEST(keys.has_value());
        if (keys) {
            BOOST_TEST(std::ranges::equal(keys->aes.bytes(), bytes_of(entry["aes_key"])));
            BOOST_TEST(std::ranges::equal(keys->hmac.bytes(), bytes_of(entry["hmac_key"])));
        }
    }
}

// 600,000 iterations of HMAC-SHA-256 take about 0.2 s in a release build and tens of times that
// in a debug one, so it runs in the release variant's run only, which defines
// WEBCPP_TRYSTERO_TEST_SLOW_KDF (see main and the Jamfile's crypto_layer_release); one vector
// proves the iteration count and the salt's use.
[[maybe_unused]] void pbkdf2_master_matches() {
    BOOST_TEST_EQ(tp::pbkdf2_iterations, 600'000U);
    const auto entry = password_vector("example.com/app", "lobby", "hunter2");
    const auto salt = bytes_of(entry["salt"]);
    const auto password = str(entry["password"]);
    std::array<std::uint8_t, 32> master{};
    const auto derived = webcpp::hash::pbkdf2<webcpp::hash::hmac_sha2_256>(
        master, std::span(reinterpret_cast<const std::uint8_t*>(password.data()), password.size()),
        salt, tp::pbkdf2_iterations);
    BOOST_TEST(derived.has_value());
    BOOST_TEST(std::ranges::equal(master, bytes_of(entry["master"])));
}

void room_auth_matches() {
    for (const auto entry : section("room_auth")) {
        const auto app = str(entry["app"]);
        const auto room = str(entry["room"]);
        const auto peer = str(entry["peer_id"]);
        const auto seq = integer(entry["seq"]);
        const auto keys = password_vector(app, room, str(entry["password"]));
        const auto hmac_bytes = bytes_of(keys["hmac_key"]);
        webcpp::crypto::secret<32> hmac;
        std::ranges::copy(hmac_bytes, hmac.writable().begin());
        BOOST_TEST_EQ(tp::room_auth_payload(app, room, peer, seq), str(entry["payload"]));
        const auto token = tp::sign_room_auth(hmac, app, room, peer, seq);
        BOOST_TEST_EQ(token, str(entry["token"]));
        BOOST_TEST_EQ(token.size(), 43U);
        BOOST_TEST_EQ(tp::verify_room_auth(hmac, app, room, peer, seq, token),
                      entry["verifies"].value_or(false));
        BOOST_TEST_EQ(tp::verify_room_auth(hmac, app, room, peer, seq + 1, token),
                      entry["wrong_seq_verifies"].value_or(true));
        BOOST_TEST(!tp::verify_room_auth(hmac, app, room, std::string(peer) + "x", seq, token));
    }
    // The largest safe integer is written as a decimal integer.
    BOOST_TEST_EQ(tp::room_auth_payload("a", "r", "p", tp::max_room_auth_seq),
                  std::string_view(R"(["trystero:ov-auth:v3","a","r","p",9007199254740991])"));
}

void room_auth_token_is_strict() {
    const auto entry = section("room_auth")[std::size_t{0}];
    const auto app = str(entry["app"]);
    const auto room = str(entry["room"]);
    const auto peer = str(entry["peer_id"]);
    const auto seq = integer(entry["seq"]);
    const auto hmac_bytes =
        bytes_of(password_vector(app, room, str(entry["password"]))["hmac_key"]);
    webcpp::crypto::secret<32> hmac;
    std::ranges::copy(hmac_bytes, hmac.writable().begin());
    const std::string token(str(entry["token"]));
    BOOST_TEST(tp::verify_room_auth(hmac, app, room, peer, seq, token));
    BOOST_TEST(!tp::verify_room_auth(hmac, app, room, peer, seq, token + "="));
    BOOST_TEST(!tp::verify_room_auth(hmac, app, room, peer, seq, token.substr(0, 42)));
    BOOST_TEST(!tp::verify_room_auth(hmac, app, room, peer, seq, token + "A"));
    BOOST_TEST(!tp::verify_room_auth(hmac, app, room, peer, seq, ""));
    // The 43rd character carries 4 bits and 2 unused ones: another character with the same top 4
    // bits decodes to the same bytes, and is refused as non-canonical.
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const auto last = alphabet.find(token.back());
    BOOST_TEST(last != std::string_view::npos);
    auto twin = token;
    twin.back() = alphabet[last ^ 1U];
    BOOST_TEST(twin != token);
    BOOST_TEST(!tp::verify_room_auth(hmac, app, room, peer, seq, twin));
    BOOST_TEST(!tp::from_base64url(twin).has_value());
    // A sequence number beyond 2^53 - 1 is refused, as 0.27.0 refuses it, not asserted.
    BOOST_TEST(!tp::verify_room_auth(hmac, app, room, peer, tp::max_room_auth_seq + 1, token));
    BOOST_TEST(!tp::verify_room_auth(hmac, app, room, peer, UINT64_MAX, token));
    auto outside = token;
    outside[3] = '+';
    BOOST_TEST(!tp::verify_room_auth(hmac, app, room, peer, seq, outside));
}

/** The AES key of an entry of "seal" or "open_refused": without a password, or the vector's. */
webcpp::crypto::secret<32> key_of(const fixture_value& entry) {
    const auto app = str(entry["app"]);
    const auto room = str(entry["room"]);
    const auto password = str(entry["password"]);
    if (password.empty()) {
        return tp::room_aes_key_without_password(app, room);
    }
    webcpp::crypto::secret<32> key;
    std::ranges::copy(bytes_of(password_vector(app, room, password)["aes_key"]),
                      key.writable().begin());
    return key;
}

/** An entry's AAD: absent when the fixture holds null. */
std::optional<std::string_view> aad_of(const fixture_value& entry) {
    const auto aad = entry["aad"];
    if (aad.is_null()) {
        return std::nullopt;
    }
    return str(aad);
}

void seal_matches() {
    for (const auto entry : section("seal")) {
        const auto key = key_of(entry);
        const auto iv = bytes_of(entry["iv"]);
        const auto plaintext = str(entry["plaintext"]);
        const auto aad = aad_of(entry);
        const auto sealed =
            tp::seal(key, std::span<const std::uint8_t, tp::gcm_iv_size>(iv), plaintext, aad);
        BOOST_TEST(sealed.has_value());
        if (sealed) {
            BOOST_TEST_EQ(*sealed, str(entry["sealed"]));
            BOOST_TEST_EQ(sealed->size(), tp::sealed_size(plaintext.size()));
        }
        const auto opened = tp::open(key, str(entry["sealed"]), aad);
        BOOST_TEST(opened.has_value());
        if (opened) {
            BOOST_TEST_EQ(*opened, plaintext);
        }
    }
}

void password_probe_opens() {
    for (const auto entry : section("room_keys_password")) {
        webcpp::crypto::secret<32> key;
        std::ranges::copy(bytes_of(entry["aes_key"]), key.writable().begin());
        const auto opened = tp::open(key, str(entry["probe_sealed"]), "offer|p|o");
        BOOST_TEST(opened.has_value());
        if (opened) {
            BOOST_TEST_EQ(*opened, std::string_view("probe"));
        }
        // Another AAD, and no AAD, do not open it.
        BOOST_TEST(!tp::open(key, str(entry["probe_sealed"]), "offer|p|x").has_value());
        BOOST_TEST(!tp::open(key, str(entry["probe_sealed"]), std::nullopt).has_value());
    }
}

void open_refuses_what_trystero_refuses() {
    for (const auto entry : section("open_refused")) {
        BOOST_TEST(entry["refused"].value_or(false));
        const auto opened = tp::open(key_of(entry), str(entry["sealed"]), std::nullopt);
        BOOST_TEST(!opened.has_value());
        if (!opened) {
            BOOST_TEST(opened.error() == webcpp::trystero::error::decrypt_failed);
        }
    }
    // A text with padding, a character outside the alphabet and another last character of the
    // same bytes, of a sealed text that opens.
    const auto entry = section("seal")[std::size_t{4}];
    const auto key = key_of(entry);
    const std::string good(str(entry["sealed"]));
    BOOST_TEST(tp::open(key, good, std::nullopt).has_value());
    BOOST_TEST(!tp::open(key, good + "=", std::nullopt).has_value());
    BOOST_TEST(!tp::open(key, good + "A", std::nullopt).has_value());
    auto outside = good;
    outside[5] = '+';
    BOOST_TEST(!tp::open(key, outside, std::nullopt).has_value());
    // Fewer than 28 bytes: 27 bytes are 36 characters.
    BOOST_TEST(!tp::open(key, std::string(36, 'A'), std::nullopt).has_value());
    // A tag that does not match.
    auto flipped = good;
    flipped[flipped.size() / 2] = flipped[flipped.size() / 2] == 'A' ? 'B' : 'A';
    BOOST_TEST(!tp::open(key, flipped, std::nullopt).has_value());
}

void open_refuses_another_aad() {
    for (const auto entry : section("seal")) {
        const auto key = key_of(entry);
        const auto aad = aad_of(entry);
        const std::string_view other = aad ? std::string_view("answer|x|y") : "offer|x|y";
        BOOST_TEST(!tp::open(key, str(entry["sealed"]), other).has_value());
        if (aad) {
            BOOST_TEST(!tp::open(key, str(entry["sealed"]), std::nullopt).has_value());
        } else {
            // An absent AAD and an empty one are the same to GCM.
            BOOST_TEST(tp::open(key, str(entry["sealed"]), std::string_view()).has_value());
        }
    }
}

// A plaintext that is not UTF-8 seals, and is refused on opening (design D12).
void open_refuses_a_plaintext_that_is_not_utf8() {
    const auto key = tp::room_aes_key_without_password("a", "r");
    const std::array<std::uint8_t, tp::gcm_iv_size> iv{};
    const auto sealed = tp::seal(key, iv, std::string_view("ab\xff\xfe", 4), std::nullopt);
    BOOST_TEST(sealed.has_value());
    if (sealed) {
        const auto opened = tp::open(key, *sealed, std::nullopt);
        BOOST_TEST(!opened.has_value());
        if (!opened) {
            BOOST_TEST(opened.error() == webcpp::trystero::error::decrypt_failed);
        }
    }
}

void signal_aad_matches() {
    for (const auto entry : section("signal_aad")) {
        BOOST_TEST_EQ(
            tp::signal_aad(str(entry["type"]), str(entry["peer_id"]), str(entry["offer_id"])),
            str(entry["aad"]));
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
    webcpp::test::run_case(room_key_without_password_matches);
    webcpp::test::run_case(room_keys_from_master_match);
#ifdef WEBCPP_TRYSTERO_TEST_SLOW_KDF
    webcpp::test::run_case(pbkdf2_master_matches);
#endif
    webcpp::test::run_case(room_auth_matches);
    webcpp::test::run_case(room_auth_token_is_strict);
    webcpp::test::run_case(seal_matches);
    webcpp::test::run_case(password_probe_opens);
    webcpp::test::run_case(open_refuses_what_trystero_refuses);
    webcpp::test::run_case(open_refuses_another_aad);
    webcpp::test::run_case(open_refuses_a_plaintext_that_is_not_utf8);
    webcpp::test::run_case(signal_aad_matches);
    return boost::report_errors();
}
