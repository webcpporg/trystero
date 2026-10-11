// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the protocol layer against values Trystero JS 0.26 computed itself, which the original's
// oracle wrote into fixtures/expected/expected.json. The fixture's path is the test's one argument,
// as webcpp.run passes its input file.

#include <webcpp/trystero/protocol/action_wire.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>
#include <webcpp/trystero/protocol/nostr.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace json = boost::json;
namespace tp = webcpp::trystero::protocol;

namespace {

json::value load(const char* path) {
    const std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return json::parse(text.str());
}

std::string_view str(const json::value& v) {
    return v.as_string();
}

tp::bytes hex(const json::value& v) {
    return *tp::from_hex(v.as_string());
}

void topics(const json::object& fixture) {
    for (const auto& entry : fixture.at("topics").as_array()) {
        const auto& t = entry.as_object();
        const auto app = str(t.at("app_id"));
        const auto room = str(t.at("room_id"));
        BOOST_TEST_EQ(tp::root_topic(app, room), str(t.at("root_topic")));
        BOOST_TEST_EQ(tp::peer_topic(app, room, str(t.at("peer_id"))), str(t.at("self_topic")));
        BOOST_TEST_EQ(tp::room_token(app, room), str(t.at("room_token")));
    }
    for (const auto& entry : fixture.at("str_to_num").as_array()) {
        const auto& t = entry.as_object();
        BOOST_TEST_EQ(tp::str_to_num(str(t.at("text"))), t.at("unbounded").to_number<double>());
        BOOST_TEST_EQ(tp::str_to_num(str(t.at("text")), 10000),
                      t.at("mod_10000").to_number<double>());
    }
}

void relays(const json::object& fixture) {
    std::vector<std::string_view> defaults;
    for (const auto& url : fixture.at("default_relays").as_array()) {
        defaults.push_back(url.as_string());
    }
    BOOST_TEST(std::ranges::equal(defaults, tp::default_nostr_relays));
    for (const auto& entry : fixture.at("relays").as_array()) {
        std::vector<std::string_view> expected;
        for (const auto& url : entry.at("relays").as_array()) {
            expected.push_back(url.as_string());
        }
        const auto derived =
            tp::derive_relays(str(entry.at("app_id")), tp::default_nostr_relays, 5);
        BOOST_TEST_ALL_EQ(derived.begin(), derived.end(), expected.begin(), expected.end());
    }
    for (const auto& entry : fixture.at("shuffle").as_array()) {
        std::vector<int> order(29);
        for (int i = 0; i < 29; ++i) {
            order[i] = i;
        }
        std::vector<int> expected;
        for (const auto& i : entry.at("order").as_array()) {
            expected.push_back(static_cast<int>(i.as_int64()));
        }
        const auto shuffled = tp::shuffle(order, entry.at("seed").to_number<double>());
        BOOST_TEST_ALL_EQ(shuffled.begin(), shuffled.end(), expected.begin(), expected.end());
    }
}

void handshake_and_sealing(const json::object& fixture) {
    for (const auto& entry : fixture.at("handshake").as_array()) {
        BOOST_TEST_EQ(tp::handshake_proof(str(entry.at("challenge")), str(entry.at("password")),
                                          str(entry.at("app_id")), str(entry.at("room_id"))),
                      str(entry.at("hash")));
    }
    const auto& sealed = fixture.at("sealed").as_object();
    BOOST_TEST_EQ(tp::to_hex(tp::seal_key("secret", "my-app", "room-1")),
                  str(sealed.at("key_material")));
    const auto parts = tp::parse_sealed(str(sealed.at("text")));
    BOOST_TEST(parts.has_value());
    BOOST_TEST_EQ(parts->iv.size(), tp::seal_iv_size);
    BOOST_TEST_EQ(parts->ciphertext.size(), str(sealed.at("plaintext")).size() + 16);
    BOOST_TEST_EQ(tp::format_sealed(parts->iv, parts->ciphertext), str(sealed.at("text")));
    BOOST_TEST(!tp::parse_sealed("1,2,300$AAAA").has_value());
    BOOST_TEST(!tp::parse_sealed("no dollar").has_value());
}

void nostr_events(const json::object& fixture) {
    for (const auto& entry : fixture.at("nostr_events").as_array()) {
        const auto& e = entry.as_object();
        BOOST_TEST_EQ(tp::nostr::topic_kind(str(e.at("topic"))), e.at("kind").as_int64());
        const auto id = tp::nostr::event_id(str(e.at("pubkey")), e.at("created_at").as_int64(),
                                            static_cast<int>(e.at("kind").as_int64()),
                                            str(e.at("topic")), str(e.at("content")));
        BOOST_TEST_EQ(tp::to_hex(id), str(e.at("id")));
    }
    // A signed event verifies, and a relay's echo of it parses back to its topic and content.
    std::array<std::uint8_t, 32> secret{};
    secret.fill(7);
    const auto key = tp::nostr::signer::from_secret(secret);
    BOOST_TEST(key.has_value());
    std::array<std::uint8_t, 32> aux{};
    const auto message =
        tp::nostr::event_message(*key, 1759700000, "topic-a", R"({"peerId":"p"})", aux);
    const auto parsed = json::parse(message).as_array();
    auto event = parsed.at(1).as_object();
    const auto id = *tp::from_hex(event.at("id").as_string());
    const auto sig = *tp::from_hex(event.at("sig").as_string());
    const auto pub = *tp::from_hex(event.at("pubkey").as_string());
    BOOST_TEST(tp::nostr::verify(std::span<const std::uint8_t, 64>(sig.data(), 64),
                                 std::span<const std::uint8_t, 32>(id.data(), 32),
                                 std::span<const std::uint8_t, 32>(pub.data(), 32)));
    const auto echoed_document = json::value(json::array{"EVENT", "sub", event});
    const auto echoed = tp::nostr::read_relay_message(echoed_document);
    BOOST_TEST(echoed.has_value());
    const auto& e = std::get<tp::nostr::event>(*echoed);
    BOOST_TEST_EQ(e.subscription, "sub");
    BOOST_TEST_EQ(e.topic, "topic-a");
    BOOST_TEST_EQ(e.content, R"({"peerId":"p"})");
    const auto rejected_document = json::parse(R"(["OK","abc",false,"rate-limited: slow down"])");
    const auto rejected = tp::nostr::read_relay_message(rejected_document);
    BOOST_TEST_EQ(std::get<tp::nostr::ok>(*rejected).reason, "rate-limited: slow down");
    BOOST_TEST(!std::get<tp::nostr::ok>(*rejected).accepted);
}

void inline_frames(const json::object& fixture) {
    const auto& actions = fixture.at("actions").as_object();
    const auto encode = [](std::string_view name, tp::payload_format format,
                           std::string_view metadata, std::string_view payload) {
        return tp::encode_inline(
            name, format, metadata,
            std::span(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
    };
    const auto text = encode("chat", tp::payload_format::text, "", "hello from Trystero JS");
    const auto expected_text = hex(actions.at("text").at(0));
    BOOST_TEST_ALL_EQ(text.begin(), text.end(), expected_text.begin(), expected_text.end());
    const auto json_frame = encode("drink", tp::payload_format::json, "",
                                   R"({"drink":"negroni","withIce":true,"n":[1,2.5,null]})");
    const auto expected_json = hex(actions.at("json").at(0));
    BOOST_TEST_ALL_EQ(json_frame.begin(), json_frame.end(), expected_json.begin(),
                      expected_json.end());
    const tp::bytes binary{0, 1, 2, 253, 254, 255};
    const auto binary_frame = tp::encode_inline("pic", tp::payload_format::binary, "", binary);
    const auto expected_binary = hex(actions.at("binary").at(0));
    BOOST_TEST_ALL_EQ(binary_frame.begin(), binary_frame.end(), expected_binary.begin(),
                      expected_binary.end());
    const auto with_metadata =
        encode("chat", tp::payload_format::text, R"({"from":"js","n":7})", "hi");
    const auto expected_with_metadata = hex(actions.at("text_with_metadata").at(0));
    BOOST_TEST_ALL_EQ(with_metadata.begin(), with_metadata.end(), expected_with_metadata.begin(),
                      expected_with_metadata.end());
    const auto empty = encode("@_hsready", tp::payload_format::text, "", "");
    const auto expected_empty = hex(actions.at("empty_text").at(0));
    BOOST_TEST_ALL_EQ(empty.begin(), empty.end(), expected_empty.begin(), expected_empty.end());
    const auto long_name =
        encode("abcdefghijklmnopqrstuvwxyz012345", tp::payload_format::text, "", "x");
    const auto expected_long_name = hex(actions.at("thirty_two_byte_name").at(0));
    BOOST_TEST_ALL_EQ(long_name.begin(), long_name.end(), expected_long_name.begin(),
                      expected_long_name.end());

    const auto frame = hex(actions.at("text_with_metadata").at(0));
    const auto decoded = tp::decode_action(frame);
    const auto& p = std::get<tp::proposal>(*decoded.frame);
    BOOST_TEST_EQ(p.name, "chat");
    BOOST_TEST(p.kind == tp::frame_kind::inline_payload);
    BOOST_TEST(p.format == tp::payload_format::text);
    BOOST_TEST_EQ(p.metadata, R"({"from":"js","n":7})");
    BOOST_TEST_EQ(std::string(p.payload.begin(), p.payload.end()), "hi");
    BOOST_TEST(tp::valid_action_name("abcdefghijklmnopqrstuvwxyz012345"));
    BOOST_TEST(!tp::valid_action_name("abcdefghijklmnopqrstuvwxyz0123456"));
    BOOST_TEST(!tp::valid_action_name(""));
}

void chunked_frames(const json::object& fixture) {
    const auto& actions = fixture.at("actions");
    const auto& chunked = actions.at("chunked").as_object();
    const auto payload = hex(chunked.at("payload"));
    const auto& frames = chunked.at("frames").as_array();
    const auto offer = hex(frames.at(0));
    const auto decoded = tp::decode_action(offer);
    const auto& p = std::get<tp::proposal>(*decoded.frame);
    BOOST_TEST(p.kind == tp::frame_kind::offer);
    BOOST_TEST_EQ(p.size, payload.size());
    BOOST_TEST_EQ(p.metadata, R"({"name":"big.bin"})");
    const auto encoded_offer = tp::encode_offer("file", tp::payload_format::binary,
                                                R"({"name":"big.bin"})", p.id, payload.size());
    BOOST_TEST_ALL_EQ(encoded_offer.begin(), encoded_offer.end(), offer.begin(), offer.end());
    std::size_t offset = 0;
    for (std::size_t i = 1; i < frames.size(); ++i) {
        const auto length = std::min(tp::chunk_size, payload.size() - offset);
        const auto expected = hex(frames.at(i));
        const auto encoded =
            tp::encode_chunk(p.id, offset, std::span(payload).subspan(offset, length));
        BOOST_TEST_ALL_EQ(encoded.begin(), encoded.end(), expected.begin(), expected.end());
        const auto c = std::get<tp::chunk>(*tp::decode_action(expected).frame);
        BOOST_TEST_EQ(c.offset, static_cast<double>(offset));
        BOOST_TEST_EQ(c.data.size(), length);
        offset += length;
    }
    BOOST_TEST_EQ(offset, payload.size());
    const auto rejected = hex(fixture.at("controls").at("rejected_too_large"));
    const auto encoded_rejected = tp::encode_rejected(p.id, "payload exceeds receiver size limit");
    BOOST_TEST_ALL_EQ(encoded_rejected.begin(), encoded_rejected.end(), rejected.begin(),
                      rejected.end());
    const auto control = std::get<tp::control_message>(*tp::decode_action(rejected).frame);
    BOOST_TEST(control.kind == tp::frame_kind::rejected);
    BOOST_TEST_EQ(control.reason, "payload exceeds receiver size limit");
    BOOST_TEST_EQ(tp::encode_accepted(p.id).size(), tp::control_header_size);
}

void envelopes(const json::object& fixture) {
    const auto& env = fixture.at("envelope").as_object();
    const auto token = str(env.at("room_token"));
    BOOST_TEST_EQ(token, tp::room_token("my-app", "room-1"));
    const auto& frames = env.at("frames").as_array();
    const auto present = tp::presence_frame(token, true);
    const auto expected_present = hex(frames.at(0));
    BOOST_TEST_ALL_EQ(present.begin(), present.end(), expected_present.begin(),
                      expected_present.end());
    const tp::bytes action{2, 0, 99, 104, 97, 116};
    const auto room_frame = tp::room_frame(token, action);
    const auto expected_room = hex(frames.at(1));
    BOOST_TEST_ALL_EQ(room_frame.begin(), room_frame.end(), expected_room.begin(),
                      expected_room.end());
    const auto absent = tp::presence_frame(token, false);
    const auto expected_absent = hex(frames.at(2));
    BOOST_TEST_ALL_EQ(absent.begin(), absent.end(), expected_absent.begin(), expected_absent.end());
    const auto room = hex(frames.at(1));
    const auto decoded = tp::decode_envelope(room);
    BOOST_TEST(decoded->type == tp::envelope::kind::room);
    BOOST_TEST_EQ(decoded->token, token);
    BOOST_TEST_ALL_EQ(decoded->payload.begin(), decoded->payload.end(), action.begin(),
                      action.end());
    const auto presence = tp::decode_envelope(hex(frames.at(2)));
    BOOST_TEST(presence->type == tp::envelope::kind::presence);
    BOOST_TEST(!presence->present);
    BOOST_TEST(!tp::decode_envelope(tp::bytes{1, 0, 0}).has_value());
    BOOST_TEST(!tp::decode_envelope(tp::bytes{9, 0, 1, 65}).has_value());
}

void signals() {
    json::value document;  // the views read_signal returns point into it
    const auto read = [&document](std::string_view text) {
        document = json::parse(text);
        return tp::read_signal(document);
    };
    const auto announce = read(R"({"peerId":"abc","nonce":"x1"})");
    if (!BOOST_TEST(announce.has_value())) {
        return;
    }
    BOOST_TEST(announce->is_announcement());
    BOOST_TEST(!read(R"({"peerId":"abc","offer":""})").has_value());
    BOOST_TEST(!read(R"({"offer":"x"})").has_value());
    BOOST_TEST(!read(R"(["peerId"])").has_value());
    const auto written = tp::write_signal(
        {.peer_id = "me", .offer_id = "o1", .candidate = "1,2$AA==", .passive = true}, "n0nce");
    BOOST_TEST_EQ(
        written,
        R"({"peerId":"me","offerId":"o1","candidate":"1,2$AA==","passive":true,"nonce":"n0nce"})");
    const auto back = read(written);
    BOOST_TEST_EQ(back->candidate, "1,2$AA==");
    BOOST_TEST_EQ(back->offer_id, "o1");
    BOOST_TEST(back->passive);
    // Escaping matches JSON.stringify, so a sealed text or a candidate with quotes survives the
    // round trip. The expected text is named outside the macro: MSVC 14.3's preprocessor, which
    // stringizes the macro's arguments, reads a raw string's \" as an escape and stops (C2017).
    constexpr std::string_view escaped = R"({"peerId":"q\"\\\n"})";
    BOOST_TEST_EQ(tp::write_signal({.peer_id = "q\"\\\n"}), escaped);
    // A fixed seed: the test checks the form of an id, the same on every run.
    // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
    std::mt19937 generator(42);
    const auto id = tp::make_id(tp::peer_id_size, generator);
    BOOST_TEST_EQ(id.size(), tp::peer_id_size);
    BOOST_TEST_EQ(id.find_first_not_of(tp::id_alphabet), std::string::npos);
}

// Each field of a signal is bounded, and the bound refuses nothing Trystero sends: its longest
// candidate, 8192 bytes of plaintext, sealed with the longest IV in decimal, is read.
void signal_bounds() {
    json::value document;
    const auto read = [&document](const tp::signal_message& m) {
        document = json::parse(tp::write_signal(m));
        return tp::read_signal(document).has_value();
    };
    std::array<std::uint8_t, tp::seal_iv_size> iv{};
    iv.fill(255);
    const tp::bytes ciphertext(8192 + 16, 7);
    const auto longest_candidate = tp::format_sealed(iv, ciphertext);
    BOOST_TEST_EQ(longest_candidate.size(), tp::sealed_size(8192));
    BOOST_TEST(read({.peer_id = "p", .offer_id = "o", .candidate = longest_candidate}));

    const auto of = [](std::size_t n) { return std::string(n, 'a'); };
    const auto candidate = of(tp::max_sealed_candidate_bytes);
    const auto description = of(tp::max_sealed_description_bytes);
    const auto offer_id = of(tp::max_offer_id_bytes);
    BOOST_TEST(read({.peer_id = "p", .offer_id = "o", .candidate = candidate}));
    BOOST_TEST(!read({.peer_id = "p", .offer_id = "o", .candidate = candidate + "a"}));
    BOOST_TEST(read({.peer_id = "p", .offer_id = "o", .offer = description}));
    BOOST_TEST(!read({.peer_id = "p", .offer_id = "o", .offer = description + "a"}));
    BOOST_TEST(read({.peer_id = "p", .offer_id = "o", .answer = description}));
    BOOST_TEST(!read({.peer_id = "p", .offer_id = "o", .answer = description + "a"}));
    BOOST_TEST(read({.peer_id = "p", .offer_id = offer_id, .offer = "x"}));
    BOOST_TEST(!read({.peer_id = "p", .offer_id = offer_id + "a", .offer = "x"}));
}

void text() {
    BOOST_TEST_EQ(tp::base64_encode(tp::bytes{}), "");
    BOOST_TEST_EQ(tp::base64_encode(tp::bytes{'f'}), "Zg==");
    BOOST_TEST_EQ(tp::base64_encode(tp::bytes{'f', 'o'}), "Zm8=");
    BOOST_TEST_EQ(tp::base64_encode(tp::bytes{'f', 'o', 'o'}), "Zm9v");
    const auto decoded = *tp::base64_decode("Zm8=");
    const tp::bytes fo{'f', 'o'};
    BOOST_TEST_ALL_EQ(decoded.begin(), decoded.end(), fo.begin(), fo.end());
    BOOST_TEST(!tp::base64_decode("Z!==").has_value());
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!BOOST_TEST_EQ(argc, 2)) {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
            << "usage: protocol_test <fixtures/expected/expected.json>\n";
        return boost::report_errors();
    }
    const auto loaded = load(argv[1]);
    const auto& fixture = loaded.as_object();
    topics(fixture);
    relays(fixture);
    handshake_and_sealing(fixture);
    nostr_events(fixture);
    inline_frames(fixture);
    chunked_frames(fixture);
    envelopes(fixture);
    signals();
    signal_bounds();
    text();
    return boost::report_errors();
}
