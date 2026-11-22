// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the crypto layer of the port to Trystero 0.27.0 against the vectors recorded from
// 0.27.0's own code (fixtures/expected/trystero-0.27.0.json, written by oracle/record_0_27.mjs).
// The fixture's path is the test's one argument, as webcpp.run passes its input file. The
// fixture is read with webcpp::document; section(name) gives a case the named section of it.

#include <webcpp/trystero/detail/identity_cache.hpp>
#include <webcpp/trystero/detail/identity_handshake.hpp>
#include <webcpp/trystero/detail/wipe.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/identity.hpp>
#include <webcpp/trystero/protocol/action_wire.hpp>
#include <webcpp/trystero/protocol/ecdsa.hpp>
#include <webcpp/trystero/protocol/handshake.hpp>
#include <webcpp/trystero/protocol/identity.hpp>
#include <webcpp/trystero/protocol/ids.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/nostr.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/room_keys.hpp>
#include <webcpp/trystero/protocol/seal.hpp>
#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/crypto/curves/p256.hpp>
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
#include <iterator>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cases.hpp"

namespace wd = webcpp::document;
namespace tp = webcpp::trystero::protocol;
namespace trystero = webcpp::trystero;

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
    const auto decoded = tp::from_hex(str(v));
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
    for (const std::string_view text : {
             "",
             "plain",
             "a\"b\\c",
             "\b\f\n\r\t",
             "\x01\x1f",
             "del\x7f",
             "ls\xe2\x80\xa8ps",
             "caf\xc3\xa9 \xf0\x9f\x98\x80",
             "\x7f\xe2\x80\xa9",
         }) {
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

// A topic's kind is 0.27.0's topicToKind: FNV-1a over its UTF-16 code units, an astral character
// as its two surrogates, modulo 10000 and above 20000; a room's root topic has the kind recorded
// with it.
void topic_kind_matches() {
    for (const auto entry : section("topic_to_kind")) {
        BOOST_TEST_EQ(tp::nostr::topic_kind(str(entry["topic"])), integer(entry["kind"]));
    }
    for (const auto entry : section("room_topics")) {
        BOOST_TEST_EQ(tp::nostr::topic_kind(str(entry["root_topic"])), integer(entry["root_kind"]));
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
    // same bytes, of a sealed text that opens: the seal of a non-ASCII plaintext, without a
    // password or an AAD, under the IV 0..11.
    std::optional<fixture_value> chosen;
    for (const auto candidate : section("seal")) {
        if (!chosen && str(candidate["app"]) == "example.com/app" &&
            str(candidate["password"]).empty() &&
            str(candidate["iv"]) == "000102030405060708090a0b" &&
            str(candidate["plaintext"]) == "caf\xc3\xa9 \xf0\x9f\x98\x80" &&
            candidate["aad"].is_null()) {
            chosen = candidate;
        }
    }
    if (!BOOST_TEST(chosen.has_value())) {
        return;
    }
    const auto entry = *chosen;
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

// The library's own wipe zeroes a buffer and a plain object.
void wipe_zeroes_what_it_is_given() {
    std::array<unsigned char, 48> buffer{};
    buffer.fill(0xA5);
    webcpp::trystero::detail::wipe(buffer.data(), buffer.size());
    BOOST_TEST(std::ranges::all_of(buffer, [](unsigned char byte) { return byte == 0; }));

    struct plain {
        std::uint64_t word;
        std::array<std::uint8_t, 13> tail;
    };

    plain object{.word = ~std::uint64_t{0}, .tail = {}};
    object.tail.fill(0xFF);
    webcpp::trystero::detail::wipe_object(object);
    BOOST_TEST_EQ(object.word, 0U);
    BOOST_TEST(std::ranges::all_of(object.tail, [](std::uint8_t byte) { return byte == 0; }));

    webcpp::trystero::detail::wipe(nullptr, 0);
}

void signal_aad_matches() {
    for (const auto entry : section("signal_aad")) {
        BOOST_TEST_EQ(
            tp::signal_aad(str(entry["type"]), str(entry["peer_id"]), str(entry["offer_id"])),
            str(entry["aad"]));
    }
}

/** The payload of an action frames case: its text, its bytes, or (i * 31) mod 256 of its size. */
tp::bytes payload_of(const fixture_value& entry) {
    if (const auto text = entry["payload_text"].value_or(std::string_view()); !text.empty()) {
        return {text.begin(), text.end()};
    }
    if (entry["payload_hex"].is_string()) {
        return bytes_of(entry["payload_hex"]);
    }
    tp::bytes out(integer(entry["payload_size"]));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(i * 31);
    }
    return out;
}

/** The text of a fixture value, or empty for null. */
std::string_view text_or_empty(const fixture_value& v) {
    return v.value_or(std::string_view());
}

/** Whether `encoded` is the recorded frame: its hex, or, for a chunk, its header, size and
 * SHA-256. */
bool is_recorded(const tp::bytes& encoded, const fixture_value& frame) {
    if (frame["hex"].is_string()) {
        const auto expected = bytes_of(frame["hex"]);
        return BOOST_TEST_ALL_EQ(encoded.begin(), encoded.end(), expected.begin(), expected.end());
    }
    const auto head = bytes_of(frame["head"]);
    const auto digest =
        tp::sha256(std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
    return BOOST_TEST_EQ(encoded.size(), integer(frame["size"])) &&
           BOOST_TEST(std::ranges::equal(std::span(encoded).first(head.size()), head)) &&
           BOOST_TEST_EQ(tp::to_hex(digest), str(frame["sha256"]));
}

/** An action frames case, as the frames of it read it. */
struct frames_case {
    fixture_value entry;
    std::string_view name;
    tp::bytes payload;
    tp::payload_format format{};
    std::string_view metadata;
    bool wide = false;
    std::size_t chunk = 0;
    /** The chunks met so far. */
    std::size_t chunks = 0;
};

/** One recorded frame of a case: who sent it, its kind, and, for a refusal, its reason. */
struct recorded_frame {
    fixture_value frame;
    bool from_sender = false;
    tp::frame_kind kind{};
    /** A control frame's reason, as recorded: what follows its header. */
    std::string reason;

    [[nodiscard]] std::uint32_t transfer_id() const {
        return static_cast<std::uint32_t>(integer(frame["transfer_id"]));
    }
};

recorded_frame read_frame(const fixture_value& frame) {
    recorded_frame out{
        .frame = frame,
        .from_sender = str(frame["from"]) == "A",
        .kind = static_cast<tp::frame_kind>(frame["hex"].is_string()
                                                ? bytes_of(frame["hex"])[1] & 15U
                                                : bytes_of(frame["head"])[1] & 15U),
        .reason = {},
    };
    if (out.kind == tp::frame_kind::rejected) {
        const auto recorded = bytes_of(frame["hex"]);
        out.reason.assign(recorded.begin() + tp::control_header_size, recorded.end());
    }
    return out;
}

/** The inline frame the port writes for a recorded one: the sender's of the case's action, or
 * the receiver's answer. */
tp::bytes encode_inline_of(const frames_case& c, const recorded_frame& f) {
    if (f.from_sender) {
        return tp::encode_inline(str(c.entry["action"]), c.format, c.metadata, c.payload);
    }
    const auto answer = c.entry["answer"];
    const auto text = str(answer["payload_text"]);
    return tp::encode_inline(str(answer["action"]),
                             static_cast<tp::payload_format>(integer(answer["format"])),
                             str(answer["metadata"]), tp::bytes(text.begin(), text.end()));
}

/** The frame the port writes from the inputs of a recorded one; a chunk counts in `c`. */
tp::bytes encode_like(frames_case& c, const recorded_frame& f) {
    switch (f.kind) {
        case tp::frame_kind::inline_payload: return encode_inline_of(c, f);
        case tp::frame_kind::offer:
            return tp::encode_offer(str(c.entry["action"]), c.format, c.metadata, f.transfer_id(),
                                    c.payload.size(),
                                    c.wide ? tp::chunk_width::wide : tp::chunk_width::narrow);
        case tp::frame_kind::chunk: {
            const auto offset = c.chunks++ * c.chunk;
            const auto length = std::min(c.chunk, c.payload.size() - offset);
            return tp::encode_chunk(f.transfer_id(), offset,
                                    std::span(c.payload).subspan(offset, length));
        }
        case tp::frame_kind::accepted: return tp::encode_accepted(f.transfer_id());
        case tp::frame_kind::cancelled: return tp::encode_cancelled(f.transfer_id());
        case tp::frame_kind::rejected: return tp::encode_rejected(f.transfer_id(), f.reason);
    }
    return {};
}

/** Checks an inline frame or an offer read back against the case it was written from. */
void check_proposal(const frames_case& c, const recorded_frame& f, const tp::proposal& p) {
    BOOST_TEST(p.kind == f.kind);
    if (f.from_sender) {
        BOOST_TEST_EQ(p.name, str(c.entry["action"]));
        BOOST_TEST(p.format == c.format);
        BOOST_TEST_EQ(p.metadata, c.metadata);
        BOOST_TEST_EQ(p.size, c.payload.size());
    }
    if (f.kind == tp::frame_kind::offer) {
        BOOST_TEST_EQ(p.wide, c.wide);
        BOOST_TEST_EQ(p.id, f.transfer_id());
        return;
    }
    if (f.from_sender) {
        BOOST_TEST(std::ranges::equal(p.payload, c.payload));
        return;
    }
    const auto answer = c.entry["answer"];
    const auto text = str(answer["payload_text"]);
    BOOST_TEST_EQ(p.name, str(answer["action"]));
    BOOST_TEST(p.format == static_cast<tp::payload_format>(integer(answer["format"])));
    BOOST_TEST_EQ(p.metadata, str(answer["metadata"]));
    BOOST_TEST(std::ranges::equal(
        p.payload, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
}

/** Checks a frame read back against the recorded one it was written like. */
void check_decoded(const frames_case& c, const recorded_frame& f, const tp::decoded_action& frame) {
    if (const auto* p = std::get_if<tp::proposal>(&frame)) {
        check_proposal(c, f, *p);
    } else if (const auto* piece = std::get_if<tp::chunk>(&frame)) {
        BOOST_TEST_EQ(piece->offset, static_cast<double>((c.chunks - 1) * c.chunk));
        BOOST_TEST_EQ(piece->id, f.transfer_id());
    } else {
        const auto& m = std::get<tp::control_message>(frame);
        BOOST_TEST(m.kind == f.kind);
        BOOST_TEST_EQ(m.id, f.transfer_id());
        BOOST_TEST_EQ(m.reason, f.reason);
    }
}

// Each frame Trystero 0.27.0's action-wire.ts sent between two of its managers: the port writes it
// from the same inputs, and reads it back to the same message. A case's first frame is the sender's
// inline frame or offer of the case's action, format, metadata and payload; a request's answer is
// the receiver's inline frame of "@_response".
void action_frames_match() {
    for (const auto entry : section("action_frames")) {
        const bool wide = entry["wide"].value_or(false);
        frames_case c{
            .entry = entry,
            .name = str(entry["case"]),
            .payload = payload_of(entry),
            .format = static_cast<tp::payload_format>(integer(entry["format"])),
            .metadata = text_or_empty(entry["metadata"]),
            .wide = wide,
            .chunk = wide ? tp::wide_chunk_size : tp::chunk_size,
            .chunks = 0,
        };
        for (const auto frame : entry["frames"]) {
            const auto f = read_frame(frame);
            const auto encoded = encode_like(c, f);
            if (!is_recorded(encoded, frame)) {
                BOOST_ERROR(std::string("case ").append(c.name).c_str());
                continue;
            }
            const auto decoded = tp::decode_action(encoded);
            if (BOOST_TEST(decoded.frame.has_value())) {
                check_decoded(c, f, *decoded.frame);
            }
        }
        if (c.chunks != 0 && c.name != "cancelled") {
            BOOST_TEST_EQ(c.chunks, (c.payload.size() + c.chunk - 1) / c.chunk);
        }
    }
    BOOST_TEST_EQ(section("action_frames").size(), 10U);
}

// The handshake's frames 0.27.0 writes (handshake_frames), each the inline frame of its action:
// the ready frame, `@_hsready` with the empty text of 0.27.0's `payload || ''`, as the port's
// encoder writes it for a room. The identity steps' and the refusals' frames come with the tasks
// that send them (2.4b, 2.5a).
void handshake_frames_match() {
    std::size_t matched = 0;
    for (const auto entry : section("handshake_frames")) {
        if (str(entry["case"]) != "ready") {
            continue;
        }
        BOOST_TEST_EQ(str(entry["action"]), "@_hsready");
        BOOST_TEST_EQ(str(entry["payload_json"]), R"("")");
        BOOST_TEST(entry["delivered"].value_or(false) == true);
        std::vector<fixture_value> frames;
        for (const auto frame : entry["frames"]) {
            frames.push_back(frame);
        }
        if (!BOOST_TEST_EQ(frames.size(), 1U)) {
            continue;
        }
        BOOST_TEST_EQ(str(frames[0]["from"]), "A");
        const auto encoded = tp::encode_inline("@_hsready", tp::payload_format::text, "", {});
        BOOST_TEST_EQ(encoded.size(), integer(frames[0]["size"]));
        BOOST_TEST(is_recorded(encoded, frames[0]));
        ++matched;
    }
    BOOST_TEST_EQ(matched, 1U);
}

// The P-256 identity of 0.27.0 (Phase 2): keys, compact keys, the user key's text, the user-proof
// and low S, against the sections 0.27.0's recorder wrote.

namespace p256 = webcpp::crypto::curves::p256;

/** The key pair of a fixture's base64url scalar. */
std::optional<p256::keypair> keypair_of(const fixture_value& d) {
    const auto scalar = tp::from_base64url(str(d));
    if (!BOOST_TEST(scalar.has_value()) || !BOOST_TEST_EQ(scalar->size(), 32U)) {
        return std::nullopt;
    }
    auto key = p256::keypair::make(*scalar);
    if (!BOOST_TEST(key.has_value())) {
        return std::nullopt;
    }
    return std::move(*key);
}

/** The entry of `p256_keys` labelled `label`. */
fixture_value p256_key_entry(std::string_view label) {
    for (const auto entry : section("p256_keys")) {
        if (str(entry["label"]) == label) {
            return entry;
        }
    }
    BOOST_ERROR("p256_keys has no such label");
    return section("p256_keys")[0];
}

/** The public key of a compact text, which must be one. */
std::optional<p256::public_key> public_key_of(std::string_view compact) {
    const auto point = tp::parse_compact_public_key(compact);
    if (!BOOST_TEST(point.has_value())) {
        return std::nullopt;
    }
    const auto key = p256::public_key::make(*point);
    if (!BOOST_TEST(key.has_value())) {
        return std::nullopt;
    }
    return *key;
}

/** The bytes of a text, as a span. */
std::span<const std::uint8_t> bytes_view(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

void p256_keys_derive_their_ids() {
    std::size_t keys = 0;
    for (const auto entry : section("p256_keys")) {
        const auto key = keypair_of(entry["d"]);
        if (!key) {
            continue;
        }
        const auto point = key->public_key();
        if (!BOOST_TEST(point.has_value())) {
            continue;
        }
        const auto uncompressed = point->uncompressed();
        BOOST_TEST(std::ranges::equal(uncompressed, bytes_of(entry["uncompressed"])));
        const auto compact = tp::compact_public_key(uncompressed);
        BOOST_TEST_EQ(compact, str(entry["compact"]));
        BOOST_TEST_EQ(tp::derive_peer_id(compact), str(entry["peer_id"]));
        BOOST_TEST_EQ(tp::derive_user_id(compact), str(entry["user_id"]));
        ++keys;
    }
    BOOST_TEST_EQ(keys, 5U);
}

void compact_keys_are_read_as_0_27_0_reads_them() {
    std::size_t cases = 0;
    for (const auto entry : section("compact_keys")) {
        const auto point = tp::parse_compact_public_key(str(entry["text"]));
        BOOST_TEST_EQ(point.has_value(), entry["text_valid"].value_or(false));
        const bool record = point && p256::public_key::make(*point).has_value();
        BOOST_TEST_EQ(record, entry["record"].value_or(false));
        ++cases;
    }
    BOOST_TEST_EQ(cases, 6U);
}

void user_identity_text_matches() {
    const auto entry = section("user_identity");
    const auto imported = trystero::import_user_identity(str(entry["text"]));
    if (!BOOST_TEST(imported.has_value())) {
        return;
    }
    BOOST_TEST_EQ(trystero::export_user_identity(*imported), str(entry["text"]));
    BOOST_TEST_EQ(imported->user_id(), str(entry["user_id"]));
    BOOST_TEST_EQ(imported->public_key(), str(p256_key_entry(str(entry["label"]))["compact"]));
    // The JSON form is the same key, and exports as the text.
    const auto from_json = trystero::import_user_identity(str(entry["json"]));
    if (BOOST_TEST(from_json.has_value())) {
        BOOST_TEST_EQ(from_json->user_id(), str(entry["user_id"]));
        BOOST_TEST_EQ(trystero::export_user_identity(*from_json), str(entry["text"]));
    }
    // The material is the JWK's.
    const auto material = tp::parse_user_key(str(entry["text"]));
    if (BOOST_TEST(material.has_value())) {
        BOOST_TEST_EQ(tp::to_base64url(material->d.bytes()), str(entry["jwk"]["d"]));
        BOOST_TEST_EQ(
            std::string(tp::compact_public_key(material->point)),
            std::string(str(entry["jwk"]["x"])) + "." + std::string(str(entry["jwk"]["y"])));
    }
}

void user_identity_imports_match() {
    std::size_t cases = 0;
    for (const auto entry : section("user_identity_imports")) {
        const auto imported = trystero::import_user_identity(str(entry["text"]));
        const bool accepted = entry["accepted"].value_or(false);
        if (!BOOST_TEST_EQ(imported.has_value(), accepted)) {
            BOOST_LIGHTWEIGHT_TEST_OSTREAM << "  case: " << str(entry["case"]) << "\n";
            continue;
        }
        if (accepted) {
            BOOST_TEST_EQ(imported->user_id(), str(entry["user_id"]));
        } else {
            BOOST_TEST(imported.error() == trystero::error::invalid_user_identity);
        }
        ++cases;
    }
    BOOST_TEST_EQ(cases, 12U);
}

/** A resource over `new` and `delete` that, as each block is freed, counts the blocks that still
 * hold a fixture key's scalar, as its 32 bytes or as its base64url text. */
class secret_watch final : public std::pmr::memory_resource {
public:
    secret_watch() {
        for (const auto entry : section("p256_keys")) {
            const std::string text(str(entry["d"]));
            const auto scalar = tp::from_base64url(text);
            if (BOOST_TEST(scalar.has_value()) && BOOST_TEST_EQ(scalar->size(), 32U)) {
                needles_.emplace_back(reinterpret_cast<const char*>(scalar->data()),
                                      scalar->size());
            }
            needles_.push_back(text);
        }
    }

    /** The blocks freed with a scalar in them so far. */
    [[nodiscard]] std::size_t hits() const { return hits_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        const std::string_view block(static_cast<const char*>(p), bytes);
        if (std::ranges::any_of(needles_, [&](const std::string& needle) {
                return block.find(needle) != std::string_view::npos;
            })) {
            ++hits_;
        }
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::vector<std::string> needles_;
    std::size_t hits_ = 0;
};

/** Makes a resource the default one while it lives. */
class default_resource_scope {
public:
    explicit default_resource_scope(std::pmr::memory_resource* r)
        : previous_(std::pmr::set_default_resource(r)) {}

    default_resource_scope(const default_resource_scope&) = delete;
    default_resource_scope& operator=(const default_resource_scope&) = delete;

    ~default_resource_scope() { std::pmr::set_default_resource(previous_); }

private:
    std::pmr::memory_resource* previous_;
};

// No buffer of trystero's that held a user key's scalar is freed with it: importing, parsing and
// exporting a key, accepted or refused after its d is decoded, through a resource that is also the
// default one, frees no block that holds a scalar of the fixture's keys.
void user_key_buffers_are_wiped() {
    // Static, so that it outlives any block allocated in it while it is the default resource.
    static secret_watch watch;
    // The watch sees a copy of a scalar's text freed as it is.
    {
        const std::pmr::string copy(str(section("user_identity")["jwk"]["d"]), &watch);
    }
    BOOST_TEST_EQ(watch.hits(), 1U);
    std::vector<std::string> texts;
    for (const auto entry : section("user_identity_imports")) {
        texts.emplace_back(str(entry["text"]));
    }
    texts.emplace_back(str(section("user_identity")["json"]));
    std::size_t accepted = 0;
    {
        const default_resource_scope scope(&watch);
        for (const auto& text : texts) {
            {
                const auto imported = trystero::import_user_identity(text, &watch);
                if (imported) {
                    ++accepted;
                    // The text is the caller's, which wipes it, as the function's doc asks.
                    auto exported = trystero::export_user_identity(*imported, &watch);
                    trystero::detail::wipe(exported.data(), exported.size());
                }
            }
            const auto material = tp::parse_user_key(text, &watch);
        }
        // A created identity, exported, then ended.
        const auto created = trystero::create_user_identity(&watch);
        if (BOOST_TEST(created.has_value())) {
            auto exported = trystero::export_user_identity(*created, &watch);
            trystero::detail::wipe(exported.data(), exported.size());
        }
    }
    BOOST_TEST_EQ(accepted, 4U);
    BOOST_TEST_EQ(watch.hits(), 1U);
}

// What 0.27.0's tryParsePrivateJwk reads beyond the fixture: String.prototype.trim's white space,
// the kty and crv it ignores when falsy, the byte order mark TextDecoder drops.
void user_key_text_is_read_as_0_27_0_reads_it() {
    const auto entry = section("user_identity");
    const std::string text(str(entry["text"]));
    const std::string json(str(entry["json"]));
    const std::string user_id(str(entry["user_id"]));
    const auto accepts = [&](std::string_view candidate) {
        const auto imported = trystero::import_user_identity(candidate);
        return imported.has_value() && imported->user_id() == user_id;
    };
    // U+00A0, U+FEFF, U+2028, U+3000, tab and vertical tab around the text.
    BOOST_TEST(accepts("\xc2\xa0\xef\xbb\xbf" + text + "\xe2\x80\xa8\xe3\x80\x80\t\v"));
    BOOST_TEST(accepts("\xe2\x80\x8a" + json + "\xe2\x81\x9f"));
    // U+200B is no white space of trim's.
    BOOST_TEST(!accepts("\xe2\x80\x8b" + text));
    const std::string members = json.substr(1);  // "d":...}
    BOOST_TEST(accepts(R"({"kty":"","crv":null,)" + members));
    BOOST_TEST(accepts(R"({"kty":0,"crv":false,)" + members));
    BOOST_TEST(!accepts(R"({"kty":1,)" + members));
    BOOST_TEST(!accepts(R"({"kty":"OKP",)" + members));
    BOOST_TEST(!accepts(R"({"crv":["P-256"],)" + members));
    // A member written twice keeps its last value, as JSON.parse keeps it.
    BOOST_TEST(accepts(R"({"d":"AAAA",)" + members));
    BOOST_TEST(!accepts(R"({"d":1,"x":2,"y":3})"));
    BOOST_TEST(!accepts("[" + json + "]"));
    // The prefixed text of the JSON with a byte order mark, which TextDecoder drops.
    const std::string bom_json = "\xef\xbb\xbf" + json;
    BOOST_TEST(accepts(std::string(tp::user_key_prefix) +
                       std::string(tp::to_base64url(bytes_view(bom_json)))));
    BOOST_TEST(!accepts(std::string(tp::user_key_prefix)));
    BOOST_TEST(!accepts("   "));
}

void user_proof_matches() {
    const auto entry = section("user_proof");
    const auto payload = tp::user_proof_payload(
        str(entry["app"]), str(entry["room"]), str(entry["peer_id"]), str(entry["peer_compact"]),
        integer(entry["seq"]), std::pmr::get_default_resource());
    BOOST_TEST_EQ(payload, str(entry["payload"]));
    BOOST_TEST_EQ(tp::to_hex(tp::sha256(payload)), str(entry["digest"]));
    const auto user_key = public_key_of(str(entry["user_compact"]));
    const auto proof = tp::from_base64url(str(entry["proof"]));
    if (!user_key || !BOOST_TEST(proof.has_value())) {
        return;
    }
    BOOST_TEST_EQ(tp::verify_p256(*proof, bytes_view(payload), *user_key),
                  entry["verifies"].value_or(false));
    const auto other_seq = tp::user_proof_payload(
        str(entry["app"]), str(entry["room"]), str(entry["peer_id"]), str(entry["peer_compact"]),
        integer(entry["seq"]) + 1, std::pmr::get_default_resource());
    BOOST_TEST_EQ(tp::verify_p256(*proof, bytes_view(other_seq), *user_key),
                  entry["other_seq_verifies"].value_or(true));
    // The port's own proof, by user-a's key, is low S and verifies.
    const auto user_a = p256_key_entry("user-a");
    BOOST_TEST_EQ(str(user_a["user_id"]), str(entry["user_id"]));
    const auto key = keypair_of(user_a["d"]);
    if (!key) {
        return;
    }
    const auto signed_payload = tp::sign_p256(*key, bytes_view(payload));
    if (BOOST_TEST(signed_payload.has_value())) {
        BOOST_TEST(tp::is_low_s(*signed_payload));
        BOOST_TEST(tp::verify_p256(*signed_payload, bytes_view(payload), *user_key));
        BOOST_TEST(!tp::verify_p256(*signed_payload, bytes_view(other_seq), *user_key));
    }
}

void low_s_matches() {
    const auto entry = section("user_proof");
    const auto payload = tp::user_proof_payload(
        str(entry["app"]), str(entry["room"]), str(entry["peer_id"]), str(entry["peer_compact"]),
        integer(entry["seq"]), std::pmr::get_default_resource());
    const auto user_key = public_key_of(str(entry["user_compact"]));
    if (!user_key) {
        return;
    }
    std::size_t cases = 0;
    std::size_t in_range = 0;
    for (const auto c : section("low_s")) {
        const auto signature = bytes_of(c["signature"]);
        BOOST_TEST_EQ(tp::is_low_s(signature), c["is_low_s"].value_or(true));
        BOOST_TEST_EQ(tp::verify_p256(signature, bytes_view(payload), *user_key),
                      c["verifies"].value_or(true));
        if (signature.size() == 64) {
            const auto normalized =
                p256::normalize_ecdsa(std::span<const std::uint8_t, 64>(signature.data(), 64));
            if (normalized) {
                BOOST_TEST(std::ranges::equal(*normalized, bytes_of(c["normalized"])));
                ++in_range;
            } else {
                // r or s is 0 or not below n: 0.27.0 leaves the signature as it is.
                BOOST_TEST(bytes_of(c["normalized"]) == signature);
            }
        }
        ++cases;
    }
    BOOST_TEST_EQ(cases, 8U);
    BOOST_TEST_EQ(in_range, 5U);
    // A signature of another length is never low S, as isLowSP256Signature says.
    BOOST_TEST(!tp::is_low_s(std::span<const std::uint8_t>{}));
}

void cpp_signatures_are_deterministic_and_low_s() {
    const auto key = keypair_of(p256_key_entry("endpoint-a")["d"]);
    if (!key) {
        return;
    }
    const auto point = key->public_key();
    if (!BOOST_TEST(point.has_value())) {
        return;
    }
    for (const std::string_view payload : {"", "payload", "trystero:identity-self-test:v1"}) {
        const auto first = tp::sign_p256(*key, bytes_view(payload));
        const auto second = tp::sign_p256(*key, bytes_view(payload));
        if (!BOOST_TEST(first.has_value()) || !BOOST_TEST(second.has_value())) {
            continue;
        }
        BOOST_TEST(*first == *second);  // RFC 6979
        BOOST_TEST(tp::is_low_s(*first));
        BOOST_TEST(tp::verify_p256(*first, bytes_view(payload), *point));
    }
    // A user identity signs as its key does.
    const auto identity = trystero::import_user_identity(str(section("user_identity")["text"]));
    if (BOOST_TEST(identity.has_value())) {
        const auto made = identity->sign(bytes_view("payload"));
        const auto again = identity->sign(bytes_view("payload"));
        BOOST_TEST(made.has_value() && again.has_value() && *made == *again);
        if (made) {
            BOOST_TEST(tp::verify_p256(*made, bytes_view("payload"),
                                       *public_key_of(identity->public_key())));
        }
    }
    // A created identity is fresh each time, and exports as a text that imports to it.
    const auto a = trystero::create_user_identity();
    const auto b = trystero::create_user_identity();
    if (BOOST_TEST(a.has_value()) && BOOST_TEST(b.has_value())) {
        BOOST_TEST_NE(a->user_id(), b->user_id());
        BOOST_TEST_EQ(a->user_id().size(), tp::user_id_size);
        const auto text = trystero::export_user_identity(*a);
        BOOST_TEST_EQ(text.size(), 214U);
        const auto again = trystero::import_user_identity(text);
        BOOST_TEST(again.has_value() && again->user_id() == a->user_id());
        // A move copies: the identity moved from keeps its key.
        auto moved = *a;
        const auto target = std::move(moved);
        // NOLINTBEGIN(bugprone-use-after-move): an identity's move is its copy
        BOOST_TEST_EQ(moved.user_id(), target.user_id());
        BOOST_TEST(moved.sign(bytes_view("payload")).has_value());
        // NOLINTEND(bugprone-use-after-move)
    }
}

void scalars_are_checked_against_the_order() {
    std::array<std::uint8_t, 32> scalar{};
    BOOST_TEST(!tp::is_p256_scalar(scalar));  // zero
    scalar.back() = 1;
    BOOST_TEST(tp::is_p256_scalar(scalar));
    scalar = tp::p256_order;
    BOOST_TEST(!tp::is_p256_scalar(scalar));  // n
    scalar.back() -= 1;
    BOOST_TEST(tp::is_p256_scalar(scalar));  // n - 1
    scalar.fill(0xff);
    BOOST_TEST(!tp::is_p256_scalar(scalar));
}

// The identity handshake: its messages, transcript and each side's run, against 0.27.0's record.

/** The fingerprints of a description, in the default resource. */
std::pmr::vector<std::pmr::string> dtls_of(std::string_view sdp) {
    return tp::dtls_fingerprints(sdp, std::pmr::get_default_resource());
}

/** The texts of a fixture's array of strings. */
std::pmr::vector<std::pmr::string> strings_of(const fixture_value& array) {
    std::pmr::vector<std::pmr::string> out;
    for (const auto item : array) {
        out.emplace_back(str(item));
    }
    return out;
}

/** A recorded step's members, as the test reads them from its JSON (not through the reader). */
tp::handshake_claims claims_of(std::string_view json) {
    tp::handshake_claims claims;
    const auto parsed = wd::parse(json);
    if (!BOOST_TEST(parsed.has_value())) {
        return claims;
    }
    const fixture_value root = parsed->root();
    claims.step = static_cast<int>(root[tp::handshake_step_key].value_or(std::int64_t{0}));
    claims.nonce = str(root["n"]);
    claims.dtls = strings_of(root["dtls"]);
    claims.user_id = str(root["uid"]);
    claims.endpoint_key = str(root["pk"]);
    claims.user_key = str(root["uk"]);
    claims.endpoint_signature = str(root["ps"]);
    claims.user_signature = str(root["us"]);
    return claims;
}

/** The recorded message of a case with `step`, or an empty text. */
std::string_view message_of(const fixture_value& c, int step) {
    for (const auto m : c["messages"]) {
        if (m["step"].value_or(std::int64_t{0}) == step) {
            return str(m["json"]);
        }
    }
    return {};
}

/** The 16 random bytes `uuid_v4` turns into a recorded nonce. */
std::array<std::uint8_t, 16> nonce_random_of(std::string_view uuid) {
    std::string hex;
    std::ranges::copy_if(uuid, std::back_inserter(hex), [](char ch) { return ch != '-'; });
    const auto decoded = tp::from_hex(hex);
    std::array<std::uint8_t, 16> out{};
    if (BOOST_TEST(decoded.has_value() && decoded->size() == 16)) {
        std::ranges::copy(*decoded, out.begin());
    }
    return out;
}

/** The scalar of a fixture key's `d`. */
tp::bytes scalar_of(std::string_view label) {
    const auto scalar = tp::from_base64url(str(p256_key_entry(label)["d"]));
    BOOST_TEST(scalar.has_value());
    return scalar.value_or(tp::bytes());
}

/** One side of a recorded handshake as the C++ side runs it: its keys, cache and machine. */
struct handshake_side {
    trystero::detail::endpoint_identity endpoint;
    trystero::user_identity user;
    trystero::detail::identity_cache cache{std::pmr::get_default_resource()};
    std::pmr::vector<std::pmr::string> local_dtls;
    std::pmr::vector<std::pmr::string> remote_dtls;
    std::optional<trystero::detail::identity_handshake> machine{};
};

/** The endpoint identity of a fixture key. */
trystero::detail::endpoint_identity endpoint_of(std::string_view label) {
    auto made = trystero::detail::make_endpoint_identity(scalar_of(label),
                                                         std::pmr::get_default_resource());
    BOOST_TEST(made.has_value());
    return std::move(made.value());
}

/** The user identity of a fixture key. */
trystero::user_identity user_of(std::string_view label) {
    auto made = trystero::detail::user_identity_access::make(scalar_of(label),
                                                             std::pmr::get_default_resource());
    BOOST_TEST(made.has_value());
    return made.value();
}

/** Makes the C++ side of `role` ("initiator" or "responder") of a recorded case. */
std::unique_ptr<handshake_side> side_of(const fixture_value& c, std::string_view role) {
    const bool initiator = role == "initiator";
    const auto self = c[role];
    const auto remote = c[initiator ? "responder" : "initiator"];
    auto side = std::unique_ptr<handshake_side>(new handshake_side{
        .endpoint = endpoint_of(str(self["endpoint"])),
        .user = user_of(str(self["user"])),
        .local_dtls = dtls_of(str(self["sdp_local"])),
        .remote_dtls = dtls_of(str(self["sdp_remote"])),
    });
    BOOST_TEST_EQ(side->endpoint.peer_id, str(self["peer_id"]));
    BOOST_TEST_EQ(side->user.user_id(), str(self["user_id"]));
    side->machine.emplace(
        trystero::detail::local_identity{
            .endpoint = side->endpoint.key,
            .peer_id = side->endpoint.peer_id,
            .endpoint_key = side->endpoint.compact,
            .user = side->user,
        },
        side->cache, str(c["app"]), str(c["room"]), str(remote["peer_id"]), initiator,
        side->local_dtls, side->remote_dtls, nonce_random_of(str(c["nonces"][initiator ? 0 : 1])),
        std::pmr::get_default_resource());
    return side;
}

/** The recorded case named `name`. */
fixture_value handshake_case(std::string_view name) {
    for (const auto c : section("handshake")) {
        if (str(c["case"]) == name) {
            return c;
        }
    }
    BOOST_ERROR("handshake has no such case");
    return section("handshake")[0];
}

/** Whether `signature` (base64url) is `compact`'s, of `payload`, through an identity cache. */
bool verifies(std::string_view compact, std::string_view signature, std::string_view payload) {
    trystero::detail::identity_cache cache(std::pmr::get_default_resource());
    const auto bytes = tp::from_base64url(signature);
    return bytes && cache.verify(compact, *bytes, bytes_view(payload));
}

void dtls_fingerprints_match() {
    std::size_t sent = 0;
    for (const auto c : section("handshake")) {
        for (const auto m : c["messages"]) {
            const auto step = m["step"].value_or(std::int64_t{0});
            if (step == 3) {
                continue;
            }
            const auto role = str(m["from"]) == "A" ? "initiator" : "responder";
            BOOST_TEST(dtls_of(str(c[role]["sdp_local"])) == claims_of(str(m["json"])).dtls);
            ++sent;
        }
    }
    BOOST_TEST_EQ(sent, 11U);
    // The rule itself: lines that start "a=fingerprint:" (a line starts after \n, \r, U+2028 or
    // U+2029), the value to the line's \r or \n, trimmed, lowercased, deduplicated, in UTF-16
    // order.
    using list = std::pmr::vector<std::pmr::string>;
    BOOST_TEST(dtls_of("") == list{});
    BOOST_TEST(dtls_of("a=fingerprint:") == list{});
    BOOST_TEST(dtls_of("a=fingerprint:   \r\n") == list{});
    BOOST_TEST(dtls_of(" a=fingerprint:x\r\nb a=fingerprint:y") == list{});
    BOOST_TEST(dtls_of("a=fingerprint:B\na=fingerprint: b \r\ra=fingerprint:A") ==
               (list{"a", "b"}));
    BOOST_TEST(dtls_of("v=0\xe2\x80\xa8"
                       "a=fingerprint:x\xe2\x80\xa9"
                       "a=fingerprint:y\r\n") == (list{"x\xe2\x80\xa9"
                                                       "a=fingerprint:y"}));
    BOOST_TEST(dtls_of("x\xe2\x80\xa8"
                       "a=fingerprint:z") == (list{"z"}));
    // U+FF01 is after U+1F600 in UTF-16 (0xff01 > 0xd83d), before it in UTF-8.
    BOOST_TEST(dtls_of("a=fingerprint:\xef\xbc\x81\na=fingerprint:\xf0\x9f\x98\x80") ==
               (list{"\xf0\x9f\x98\x80", "\xef\xbc\x81"}));
    // JavaScript's trim removes U+00A0 and U+3000 too.
    BOOST_TEST(dtls_of("a=fingerprint:\xc2\xa0Sha-256 AB\xe3\x80\x80") == (list{"sha-256 ab"}));
}

void handshake_steps_match() {
    const auto c = handshake_case("verified");
    std::size_t steps = 0;
    for (const auto m : c["messages"]) {
        const auto json = str(m["json"]);
        BOOST_TEST_EQ(tp::write_handshake_step(claims_of(json), std::pmr::get_default_resource()),
                      json);
        ++steps;
    }
    BOOST_TEST_EQ(steps, 3U);
    // The frames' payloads are these steps.
    BOOST_TEST_EQ(str(section("handshake_frames")[0]["payload_json"]), message_of(c, 1));
    // Each step reads back as it was written, under the descriptions the recipient applied.
    const auto a_dtls = dtls_of(str(c["initiator"]["sdp_local"]));
    const auto b_dtls = dtls_of(str(c["responder"]["sdp_local"]));
    for (int step = 1; step <= 3; ++step) {
        const auto json = message_of(c, step);
        const bool from_b = step == 2;
        const auto read =
            tp::read_handshake_step(json, step, from_b ? b_dtls : a_dtls, from_b ? a_dtls : b_dtls,
                                    std::pmr::get_default_resource());
        if (BOOST_TEST(read.claims.has_value())) {
            BOOST_TEST(read.failure.empty());
            BOOST_TEST_EQ(tp::write_handshake_step(*read.claims, std::pmr::get_default_resource()),
                          json);
        }
    }
}

void handshake_steps_are_read_as_0_27_0_reads_them() {
    const auto c = handshake_case("verified");
    const auto a_dtls = dtls_of(str(c["initiator"]["sdp_local"]));
    const auto b_dtls = dtls_of(str(c["responder"]["sdp_local"]));
    auto* memory = std::pmr::get_default_resource();
    const auto read = [&](std::string_view json, int step) {
        return tp::read_handshake_step(json, step, step == 2 ? b_dtls : a_dtls,
                                       step == 2 ? a_dtls : b_dtls, memory);
    };
    // A step 1 with one member replaced.
    const auto step1 = [&](std::string_view member, std::string_view value) {
        auto claims = claims_of(message_of(c, 1));
        std::pmr::string json = tp::write_handshake_step(claims, memory);
        const std::string key = "\"" + std::string(member) + "\":";
        const auto at = json.find(key);
        BOOST_TEST(at != std::string::npos);
        auto end = at + key.size();
        int depth = 0;
        bool quoted = false;
        for (; end < json.size(); ++end) {
            const char ch = json[end];
            if (quoted) {
                quoted = ch != '"';
                continue;
            }
            if (ch == '"') {
                quoted = true;
            } else if (ch == '[') {
                ++depth;
            } else if (ch == ']') {
                --depth;
            } else if ((ch == ',' || ch == '}') && depth == 0) {
                break;
            }
        }
        json.replace(at + key.size(), end - at - key.size(), value);
        return json;
    };
    const auto identity = tp::handshake_reason::identity;
    const auto mismatch = tp::handshake_reason::dtls_mismatch;
    BOOST_TEST(read(message_of(c, 1), 1).claims.has_value());
    for (const std::string_view json : {"", "null", "1", "\"x\"", "[]", "{", "{}"}) {
        BOOST_TEST_EQ(read(json, 1).failure, identity);
    }
    BOOST_TEST_EQ(read(message_of(c, 1), 2).failure, identity);  // another step
    BOOST_TEST_EQ(read(step1("__trystero_id", "\"1\""), 1).failure, identity);
    BOOST_TEST(read(step1("__trystero_id", "1.0"), 1).claims.has_value());
    BOOST_TEST(read(step1("__trystero_id", "1e0"), 1).claims.has_value());
    BOOST_TEST_EQ(read(step1("n", "123456789012345"), 1).failure, identity);
    BOOST_TEST_EQ(read(step1("n", "\"012345678901234\""), 1).failure, identity);
    BOOST_TEST(read(step1("n", "\"0123456789012345\""), 1).claims.has_value());
    BOOST_TEST_EQ(read(step1("uid", "\"\""), 1).failure, identity);
    BOOST_TEST_EQ(read(step1("uid", "7"), 1).failure, identity);
    BOOST_TEST(read(step1("pk", "\"\""), 1).claims.has_value());  // the key checks are the cache's
    BOOST_TEST_EQ(read(step1("uk", "null"), 1).failure, identity);
    BOOST_TEST_EQ(read(step1("dtls", "\"x\""), 1).failure, identity);
    // The DTLS rule: what the description gives, or a list when this side has one.
    std::string dtls_with_extras = "[1,null,{\"a\":[]},";
    for (std::size_t i = 0; i < a_dtls.size(); ++i) {
        dtls_with_extras += (i == 0 ? "\"" : ",true,\"") + std::string(a_dtls[i]) + "\"";
    }
    dtls_with_extras += ",false]";
    const auto extras = read(step1("dtls", dtls_with_extras), 1);
    if (BOOST_TEST(extras.claims.has_value())) {
        BOOST_TEST(extras.claims->dtls == a_dtls);  // the non-strings dropped
    }
    BOOST_TEST_EQ(read(step1("dtls", "[]"), 1).failure, mismatch);
    BOOST_TEST_EQ(read(step1("dtls", "[\"sha-256 00\"]"), 1).failure, mismatch);
    // Lists are compared as 0.27.0 compares them, joined with commas.
    const std::string joined =
        "[\"" + std::string(a_dtls[0]) + "," + std::string(a_dtls[1]) + "\"]";
    BOOST_TEST(read(step1("dtls", joined), 1).claims.has_value());
    // Without a description, the claimed list is taken; without a local list, an empty one too.
    BOOST_TEST(tp::read_handshake_step(step1("dtls", "[\"sha-256 00\"]"), 1, {}, b_dtls, memory)
                   .claims.has_value());
    BOOST_TEST_EQ(tp::read_handshake_step(step1("dtls", "[]"), 1, {}, b_dtls, memory).failure,
                  mismatch);
    BOOST_TEST(tp::read_handshake_step(step1("dtls", "[]"), 1, {}, {}, memory).claims.has_value());
    // Step 2 needs both signatures as strings; step 3 is its signatures alone.
    auto step2 = claims_of(message_of(c, 2));
    BOOST_TEST(read(message_of(c, 2), 2).claims.has_value());
    step2.step = 1;  // written without its signatures
    auto unsigned_step2 = tp::write_handshake_step(step2, memory);
    unsigned_step2.replace(unsigned_step2.find(":1,"), 3, ":2,");
    BOOST_TEST_EQ(read(unsigned_step2, 2).failure, identity);
    BOOST_TEST_EQ(read("{\"__trystero_id\":3,\"ps\":\"a\"}", 3).failure, identity);
    BOOST_TEST_EQ(read("{\"__trystero_id\":2,\"ps\":\"a\",\"us\":\"b\"}", 3).failure, identity);
    const auto step3 = read("{\"__trystero_id\":3,\"ps\":\"a\",\"us\":\"\"}", 3);
    if (BOOST_TEST(step3.claims.has_value())) {
        BOOST_TEST_EQ(step3.claims->endpoint_signature, std::string_view("a"));
        BOOST_TEST(step3.claims->user_signature.empty());
    }
}

void handshake_transcript_matches() {
    for (const auto c : section("handshake")) {
        if (!c["transcript"].is_string()) {
            continue;
        }
        const auto transcript = tp::handshake_transcript(
            str(c["app"]), str(c["room"]), str(c["initiator"]["peer_id"]),
            str(c["responder"]["peer_id"]), str(c["nonces"][0]), str(c["nonces"][1]),
            dtls_of(str(c["initiator"]["sdp_local"])), dtls_of(str(c["responder"]["sdp_local"])),
            str(c["initiator"]["user_id"]), str(c["responder"]["user_id"]),
            std::pmr::get_default_resource());
        BOOST_TEST_EQ(transcript, str(c["transcript"]));
        BOOST_TEST_EQ(tp::to_hex(tp::sha256(transcript)), str(c["transcript_digest"]));
    }
}

void js_handshake_signatures_verify() {
    const auto c = handshake_case("verified");
    const auto transcript = str(c["transcript"]);
    const auto step2 = claims_of(message_of(c, 2));
    const auto step3 = claims_of(message_of(c, 3));
    const auto compact = [](std::string_view label) {
        return str(p256_key_entry(label)["compact"]);
    };
    BOOST_TEST_EQ(verifies(compact("endpoint-b"), step2.endpoint_signature, transcript),
                  c["transcript_verified"]["step2_endpoint"].value_or(false));
    BOOST_TEST_EQ(verifies(compact("user-b"), step2.user_signature, transcript),
                  c["transcript_verified"]["step2_user"].value_or(false));
    BOOST_TEST(verifies(compact("endpoint-a"), step3.endpoint_signature, transcript));
    BOOST_TEST(verifies(compact("user-a"), step3.user_signature, transcript));
    // Of another key, or of another transcript, they do not.
    BOOST_TEST(!verifies(compact("endpoint-a"), step2.endpoint_signature, transcript));
    BOOST_TEST(!verifies(compact("user-b"), step3.user_signature, transcript));
    BOOST_TEST(
        !verifies(compact("endpoint-b"), step2.endpoint_signature, std::string(transcript) + " "));
}

void cpp_responder_meets_the_js_initiator() {
    const auto c = handshake_case("verified");
    const auto b = side_of(c, "responder");
    const auto started = b->machine->start();
    BOOST_TEST(started.send.empty());
    BOOST_TEST(!started.done.has_value());
    BOOST_TEST(started.failure.empty());
    const auto stepped = b->machine->receive(message_of(c, 1));
    BOOST_TEST(stepped.failure.empty());
    BOOST_TEST(!stepped.done.has_value());
    // Step 2 is the recorded one but for its signatures, RFC 6979's where WebCrypto's are random.
    const auto sent = claims_of(stepped.send);
    auto expected = claims_of(message_of(c, 2));
    expected.endpoint_signature = sent.endpoint_signature;
    expected.user_signature = sent.user_signature;
    BOOST_TEST_EQ(stepped.send,
                  tp::write_handshake_step(expected, std::pmr::get_default_resource()));
    BOOST_TEST(verifies(b->endpoint.compact, sent.endpoint_signature, str(c["transcript"])));
    BOOST_TEST(verifies(b->user.public_key(), sent.user_signature, str(c["transcript"])));
    const auto finished = b->machine->receive(message_of(c, 3));
    BOOST_TEST(finished.failure.empty());
    BOOST_TEST(finished.send.empty());
    if (BOOST_TEST(finished.done.has_value())) {
        BOOST_TEST_EQ(finished.done->user_id, str(c["verified"]["B"]["user_id"]));
        BOOST_TEST_EQ(finished.done->endpoint_key, str(p256_key_entry("endpoint-a")["compact"]));
        BOOST_TEST_EQ(finished.done->user_key, str(p256_key_entry("user-a")["compact"]));
    }
    // A finished run takes nothing more.
    const auto after = b->machine->receive(message_of(c, 3));
    BOOST_TEST(after.send.empty() && !after.done && after.failure.empty());
}

void cpp_initiator_meets_the_js_responder() {
    const auto c = handshake_case("verified");
    const auto a = side_of(c, "initiator");
    // Before its start, a run takes nothing.
    const auto early = a->machine->receive(message_of(c, 2));
    BOOST_TEST(early.send.empty() && !early.done && early.failure.empty());
    const auto started = a->machine->start();
    BOOST_TEST_EQ(started.send, message_of(c, 1));
    BOOST_TEST(!started.done.has_value());
    BOOST_TEST(started.failure.empty());
    const auto finished = a->machine->receive(message_of(c, 2));
    BOOST_TEST(finished.failure.empty());
    if (BOOST_TEST(finished.done.has_value())) {
        BOOST_TEST_EQ(finished.done->user_id, str(c["verified"]["A"]["user_id"]));
        BOOST_TEST_EQ(finished.done->endpoint_key, str(p256_key_entry("endpoint-b")["compact"]));
        BOOST_TEST_EQ(finished.done->user_key, str(p256_key_entry("user-b")["compact"]));
    }
    const auto step3 = claims_of(finished.send);
    BOOST_TEST_EQ(step3.step, 3);
    BOOST_TEST_EQ(finished.send, tp::write_handshake_step(step3, std::pmr::get_default_resource()));
    BOOST_TEST(verifies(a->endpoint.compact, step3.endpoint_signature, str(c["transcript"])));
    BOOST_TEST(verifies(a->user.public_key(), step3.user_signature, str(c["transcript"])));
}

void handshake_failures_match() {
    std::size_t failing = 0;
    std::size_t waiting = 0;
    std::size_t verified = 0;
    for (const auto c : section("handshake")) {
        if (str(c["case"]) == "verified") {
            continue;
        }
        for (const std::string_view role : {"initiator", "responder"}) {
            const auto error = c[std::string(role) + "_error"];
            const auto side = side_of(c, role);
            const std::string_view own = role == "initiator" ? "A" : "B";
            std::vector<trystero::detail::identity_handshake::outcome> outcomes;
            outcomes.push_back(side->machine->start());
            for (const auto m : c["messages"]) {
                if (!outcomes.back().failure.empty()) {
                    break;
                }
                if (str(m["from"]) != own) {
                    outcomes.push_back(side->machine->receive(str(m["json"])));
                }
            }
            const auto failed =
                std::ranges::find_if(outcomes, [](const auto& o) { return !o.failure.empty(); });
            const auto done =
                std::ranges::find_if(outcomes, [](const auto& o) { return o.done.has_value(); });
            if (error.is_null()) {
                // The side 0.27.0 verified verifies too.
                BOOST_TEST(failed == outcomes.end());
                if (BOOST_TEST(done != outcomes.end())) {
                    BOOST_TEST_EQ(done->done->user_id, str(c["verified"][own]["user_id"]));
                }
                ++verified;
            } else if (str(error) == "waits") {
                BOOST_TEST(failed == outcomes.end());
                BOOST_TEST(done == outcomes.end());
                ++waiting;
            } else {
                std::string_view expected = str(error);
                BOOST_TEST(expected.starts_with("Trystero: "));
                expected.remove_prefix(std::string_view("Trystero: ").size());
                if (BOOST_TEST(failed != outcomes.end())) {
                    BOOST_TEST_EQ(failed->failure, expected);
                }
                BOOST_TEST(done == outcomes.end());
                ++failing;
            }
        }
    }
    BOOST_TEST_EQ(failing, 7U);
    BOOST_TEST_EQ(waiting, 6U);
    BOOST_TEST_EQ(verified, 1U);
}

void uuid_nonces_have_16_units_or_more() {
    const auto c = handshake_case("verified");
    const auto a_dtls = dtls_of(str(c["initiator"]["sdp_local"]));
    const auto b_dtls = dtls_of(str(c["responder"]["sdp_local"]));
    auto* memory = std::pmr::get_default_resource();
    auto claims = claims_of(message_of(c, 1));
    const auto accepted = [&](std::string_view nonce) {
        claims.nonce = nonce;
        return tp::read_handshake_step(tp::write_handshake_step(claims, memory), 1, a_dtls, b_dtls,
                                       memory)
            .claims.has_value();
    };
    for (const std::uint8_t fill : {0x00, 0x5a, 0xff}) {
        std::array<std::uint8_t, 16> random{};
        random.fill(fill);
        const auto uuid = tp::uuid_v4(random);
        BOOST_TEST_GE(uuid.size(), tp::min_nonce_units);
        BOOST_TEST(accepted(std::string_view(uuid.data(), uuid.size())));
    }
    // Counted in UTF-16 code units, as `n.length`: 15 accented letters are 30 bytes but 15 units;
    // 8 emoji are 16 units.
    std::string accented;
    std::string emoji;
    for (int i = 0; i < 15; ++i) {
        accented += "\xc3\xa9";
    }
    for (int i = 0; i < 8; ++i) {
        emoji += "\xf0\x9f\x98\x80";
    }
    BOOST_TEST(!accepted(accented));
    BOOST_TEST(accepted(accented + "x"));
    BOOST_TEST(accepted(emoji));
    BOOST_TEST(!accepted(std::string(15, 'x')));
    BOOST_TEST(accepted(std::string(16, 'x')));
}

/** A failed start: the run reports `dtls_missing` when a description has no fingerprint. */
void a_run_without_fingerprints_fails_at_its_start() {
    const auto c = handshake_case("verified");
    for (const std::string_view role : {"initiator", "responder"}) {
        for (const bool local : {true, false}) {
            auto side = side_of(c, role);
            (local ? side->local_dtls : side->remote_dtls).clear();
            side->machine.emplace(
                trystero::detail::local_identity{
                    .endpoint = side->endpoint.key,
                    .peer_id = side->endpoint.peer_id,
                    .endpoint_key = side->endpoint.compact,
                    .user = side->user,
                },
                side->cache, str(c["app"]), str(c["room"]),
                str(c[role == "initiator" ? "responder" : "initiator"]["peer_id"]),
                role == "initiator", side->local_dtls, side->remote_dtls,
                std::array<std::uint8_t, 16>{}, std::pmr::get_default_resource());
            const auto started = side->machine->start();
            BOOST_TEST_EQ(started.failure, tp::handshake_reason::dtls_missing);
            BOOST_TEST(started.send.empty());
            // A failed run takes nothing more.
            const auto after = side->machine->receive(message_of(c, role == "initiator" ? 2 : 1));
            BOOST_TEST(after.send.empty() && !after.done && after.failure.empty());
        }
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
    webcpp::test::run_case(topic_kind_matches);
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
    webcpp::test::run_case(wipe_zeroes_what_it_is_given);
    webcpp::test::run_case(signal_aad_matches);
    webcpp::test::run_case(action_frames_match);
    webcpp::test::run_case(handshake_frames_match);
    webcpp::test::run_case(p256_keys_derive_their_ids);
    webcpp::test::run_case(compact_keys_are_read_as_0_27_0_reads_them);
    webcpp::test::run_case(user_identity_text_matches);
    webcpp::test::run_case(user_identity_imports_match);
    webcpp::test::run_case(user_key_buffers_are_wiped);
    webcpp::test::run_case(user_key_text_is_read_as_0_27_0_reads_it);
    webcpp::test::run_case(user_proof_matches);
    webcpp::test::run_case(low_s_matches);
    webcpp::test::run_case(cpp_signatures_are_deterministic_and_low_s);
    webcpp::test::run_case(scalars_are_checked_against_the_order);
    webcpp::test::run_case(dtls_fingerprints_match);
    webcpp::test::run_case(handshake_steps_match);
    webcpp::test::run_case(handshake_steps_are_read_as_0_27_0_reads_them);
    webcpp::test::run_case(handshake_transcript_matches);
    webcpp::test::run_case(js_handshake_signatures_verify);
    webcpp::test::run_case(cpp_responder_meets_the_js_initiator);
    webcpp::test::run_case(cpp_initiator_meets_the_js_responder);
    webcpp::test::run_case(handshake_failures_match);
    webcpp::test::run_case(uuid_nonces_have_16_units_or_more);
    webcpp::test::run_case(a_run_without_fingerprints_fails_at_its_start);
    return boost::report_errors();
}
