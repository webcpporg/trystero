// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the protocol layer against values Trystero JS 0.26 computed itself, which the original's
// oracle wrote into fixtures/expected/expected.json. The fixture's path is the test's one argument,
// as webcpp.run passes its input file. The fixture is read with webcpp::document, and every text
// the protocol writes is read back as a room reads it, with protocol::json::parse and its bounds.

#include <webcpp/trystero/protocol/action_wire.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/nostr.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/value.hpp>

#include <boost/core/lightweight_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory_resource>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace wd = webcpp::document;
namespace tp = webcpp::trystero::protocol;

namespace {

/** A value of the fixture: a read-only handle of an owned document. */
using fixture_value = wd::basic_const_value<>;

/** The fixture, read as JSON.parse reads it. */
wd::result<wd::document> load(const char* path) {
    const std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return wd::parse(text.str());
}

std::string_view str(const fixture_value& v) {
    return v.value_or(std::string_view());
}

// Every number of the fixture is an integer; -1 marks one that is not, or a missing one.
std::int64_t integer(const fixture_value& v) {
    return v.value_or(std::int64_t{-1});
}

tp::bytes hex(const fixture_value& v) {
    return tp::from_hex(str(v)).value();
}

/** A protocol text read as a room reads it: at most `max_bytes`, into a buffer of `memory`. */
wd::result<tp::json::document> parse_text(
    std::string_view text, std::size_t max_bytes,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return tp::json::parse(text, tp::json::protocol_limits(max_bytes), memory);
}

/** A resource that counts what it gives: its buffers, their bytes, and what is still out. */
class counting_resource final : public std::pmr::memory_resource {
public:
    /** How many allocations it made. */
    std::size_t allocations = 0;
    /** How many of them are not yet returned. */
    std::size_t outstanding = 0;
    /** The bytes of all its allocations. */
    std::size_t allocated = 0;
    /** The bytes of its largest allocation. */
    std::size_t largest = 0;
    /** Its last allocation. */
    void* last = nullptr;
    /** The bytes of its last allocation. */
    std::size_t last_bytes = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        ++outstanding;
        allocated += bytes;
        largest = std::max(largest, bytes);
        last_bytes = bytes;
        last = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        return last;
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        --outstanding;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

void topics(const fixture_value& fixture) {
    for (const auto t : fixture["topics"]) {
        const auto app = str(t["app_id"]);
        const auto room = str(t["room_id"]);
        BOOST_TEST_EQ(tp::root_topic(app, room), str(t["root_topic"]));
        BOOST_TEST_EQ(tp::peer_topic(app, room, str(t["peer_id"])), str(t["self_topic"]));
        BOOST_TEST_EQ(tp::room_token(app, room), str(t["room_token"]));
    }
    for (const auto t : fixture["str_to_num"]) {
        BOOST_TEST_EQ(tp::str_to_num(str(t["text"])), static_cast<double>(integer(t["unbounded"])));
        BOOST_TEST_EQ(tp::str_to_num(str(t["text"]), 10000),
                      static_cast<double>(integer(t["mod_10000"])));
    }
}

void relays(const fixture_value& fixture) {
    std::vector<std::string_view> defaults;
    for (const auto url : fixture["default_relays"]) {
        defaults.push_back(str(url));
    }
    BOOST_TEST(std::ranges::equal(defaults, tp::default_nostr_relays));
    for (const auto entry : fixture["relays"]) {
        std::vector<std::string_view> expected;
        for (const auto url : entry["relays"]) {
            expected.push_back(str(url));
        }
        const auto derived = tp::derive_relays(str(entry["app_id"]), tp::default_nostr_relays, 5);
        BOOST_TEST_ALL_EQ(derived.begin(), derived.end(), expected.begin(), expected.end());
    }
    for (const auto entry : fixture["shuffle"]) {
        std::vector<int> order(29);
        for (int i = 0; i < 29; ++i) {
            order[i] = i;
        }
        std::vector<int> expected;
        for (const auto i : entry["order"]) {
            expected.push_back(static_cast<int>(integer(i)));
        }
        const auto shuffled = tp::shuffle(order, static_cast<double>(integer(entry["seed"])));
        BOOST_TEST_ALL_EQ(shuffled.begin(), shuffled.end(), expected.begin(), expected.end());
    }
}

void handshake_and_sealing(const fixture_value& fixture) {
    for (const auto entry : fixture["handshake"]) {
        BOOST_TEST_EQ(tp::handshake_proof(str(entry["challenge"]), str(entry["password"]),
                                          str(entry["app_id"]), str(entry["room_id"])),
                      str(entry["hash"]));
    }
    const auto sealed = fixture["sealed"];
    BOOST_TEST_EQ(tp::to_hex(tp::seal_key("secret", "my-app", "room-1")),
                  str(sealed["key_material"]));
    const auto parts = tp::parse_sealed(str(sealed["text"]));
    BOOST_TEST(parts.has_value());
    BOOST_TEST_EQ(parts->iv.size(), tp::seal_iv_size);
    BOOST_TEST_EQ(parts->ciphertext.size(), str(sealed["plaintext"]).size() + 16);
    BOOST_TEST_EQ(tp::format_sealed(parts->iv, parts->ciphertext), str(sealed["text"]));
    BOOST_TEST(!tp::parse_sealed("1,2,300$AAAA").has_value());
    BOOST_TEST(!tp::parse_sealed("no dollar").has_value());
}

/** A relay's echo of a client's `["EVENT",<event>]`: the event's text whole, less the client's
 * first 9 bytes and its last, in `["EVENT","sub",<event>]`. */
std::string echo_of(std::string_view message) {
    std::string echo = R"(["EVENT","sub",)";
    echo += message.substr(9, message.size() - 10);
    echo += ']';
    return echo;
}

/** The integer a protocol text holds, -1 for any other value. */
std::int64_t integer_of(const tp::json::const_value& v) {
    return v.value_or(std::int64_t{-1});
}

void nostr_events(const fixture_value& fixture) {
    for (const auto e : fixture["nostr_events"]) {
        BOOST_TEST_EQ(tp::nostr::topic_kind(str(e["topic"])), integer(e["kind"]));
        const auto created_at =
            std::chrono::sys_seconds(std::chrono::seconds(integer(e["created_at"])));
        const auto id = tp::nostr::event_id(str(e["pubkey"]), created_at,
                                            static_cast<std::uint32_t>(integer(e["kind"])),
                                            str(e["topic"]), str(e["content"]));
        BOOST_TEST_EQ(tp::to_hex(id), str(e["id"]));
    }
    // A signed event verifies, and a relay's echo of it parses back to its topic and content.
    std::array<std::uint8_t, 32> secret{};
    secret.fill(7);
    const auto key = tp::nostr::signer::from_secret(secret);
    BOOST_TEST(key.has_value());
    std::array<std::uint8_t, 32> aux{};
    const auto message =
        tp::nostr::event_message(*key, std::chrono::sys_seconds(std::chrono::seconds(1759700000)),
                                 "topic-a", R"({"peerId":"p"})", aux);
    const auto parsed = parse_text(message, tp::nostr::max_parsed_relay_text_bytes).value();
    const auto event = parsed.root()[1];
    const auto id = tp::from_hex(tp::json::text_of(event["id"])).value();
    const auto sig = tp::from_hex(tp::json::text_of(event["sig"])).value();
    const auto pub = tp::from_hex(tp::json::text_of(event["pubkey"])).value();
    BOOST_TEST(tp::nostr::verify(std::span<const std::uint8_t, 64>(sig.data(), 64),
                                 std::span<const std::uint8_t, 32>(id.data(), 32),
                                 std::span<const std::uint8_t, 32>(pub.data(), 32)));
    const std::string_view written = message;
    BOOST_TEST(written.starts_with(R"(["EVENT",{)") && written.ends_with("}]"));
    const auto echoed_document =
        parse_text(echo_of(message), tp::nostr::max_parsed_relay_text_bytes).value();
    const auto echoed = tp::nostr::read_relay_message(echoed_document.root());
    BOOST_TEST(echoed.has_value());
    const auto& e = std::get<tp::nostr::event>(*echoed);
    BOOST_TEST_EQ(e.subscription, "sub");
    BOOST_TEST_EQ(e.topic, "topic-a");
    BOOST_TEST_EQ(e.content, R"({"peerId":"p"})");
    constexpr std::string_view rate_limited = R"(["OK","abc",false,"rate-limited: slow down"])";
    const auto rejected_document =
        parse_text(rate_limited, tp::nostr::max_parsed_relay_text_bytes).value();
    const auto rejected = tp::nostr::read_relay_message(rejected_document.root());
    BOOST_TEST_EQ(std::get<tp::nostr::ok>(*rejected).reason, "rate-limited: slow down");
    BOOST_TEST(!std::get<tp::nostr::ok>(*rejected).accepted);
}

// An event message and a subscription write the values they are given, read back as a relay reads
// them: the kind Trystero gives the topic, `created_at` and `since` the time, the fixture's and one
// past 2^32 seconds, which 32 bits would not hold, and an id computed again from the message's own
// fields. A subscription lists each kind once, in increasing order, and the topics as given.
void nostr_message_fields(const fixture_value& fixture) {
    const auto first = fixture["nostr_events"][0];
    const auto topic = str(first["topic"]);
    const auto kind = integer(first["kind"]);
    // A topic of another kind, which Trystero's own event message for it holds.
    const auto other = fixture["event_messages"]["events"][0];
    const auto other_topic = str(other["topic"]);
    const auto other_message =
        parse_text(str(other["message"]), tp::nostr::max_parsed_relay_text_bytes);
    if (!BOOST_TEST(other_message.has_value())) {
        return;
    }
    const auto other_kind = integer_of(other_message->root()[1]["kind"]);
    BOOST_TEST_NE(kind, other_kind);
    std::array<std::uint8_t, 32> secret{};
    secret.fill(7);
    const auto key = tp::nostr::signer::from_secret(secret);
    if (!BOOST_TEST(key.has_value())) {
        return;
    }
    const std::array<std::uint8_t, 32> aux{};
    for (const std::int64_t seconds : {integer(first["created_at"]), std::int64_t{4'354'000'000}}) {
        const auto at = std::chrono::sys_seconds(std::chrono::seconds(seconds));
        const auto message = tp::nostr::event_message(*key, at, topic, R"({"peerId":"p"})", aux);
        const auto document = parse_text(message, tp::nostr::max_parsed_relay_text_bytes);
        if (!BOOST_TEST(document.has_value())) {
            continue;
        }
        const auto event = document->root()[1];
        BOOST_TEST_EQ(integer_of(event["created_at"]), seconds);
        BOOST_TEST_EQ(integer_of(event["kind"]), kind);
        const auto tags = event["tags"];
        BOOST_TEST_EQ(tags.size(), 1U);
        BOOST_TEST_EQ(tags[0].size(), 2U);
        BOOST_TEST_EQ(tp::json::text_of(tags[0][0]), "x");
        BOOST_TEST_EQ(tp::json::text_of(tags[0][1]), topic);
        BOOST_TEST_EQ(tp::json::text_of(event["content"]), R"({"peerId":"p"})");
        BOOST_TEST_EQ(tp::json::text_of(event["pubkey"]), key->pubkey());
        const auto id = tp::nostr::event_id(
            tp::json::text_of(event["pubkey"]),
            std::chrono::sys_seconds(std::chrono::seconds(integer_of(event["created_at"]))),
            static_cast<std::uint32_t>(integer_of(event["kind"])), tp::json::text_of(tags[0][1]),
            tp::json::text_of(event["content"]));
        BOOST_TEST_EQ(tp::to_hex(id), tp::json::text_of(event["id"]));

        const std::array<std::pmr::string, 3> topics{
            std::pmr::string(topic),
            std::pmr::string(other_topic),
            std::pmr::string(topic),
        };
        const auto request = parse_text(tp::nostr::subscribe_message("sub-1", topics, at),
                                        tp::nostr::max_parsed_relay_text_bytes);
        if (!BOOST_TEST(request.has_value())) {
            continue;
        }
        const auto req = request->root();
        BOOST_TEST_EQ(req.size(), 3U);
        BOOST_TEST_EQ(tp::json::text_of(req[0]), "REQ");
        BOOST_TEST_EQ(tp::json::text_of(req[1]), "sub-1");
        const auto filter = req[2];
        BOOST_TEST_EQ(integer_of(filter["since"]), seconds);
        std::vector<std::int64_t> kinds;
        for (const auto k : filter["kinds"]) {
            kinds.push_back(integer_of(k));
        }
        const std::array<std::int64_t, 2> increasing{
            std::min(kind, other_kind),
            std::max(kind, other_kind),
        };
        BOOST_TEST_ALL_EQ(kinds.begin(), kinds.end(), increasing.begin(), increasing.end());
        std::vector<std::string_view> named;
        for (const auto t : filter["#x"]) {
            named.push_back(tp::json::text_of(t));
        }
        BOOST_TEST_ALL_EQ(named.begin(), named.end(), topics.begin(), topics.end());
    }
}

/** `text` with the first `from` in it replaced by `to`; a `from` it does not hold fails the test.
 */
std::string replaced(std::string_view text, std::string_view from, std::string_view to) {
    std::string out(text);
    const auto at = out.find(from);
    if (BOOST_TEST(at != std::string::npos)) {
        out.replace(at, from.size(), to);
    }
    return out;
}

/** The id a client's `["EVENT",<event>]` holds, as event_message writes it. */
std::string_view id_in(std::string_view message) {
    constexpr std::string_view key = R"("id":")";
    return message.substr(message.find(key) + key.size(), 64);
}

/** `hex` with its letters in uppercase. */
std::string uppercase(std::string_view hex) {
    std::string out(hex);
    for (char& c : out) {
        if (c >= 'a' && c <= 'f') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

/** Whether the event of a client's `["EVENT",<event>]`, read from a relay's echo of it, has its
 * own id; false when it is no event. */
bool own_id(std::string_view message) {
    const auto document = parse_text(echo_of(message), tp::nostr::max_parsed_relay_text_bytes);
    if (!document.has_value()) {
        return false;
    }
    const auto read = tp::nostr::read_relay_message(document->root());
    const auto* e = read.has_value() ? std::get_if<tp::nostr::event>(&*read) : nullptr;
    return e != nullptr && tp::nostr::has_own_id(*e);
}

// An event's id is its own when it is the SHA-256 of NIP-01's serialization of the event: that of
// each event Trystero's createEvent made, whose contents hold every character JSON escapes, and of
// one whose time is written as another JSON text of the same number. An event changed in any field
// the id hashes, or under another event's id, does not have its own id; neither has one whose tags
// are more than its topic's, which NIP-01 hashes too, nor one whose time or kind is not an integer,
// nor one whose public key or id is not in lowercase, whatever its id.
void event_ids_are_their_own(const fixture_value& fixture) {
    for (const auto e : fixture["event_messages"]["events"]) {
        BOOST_TEST(own_id(str(e["message"])));
    }
    std::array<std::uint8_t, 32> secret{};
    secret.fill(7);
    const auto key = tp::nostr::signer::from_secret(secret);
    if (!BOOST_TEST(key.has_value())) {
        return;
    }
    const std::array<std::uint8_t, 32> aux{};
    const auto at = std::chrono::sys_seconds(std::chrono::seconds(1759700000));
    const std::string message(
        tp::nostr::event_message(*key, at, "topic-a", R"({"peerId":"p"})", aux));
    const std::string kind = R"("kind":)" + std::to_string(tp::nostr::topic_kind("topic-a"));
    const std::string other_kind =
        R"("kind":)" + std::to_string(tp::nostr::topic_kind("topic-a") + 1);
    const std::string created_at = R"("created_at":1759700000)";
    const std::string forged_from(
        tp::nostr::event_message(*key, at, "topic-a", R"({"peerId":"forged"})", aux));
    const std::string forged = replaced(forged_from, id_in(forged_from), id_in(message));

    BOOST_TEST(own_id(message));
    BOOST_TEST(own_id(replaced(message, created_at, R"("created_at":1.7597e9)")));
    BOOST_TEST(!own_id(replaced(message, R"({\"peerId\")", R"({\"peerID\")")));
    BOOST_TEST(!own_id(replaced(message, R"("topic-a")", R"("topic-b")")));
    BOOST_TEST(!own_id(replaced(message, kind, other_kind)));
    BOOST_TEST(!own_id(replaced(message, created_at, R"("created_at":1759700001)")));
    BOOST_TEST(!own_id(forged));
    BOOST_TEST(!own_id(replaced(message, R"("topic-a"]])", R"("topic-a"],["e","f"]])")));
    BOOST_TEST(!own_id(replaced(message, R"("topic-a"]])", R"("topic-a","r"]])")));
    BOOST_TEST(!own_id(replaced(message, created_at, R"("created_at":"1759700000")")));
    BOOST_TEST(!own_id(replaced(message, created_at, R"("created_at":1759700000.5)")));
    BOOST_TEST(!own_id(replaced(message, created_at, R"("created_at":1e30)")));
    BOOST_TEST(!own_id(replaced(message, kind, R"("kind":4294967296)")));
    BOOST_TEST(!own_id(replaced(message, key->pubkey(), uppercase(key->pubkey()))));
    BOOST_TEST(!own_id(replaced(message, id_in(message), uppercase(id_in(message)))));
}

void inline_frames(const fixture_value& fixture) {
    const auto actions = fixture["actions"];
    const auto encode = [](std::string_view name, tp::payload_format format,
                           std::string_view metadata, std::string_view payload) {
        return tp::encode_inline(
            name, format, metadata,
            std::span(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
    };
    const auto text = encode("chat", tp::payload_format::text, "", "hello from Trystero JS");
    const auto expected_text = hex(actions["text"][0]);
    BOOST_TEST_ALL_EQ(text.begin(), text.end(), expected_text.begin(), expected_text.end());
    const auto json_frame = encode("drink", tp::payload_format::json, "",
                                   R"({"drink":"negroni","withIce":true,"n":[1,2.5,null]})");
    const auto expected_json = hex(actions["json"][0]);
    BOOST_TEST_ALL_EQ(json_frame.begin(), json_frame.end(), expected_json.begin(),
                      expected_json.end());
    const tp::bytes binary{0, 1, 2, 253, 254, 255};
    const auto binary_frame = tp::encode_inline("pic", tp::payload_format::binary, "", binary);
    const auto expected_binary = hex(actions["binary"][0]);
    BOOST_TEST_ALL_EQ(binary_frame.begin(), binary_frame.end(), expected_binary.begin(),
                      expected_binary.end());
    const auto with_metadata =
        encode("chat", tp::payload_format::text, R"({"from":"js","n":7})", "hi");
    const auto expected_with_metadata = hex(actions["text_with_metadata"][0]);
    BOOST_TEST_ALL_EQ(with_metadata.begin(), with_metadata.end(), expected_with_metadata.begin(),
                      expected_with_metadata.end());
    const auto empty = encode("@_hsready", tp::payload_format::text, "", "");
    const auto expected_empty = hex(actions["empty_text"][0]);
    BOOST_TEST_ALL_EQ(empty.begin(), empty.end(), expected_empty.begin(), expected_empty.end());
    const auto long_name =
        encode("abcdefghijklmnopqrstuvwxyz012345", tp::payload_format::text, "", "x");
    const auto expected_long_name = hex(actions["thirty_two_byte_name"][0]);
    BOOST_TEST_ALL_EQ(long_name.begin(), long_name.end(), expected_long_name.begin(),
                      expected_long_name.end());

    const auto frame = hex(actions["text_with_metadata"][0]);
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

void chunked_frames(const fixture_value& fixture) {
    const auto chunked = fixture["actions"]["chunked"];
    const auto payload = hex(chunked["payload"]);
    const auto frames = chunked["frames"];
    const auto offer = hex(frames[0]);
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
        const auto expected = hex(frames[i]);
        const auto encoded =
            tp::encode_chunk(p.id, offset, std::span(payload).subspan(offset, length));
        BOOST_TEST_ALL_EQ(encoded.begin(), encoded.end(), expected.begin(), expected.end());
        const auto c = std::get<tp::chunk>(*tp::decode_action(expected).frame);
        BOOST_TEST_EQ(c.offset, static_cast<double>(offset));
        BOOST_TEST_EQ(c.data.size(), length);
        offset += length;
    }
    BOOST_TEST_EQ(offset, payload.size());
    const auto rejected = hex(fixture["controls"]["rejected_too_large"]);
    const auto encoded_rejected = tp::encode_rejected(p.id, "payload exceeds receiver size limit");
    BOOST_TEST_ALL_EQ(encoded_rejected.begin(), encoded_rejected.end(), rejected.begin(),
                      rejected.end());
    const auto control = std::get<tp::control_message>(*tp::decode_action(rejected).frame);
    BOOST_TEST(control.kind == tp::frame_kind::rejected);
    BOOST_TEST_EQ(control.reason, "payload exceeds receiver size limit");
    BOOST_TEST_EQ(tp::encode_accepted(p.id).size(), tp::control_header_size);
}

void envelopes(const fixture_value& fixture) {
    const auto env = fixture["envelope"];
    const auto token = str(env["room_token"]);
    BOOST_TEST_EQ(token, tp::room_token("my-app", "room-1"));
    const auto frames = env["frames"];
    const auto present = tp::presence_frame(token, true);
    const auto expected_present = hex(frames[0]);
    BOOST_TEST_ALL_EQ(present.begin(), present.end(), expected_present.begin(),
                      expected_present.end());
    const tp::bytes action{2, 0, 99, 104, 97, 116};
    const auto room_frame = tp::room_frame(token, action);
    const auto expected_room = hex(frames[1]);
    BOOST_TEST_ALL_EQ(room_frame.begin(), room_frame.end(), expected_room.begin(),
                      expected_room.end());
    const auto absent = tp::presence_frame(token, false);
    const auto expected_absent = hex(frames[2]);
    BOOST_TEST_ALL_EQ(absent.begin(), absent.end(), expected_absent.begin(), expected_absent.end());
    const auto room = hex(frames[1]);
    const auto decoded = tp::decode_envelope(room);
    BOOST_TEST(decoded->type == tp::envelope::kind::room);
    BOOST_TEST_EQ(decoded->token, token);
    BOOST_TEST_ALL_EQ(decoded->payload.begin(), decoded->payload.end(), action.begin(),
                      action.end());
    const auto presence = tp::decode_envelope(hex(frames[2]));
    BOOST_TEST(presence->type == tp::envelope::kind::presence);
    BOOST_TEST(!presence->present);
    BOOST_TEST(!tp::decode_envelope(tp::bytes{1, 0, 0}).has_value());
    BOOST_TEST(!tp::decode_envelope(tp::bytes{9, 0, 1, 65}).has_value());
}

void signals() {
    tp::json::document document;  // the views read_signal returns point into it
    const auto read = [&document](std::string_view text) -> std::optional<tp::signal_message> {
        auto parsed = parse_text(text, tp::max_parsed_signal_bytes);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        document = std::move(parsed).value();
        return tp::read_signal(std::as_const(document).root());
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
}

// An id has one character of Trystero's 62 for each random byte, the byte's remainder in 62: the
// first and the last character, and the remainders 0 and 61 of larger bytes.
void random_ids() {
    const std::array<std::uint8_t, 6> random{0, 61, 62, 123, 124, 255};
    BOOST_TEST_EQ(tp::random_id(random), "0z0z07");
    BOOST_TEST_EQ(tp::random_id(std::span<const std::uint8_t>{}), "");
    std::array<std::uint8_t, tp::peer_id_size> all{};
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = static_cast<std::uint8_t>(i * 13);
    }
    std::pmr::monotonic_buffer_resource memory;
    const auto id = tp::random_id(all, &memory);
    BOOST_TEST(id.get_allocator().resource() == &memory);
    BOOST_TEST_EQ(id.size(), tp::peer_id_size);
    BOOST_TEST_EQ(id.find_first_not_of(tp::id_alphabet), std::string::npos);
}

// An offer's size is a float64 that Trystero accepts up to Number.MAX_SAFE_INTEGER, 2^53 - 1, the
// bytes 43 3f ff ff ff ff ff ff, and refuses from 2^53, 43 40 00 00 00 00 00 00, as any size that
// is not a positive integer: the largest is read whole on every target, a 32-bit one too.
void offer_sizes_at_two_to_the_53() {
    using size_bytes = std::array<std::uint8_t, 8>;
    const auto offer_of_size = [](const size_bytes& size) {
        auto frame = tp::encode_offer("file", tp::payload_format::binary, "", 7, 1);
        std::ranges::copy(size, frame.begin() + 40);
        return frame;
    };
    // The decoded frame's views point into the frame, which is kept while they are read.
    const auto frame = offer_of_size({0x43, 0x3f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
    const auto largest = tp::decode_action(frame);
    if (!BOOST_TEST(largest.frame.has_value())) {
        return;
    }
    const auto& p = std::get<tp::proposal>(*largest.frame);
    BOOST_TEST_EQ(p.size, std::uint64_t{9007199254740991});
    BOOST_TEST_EQ(p.size, tp::max_offer_size);
    BOOST_TEST_EQ(p.id, 7U);
    const std::array refused{
        size_bytes{0x43, 0x40, 0, 0, 0, 0, 0, 0},  // 2^53
        size_bytes{0x43, 0x40, 0, 0, 0, 0, 0, 1},  // 2^53 + 2
        size_bytes{0x3f, 0xf8, 0, 0, 0, 0, 0, 0},  // 1.5
        size_bytes{0, 0, 0, 0, 0, 0, 0, 0},        // 0
        size_bytes{0xbf, 0xf0, 0, 0, 0, 0, 0, 0},  // -1
        size_bytes{0x7f, 0xf8, 0, 0, 0, 0, 0, 0},  // NaN
    };
    for (const auto& size : refused) {
        const auto refused_frame = offer_of_size(size);
        const auto decoded = tp::decode_action(refused_frame);
        BOOST_TEST(!decoded.frame.has_value());
        BOOST_TEST(decoded.error == tp::wire_error::invalid_proposal);
    }
}

// Every field wider than a byte is big-endian, as DataView writes it, high bytes included: the
// oracle's frames have a short metadata, small ids, offsets and sizes, and a token of 64 bytes.
void wide_fields_are_big_endian() {
    const std::string metadata(0x0102, 'm');
    const auto offer =
        tp::encode_offer("file", tp::payload_format::binary, metadata, 0x01020304, 0x12345678);
    // The metadata's length, the transfer's id, and the size as a float64.
    const tp::bytes offer_fields{
        0x01, 0x02, 0x01, 0x02, 0x03, 0x04, 0x41, 0xb2, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00,
    };
    BOOST_TEST_ALL_EQ(offer.begin() + 34, offer.begin() + 48, offer_fields.begin(),
                      offer_fields.end());
    const auto decoded = std::get<tp::proposal>(*tp::decode_action(offer).frame);
    BOOST_TEST_EQ(decoded.id, 0x01020304U);
    BOOST_TEST_EQ(decoded.size, 0x12345678U);
    BOOST_TEST_EQ(decoded.metadata.size(), metadata.size());

    const tp::bytes data{9};
    const auto chunk = tp::encode_chunk(0x0a0b0c0d, 0x12345678, data);
    const tp::bytes expected_chunk{
        2, 2, 0x0a, 0x0b, 0x0c, 0x0d, 0x41, 0xb2, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 9,
    };
    BOOST_TEST_ALL_EQ(chunk.begin(), chunk.end(), expected_chunk.begin(), expected_chunk.end());
    const auto c = std::get<tp::chunk>(*tp::decode_action(chunk).frame);
    BOOST_TEST_EQ(c.id, 0x0a0b0c0dU);
    BOOST_TEST_EQ(c.offset, 305419896.0);
    const auto accepted = tp::encode_accepted(0xfffefdfc);
    const tp::bytes expected_accepted{2, 3, 0xff, 0xfe, 0xfd, 0xfc};
    BOOST_TEST_ALL_EQ(accepted.begin(), accepted.end(), expected_accepted.begin(),
                      expected_accepted.end());

    const std::string token(0x012c, 't');
    const auto presence = tp::presence_frame(token, true);
    BOOST_TEST_EQ(presence.size(), 4 + token.size());
    BOOST_TEST_EQ(presence[2], 0x01);
    BOOST_TEST_EQ(presence[3], 0x2c);
    const auto room = tp::room_frame(token, data);
    BOOST_TEST_EQ(room.size(), 3 + token.size() + data.size());
    BOOST_TEST_EQ(room[1], 0x01);
    BOOST_TEST_EQ(room[2], 0x2c);
    const std::string longest(tp::max_room_token_bytes, 't');
    const auto longest_frame = tp::room_frame(longest, data);
    const auto envelope = tp::decode_envelope(longest_frame);
    if (BOOST_TEST(envelope.has_value())) {
        BOOST_TEST_EQ(envelope->token.size(), tp::max_room_token_bytes);
    }
}

// Each field of a signal is bounded, and the bound refuses nothing Trystero sends: its longest
// candidate, 8192 bytes of plaintext, sealed with the longest IV in decimal, is read.
void signal_bounds() {
    tp::json::document document;
    const auto read = [&document](const tp::signal_message& m) {
        auto parsed = parse_text(tp::write_signal(m), tp::max_parsed_signal_bytes);
        if (!parsed.has_value()) {
            return false;
        }
        document = std::move(parsed).value();
        return tp::read_signal(std::as_const(document).root()).has_value();
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

// An event message is Trystero's createEvent's, byte for byte, signed with the oracle's key, clock
// and auxiliary bytes: its topic and content hold every control character, `"`, `\`, U+2028,
// U+2029, DEL and astral characters, which document's quoting writes as JSON.stringify does, and
// its id is the one the protocol computes.
void event_messages(const fixture_value& fixture) {
    const auto messages = fixture["event_messages"];
    const auto secret = hex(messages["secret_key"]);
    const auto aux = hex(messages["aux"]);
    if (!BOOST_TEST_EQ(secret.size(), 32U) || !BOOST_TEST_EQ(aux.size(), 32U)) {
        return;
    }
    const auto key =
        tp::nostr::signer::from_secret(std::span<const std::uint8_t, 32>(secret.data(), 32));
    if (!BOOST_TEST(key.has_value())) {
        return;
    }
    const std::span<const std::uint8_t, 32> aux_bytes(aux.data(), 32);
    const auto created_at =
        std::chrono::sys_seconds(std::chrono::seconds(integer(messages["created_at"])));
    BOOST_TEST_EQ(messages["events"].size(), 6U);
    for (const auto e : messages["events"]) {
        const auto topic = str(e["topic"]);
        const auto content = str(e["content"]);
        BOOST_TEST_EQ(tp::nostr::event_message(*key, created_at, topic, content, aux_bytes),
                      str(e["message"]));
        const auto id = tp::nostr::event_id(key->pubkey(), created_at, tp::nostr::topic_kind(topic),
                                            topic, content);
        BOOST_TEST_EQ(tp::to_hex(id), str(e["id"]));
    }
}

// A signal's text is JSON.stringify's of Trystero's object, member for member, with the same
// strings, every byte of their quoting included, and the protocol reads the same fields back.
void signal_texts(const fixture_value& fixture) {
    BOOST_TEST_EQ(fixture["signal_texts"].size(), 7U);
    for (const auto entry : fixture["signal_texts"]) {
        const auto s = entry["signal"];
        // The answerer's messages are those whose second member is the answer or the candidate.
        std::string_view second;
        std::size_t at = 0;
        for (const std::string_view key : s.keys()) {
            if (at++ == 1) {
                second = key;
            }
        }
        const tp::signal_message m{
            .peer_id = str(s["peerId"]),
            .offer_id = str(s["offerId"]),
            .offer = str(s["offer"]),
            .answer = str(s["answer"]),
            .candidate = str(s["candidate"]),
            .passive = s["passive"].value_or(false),
            .answering =
                second == "answer" || (second == "candidate" && !s["offerId"].is_undefined()),
        };
        const auto text = str(entry["text"]);
        BOOST_TEST_EQ(tp::write_signal(m, str(s["nonce"])), text);
        const auto document = parse_text(text, tp::max_parsed_signal_bytes).value();
        const auto read = tp::read_signal(document.root());
        if (!BOOST_TEST(read.has_value())) {
            continue;
        }
        BOOST_TEST_EQ(read->peer_id, m.peer_id);
        BOOST_TEST_EQ(read->offer_id, m.offer_id);
        BOOST_TEST_EQ(read->offer, m.offer);
        BOOST_TEST_EQ(read->answer, m.answer);
        BOOST_TEST_EQ(read->candidate, m.candidate);
        BOOST_TEST_EQ(read->passive, m.passive);
    }
}

/**
 * The sender of a relay's event that a room acts on as the oracle's room did: an event of the
 * room's root topic whose content is a signal with an answer. Empty when the room ignores it.
 */
std::string answer_sender(std::string_view text, std::string_view root_topic) {
    const auto document = parse_text(text, tp::nostr::max_parsed_relay_text_bytes);
    if (!document.has_value()) {
        return {};
    }
    const auto message = tp::nostr::read_relay_message(document->root());
    const auto* e = message.has_value() ? std::get_if<tp::nostr::event>(&*message) : nullptr;
    if (e == nullptr || e->topic != root_topic) {
        return {};
    }
    const auto signal_document = parse_text(e->content, tp::max_parsed_signal_bytes);
    if (!signal_document.has_value()) {
        return {};
    }
    const auto signal = tp::read_signal(signal_document->root());
    if (!signal.has_value() || signal->answer.empty()) {
        return {};
    }
    return std::string(signal->peer_id);
}

// A key written twice keeps its last value, as JSON.parse keeps it, an escaped key included: a
// signal reads what Trystero's own signal handler read from the same text, and a relay's event is
// acted on, with the same sender, exactly when Trystero's room acted on it.
void duplicate_keys(const fixture_value& fixture) {
    const auto cases = fixture["duplicate_keys"];
    BOOST_TEST_EQ(cases["signals"].size(), 13U);
    for (const auto entry : cases["signals"]) {
        const auto document = parse_text(str(entry["text"]), tp::max_parsed_signal_bytes).value();
        const auto read = tp::read_signal(document.root());
        const auto expected = entry["read"];
        if (expected.is_null()) {
            BOOST_TEST(!read.has_value());
            continue;
        }
        if (!BOOST_TEST(read.has_value())) {
            continue;
        }
        BOOST_TEST_EQ(read->peer_id, str(expected["peer_id"]));
        BOOST_TEST_EQ(read->offer, str(expected["offer"]));
        BOOST_TEST_EQ(read->answer, str(expected["answer"]));
        BOOST_TEST_EQ(read->candidate, str(expected["candidate"]));
        BOOST_TEST_EQ(read->passive, expected["passive"].value_or(false));
        // Trystero shows an offer's id, which it keeps to answer with.
        if (expected.contains("offer_id")) {
            BOOST_TEST_EQ(read->offer_id, str(expected["offer_id"]));
        }
    }
    const auto root_topic = str(cases["root_topic"]);
    BOOST_TEST_EQ(cases["events"].size(), 6U);
    for (const auto entry : cases["events"]) {
        BOOST_TEST_EQ(answer_sender(str(entry["text"]), root_topic), str(entry["acted"]));
    }
}

/** `text` with spaces after its value, which JSON allows, up to `size` bytes. */
std::string padded(std::string_view text, std::size_t size) {
    std::string out(text);
    out.resize(size, ' ');
    return out;
}

/** The longest signal the bounds of its fields admit: a passive room's sealed offer. */
std::string longest_signal() {
    const std::string peer(tp::max_peer_id_bytes, 'p');
    const std::string offer_id(tp::max_offer_id_bytes, 'o');
    const std::string description(tp::max_sealed_description_bytes, 'a');
    return std::string(tp::write_signal(
        {.peer_id = peer, .offer_id = offer_id, .offer = description, .passive = true}));
}

// The bounds a room parses with refuse no text Trystero sends, and the relay's is under the length
// that closes a relay's connection, so that a text between the two is dropped and the room goes on.
static_assert(tp::max_parsed_signal_bytes == tp::max_sealed_description_bytes + 1024);
static_assert(tp::max_parsed_signal_bytes == 66560);
static_assert(tp::max_sealed_description_bytes + tp::max_peer_id_bytes + tp::max_offer_id_bytes +
                  64 <=
              tp::max_parsed_signal_bytes);
static_assert(tp::nostr::max_parsed_relay_text_bytes == tp::max_parsed_signal_bytes + 2048);
static_assert(tp::nostr::max_parsed_relay_text_bytes == 68608);
static_assert(tp::nostr::max_parsed_relay_text_bytes < tp::max_relay_message_bytes);
static_assert(tp::max_handshake_bytes == 1024);
// A parse reads at most its bound of text, into an image of at most twice that and 64 KiB more.
static_assert(tp::json::protocol_limits(1000).max_bytes == 1000);
static_assert(tp::json::protocol_limits(1000).max_document_bytes == 2000 + 65536);

// Each text a room parses is read up to its bound, to the byte, and one byte more is refused
// before anything of it is read: no buffer is made, and a text that is not JSON at all is refused
// for its length, not its syntax. The relay's bound reads an event that carries the longest
// signal, which the signal's bound reads in turn, and the handshake's reads Trystero's message.
void parse_bounds() {
    const auto refused_unread = [](std::string_view text, std::size_t max_bytes) {
        counting_resource memory;
        const auto refused = parse_text(text, max_bytes, &memory);
        const auto garbage = parse_text(std::string(text.size(), 'x'), max_bytes, &memory);
        return !refused.has_value() && refused.error() == wd::errc::capacity &&
               !garbage.has_value() && garbage.error() == wd::errc::capacity &&
               memory.allocations == 0;
    };

    const auto signal = longest_signal();
    const auto longest = padded(signal, tp::max_parsed_signal_bytes);
    if (!BOOST_TEST_LT(signal.size(), longest.size())) {
        return;
    }
    {
        const auto document = parse_text(longest, tp::max_parsed_signal_bytes).value();
        const auto read = tp::read_signal(document.root());
        BOOST_TEST(read.has_value() && read->offer.size() == tp::max_sealed_description_bytes &&
                   read->peer_id.size() == tp::max_peer_id_bytes && read->passive);
    }
    BOOST_TEST(refused_unread(longest + " ", tp::max_parsed_signal_bytes));

    // A relay's echo of an event that carries it, with a subscription id of Trystero's 64
    // characters.
    std::array<std::uint8_t, 32> secret{};
    secret.fill(7);
    const auto key = tp::nostr::signer::from_secret(secret);
    if (!BOOST_TEST(key.has_value())) {
        return;
    }
    const std::array<std::uint8_t, 32> aux{};
    const auto topic = tp::peer_topic("my-app", "room-1", std::string(tp::max_peer_id_bytes, 'p'));
    const auto message = tp::nostr::event_message(
        *key, std::chrono::sys_seconds(std::chrono::seconds(1759700000)), topic, signal, aux);
    const std::string_view written = message;
    std::string echo = R"(["EVENT",")" + std::string(64, 's') + R"(",)";
    echo += written.substr(9, written.size() - 10);
    echo += ']';
    const auto event_text = padded(echo, tp::nostr::max_parsed_relay_text_bytes);
    if (!BOOST_TEST_LT(echo.size(), event_text.size())) {
        return;
    }
    {
        const auto document =
            parse_text(event_text, tp::nostr::max_parsed_relay_text_bytes).value();
        const auto message_read = tp::nostr::read_relay_message(document.root());
        const auto* e =
            message_read.has_value() ? std::get_if<tp::nostr::event>(&*message_read) : nullptr;
        if (BOOST_TEST(e != nullptr)) {
            BOOST_TEST_EQ(e->topic, topic);
            BOOST_TEST_EQ(e->content, signal);
        }
    }
    BOOST_TEST(refused_unread(event_text + " ", tp::nostr::max_parsed_relay_text_bytes));

    // Trystero's handshake message: `{"__trystero_pw":"response","h":"<64 hexadecimal digits>"}`.
    const auto proof = tp::handshake_proof("ch4ll3ng3", "secret", "my-app", "room-1");
    const auto handshake =
        padded(R"({"__trystero_pw":"response","h":")" + std::string(proof) + "\"}",
               tp::max_handshake_bytes);
    {
        const auto document = parse_text(handshake, tp::max_handshake_bytes).value();
        BOOST_TEST_EQ(tp::json::text_of(document.root()["h"]), proof);
    }
    BOOST_TEST(refused_unread(handshake + " ", tp::max_handshake_bytes));
}

/** `["EVENT","sub",<event>]` whose event holds `members` more after its tags and content. */
std::string event_with(std::string_view members) {
    std::string text = R"(["EVENT","sub",{"id":"i","tags":[["x","topic"]],"content":"c")";
    text += members;
    return text + "}]";
}

// No text costs a room more than a few times its bound to read. An event of 256 distinct keys of
// 128 bytes, which collided in Boost.JSON's hashed objects (chapter 18's draft), is read as any
// text is: document's objects are ordered indices. A text of thousands of values, each a node of
// some 200 bytes of image for 2 of text, is refused once its image fills the bound; and what the
// parse took from the resource, its doubling buffers, sums to less than three times the bound.
void parse_cost() {
    const auto limits = tp::json::protocol_limits(tp::nostr::max_parsed_relay_text_bytes);
    const auto bound = static_cast<std::size_t>(limits.max_document_bytes);

    std::string keys;
    for (std::size_t i = 0; i < 256; ++i) {
        std::string key = std::to_string(i);
        key.resize(128, 'k');
        keys += ",\"" + key + "\":0";
    }
    counting_resource keyed;
    {
        const auto document = tp::json::parse(event_with(keys), limits, &keyed);
        if (BOOST_TEST(document.has_value())) {
            BOOST_TEST_EQ(document->root()[2].size(), 3U + 256U);
            const auto message = tp::nostr::read_relay_message(document->root());
            const auto* e =
                message.has_value() ? std::get_if<tp::nostr::event>(&*message) : nullptr;
            BOOST_TEST(e != nullptr && e->topic == "topic" && e->content == "c");
        }
    }
    BOOST_TEST_LE(keyed.largest, bound);
    BOOST_TEST_LT(keyed.allocated, 3 * bound);
    BOOST_TEST_EQ(keyed.outstanding, 0U);

    const auto values = [](std::size_t n) {
        std::string text = R"(,"values":[0)";
        for (std::size_t i = 1; i < n; ++i) {
            text += ",0";
        }
        return event_with(text + "]");
    };
    counting_resource few;
    BOOST_TEST(tp::json::parse(values(100), limits, &few).has_value());
    // 4,000 values in 8 KB of text, and the most the bound reads: some 34,000 in 68 KB.
    const auto longest =
        values(((tp::nostr::max_parsed_relay_text_bytes - event_with("").size()) / 2) - 8);
    BOOST_TEST_LE(longest.size(), tp::nostr::max_parsed_relay_text_bytes);
    for (const auto& text : {values(4000), longest}) {
        counting_resource many;
        const auto refused = tp::json::parse(text, limits, &many);
        BOOST_TEST(!refused.has_value() && refused.error() == wd::errc::capacity);
        BOOST_TEST_EQ(many.largest, bound);
        BOOST_TEST_LT(many.allocated, 3 * bound);
        BOOST_TEST_EQ(many.outstanding, 0U);
    }
}

// A text nested past max_depth is refused at once: Boost.JSON's parser stops at the 257th level,
// which the parse does not take for a full buffer, so it makes no larger one to try again. The
// texts are padded so that their first buffer holds the 256 levels a room reads; unpadded, the
// same nesting fills the first buffers, which do grow.
void parse_depth() {
    const auto limits = tp::json::protocol_limits(tp::nostr::max_parsed_relay_text_bytes);
    const auto depth = static_cast<std::size_t>(limits.max_depth);
    BOOST_TEST_EQ(depth, 256U);
    const auto nested = [](std::size_t levels, std::size_t size) {
        std::string text(size - (2 * levels), ' ');
        text.append(levels, '[');
        text.append(levels, ']');
        return text;
    };
    const std::size_t size = tp::nostr::max_parsed_relay_text_bytes;
    counting_resource deepest;
    BOOST_TEST(tp::json::parse(nested(depth, size), limits, &deepest).has_value());
    BOOST_TEST_EQ(deepest.allocations, 1U);
    counting_resource deeper;
    const auto refused = tp::json::parse(nested(depth + 1, size), limits, &deeper);
    BOOST_TEST(!refused.has_value() && refused.error() == wd::errc::capacity);
    BOOST_TEST_EQ(deeper.allocations, 1U);
    BOOST_TEST_EQ(deeper.outstanding, 0U);
    counting_resource growing;
    BOOST_TEST(tp::json::parse(nested(depth, 2 * depth), limits, &growing).has_value());
    BOOST_TEST_GT(growing.allocations, 1U);
}

// A parse's image is one buffer of the resource it is given, the only thing it holds there once
// the parse succeeds: the strings read from it point into it, a move keeps it where it is, and the
// resource has nothing out once the document goes.
void parse_resource() {
    constexpr std::string_view text =
        R"(["EVENT","sub",{"id":"abc","tags":[["x","topic-a"]],"content":"{\"peerId\":\"p\"}"}])";
    counting_resource memory;
    {
        auto parsed = parse_text(text, tp::nostr::max_parsed_relay_text_bytes, &memory);
        if (!BOOST_TEST(parsed.has_value())) {
            return;
        }
        tp::json::document document = std::move(parsed).value();
        BOOST_TEST(document.resource() == &memory);
        BOOST_TEST_EQ(memory.outstanding, 1U);
        BOOST_TEST_EQ(memory.last_bytes, document.capacity());
        const auto image = document.get().image();
        BOOST_TEST(static_cast<const void*>(image.data()) == memory.last);
        BOOST_TEST_EQ(image.size(), document.capacity());
        const auto* begin = reinterpret_cast<const char*>(image.data());
        const auto* end = begin + image.size();
        const auto inside = [begin, end](std::string_view s) {
            return std::less_equal<>()(begin, s.data()) &&
                   std::less_equal<>()(s.data() + s.size(), end);
        };
        const auto message = tp::nostr::read_relay_message(std::as_const(document).root());
        const auto* e = message.has_value() ? std::get_if<tp::nostr::event>(&*message) : nullptr;
        if (BOOST_TEST(e != nullptr)) {
            BOOST_TEST_EQ(e->content, R"({"peerId":"p"})");
            BOOST_TEST(inside(e->subscription) && inside(e->id) && inside(e->topic) &&
                       inside(e->content));
        }
        const tp::json::document moved = std::move(document);
        // A document moved from holds no image.
        BOOST_TEST(!document.has_value());  // NOLINT(bugprone-use-after-move)
        const auto moved_image = moved.get().image();
        BOOST_TEST(static_cast<const void*>(moved_image.data()) == memory.last);
        BOOST_TEST_EQ(tp::json::text_of(moved.root()[2]["content"]), R"({"peerId":"p"})");
        BOOST_TEST_EQ(memory.allocations, 1U);
        BOOST_TEST_EQ(memory.outstanding, 1U);
    }
    BOOST_TEST_EQ(memory.outstanding, 0U);
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
    if (!BOOST_TEST(loaded.has_value())) {
        return boost::report_errors();
    }
    const auto fixture = loaded->root();
    topics(fixture);
    relays(fixture);
    handshake_and_sealing(fixture);
    nostr_events(fixture);
    nostr_message_fields(fixture);
    event_ids_are_their_own(fixture);
    inline_frames(fixture);
    chunked_frames(fixture);
    envelopes(fixture);
    offer_sizes_at_two_to_the_53();
    wide_fields_are_big_endian();
    signals();
    random_ids();
    signal_bounds();
    event_messages(fixture);
    signal_texts(fixture);
    duplicate_keys(fixture);
    parse_bounds();
    parse_cost();
    parse_depth();
    parse_resource();
    text();
    return boost::report_errors();
}
