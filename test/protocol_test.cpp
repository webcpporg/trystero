// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the protocol layer against values Trystero JS 0.27.0 computed itself, which the original's
// oracle wrote into fixtures/oracle/expected.json. The fixture's path is the test's one argument,
// as webcpp.run passes its input file. The fixture is read with webcpp::document, and every text
// the protocol writes is read back as a room reads it, with webcpp::document::parse into a
// pmr::document, within protocol::json::protocol_limits of its bound.

#include <webcpp/trystero/protocol/action_wire.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/nostr.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/room_keys.hpp>
#include <webcpp/trystero/protocol/seal.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/storage.hpp>
#include <webcpp/document/value.hpp>

#include <boost/core/lightweight_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory_resource>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bookkeeping.hpp"

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

/** A protocol text read as a room reads it: at most `max_bytes`, into a document of `memory`. */
wd::result<wd::pmr::document> parse_text(
    std::string_view text, std::size_t max_bytes,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return wd::parse(text, tp::json::protocol_limits(max_bytes),
                     std::pmr::polymorphic_allocator<std::byte>(memory));
}

/**
 * A resource that counts what it gives: its allocations, their bytes, the most bytes out at once,
 * and what is still out, by address.
 */
class counting_resource final : public std::pmr::memory_resource {
public:
    /** How many allocations it made, a standard container's own (bookkeeping.hpp) left out. */
    std::size_t allocations = 0;
    /** The bytes of its largest allocation. */
    std::size_t largest = 0;
    /** The bytes out now. */
    std::size_t bytes_out = 0;
    /** The most bytes that were out at once. */
    std::size_t peak = 0;
    /** The allocations not yet returned, with their bytes. */
    std::map<const void*, std::size_t> live;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (!webcpp::test::is_bookkeeping(bytes, alignment)) {
            ++allocations;
        }
        largest = std::max(largest, bytes);
        bytes_out += bytes;
        peak = std::max(peak, bytes_out);
        void* const p = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        live.emplace(p, bytes);
        return p;
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        bytes_out -= bytes;
        live.erase(p);
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

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
        BOOST_TEST_EQ(integer(e["kind"]), std::int64_t{tp::nostr::topic_kind(str(e["topic"]))});
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
    const auto message =
        tp::nostr::event_message(*key, std::chrono::sys_seconds(std::chrono::seconds(1759700000)),
                                 "topic-a", R"({"peerId":"p"})")
            .value();
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
// fields. A subscription lists each kind once, in the order of its first topic, and the topics as
// given.
void nostr_message_fields(const fixture_value& fixture) {
    const auto first = fixture["nostr_events"][0];
    const auto topic = str(first["topic"]);
    const std::int64_t kind = tp::nostr::topic_kind(topic);
    // A topic of another kind.
    const auto other_topic = str(fixture["event_messages"]["events"][0]["topic"]);
    const std::int64_t other_kind = tp::nostr::topic_kind(other_topic);
    BOOST_TEST_NE(kind, other_kind);
    std::array<std::uint8_t, 32> secret{};
    secret.fill(7);
    const auto key = tp::nostr::signer::from_secret(secret);
    if (!BOOST_TEST(key.has_value())) {
        return;
    }
    for (const std::int64_t seconds : {integer(first["created_at"]), std::int64_t{4'354'000'000}}) {
        const auto at = std::chrono::sys_seconds(std::chrono::seconds(seconds));
        const auto message = tp::nostr::event_message(*key, at, topic, R"({"peerId":"p"})").value();
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
        BOOST_TEST_EQ(tp::json::text_of(event["pubkey"]), key->pubkey_hex());
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
        // Once each, in the order of their first topic, as Trystero's `new Set` keeps them.
        const std::array<std::int64_t, 2> first_seen{kind, other_kind};
        BOOST_TEST_ALL_EQ(kinds.begin(), kinds.end(), first_seen.begin(), first_seen.end());
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
    const auto at = std::chrono::sys_seconds(std::chrono::seconds(1759700000));
    const std::string message(
        tp::nostr::event_message(*key, at, "topic-a", R"({"peerId":"p"})").value());
    const std::string kind = R"("kind":)" + std::to_string(tp::nostr::topic_kind("topic-a"));
    const std::string other_kind =
        R"("kind":)" + std::to_string(tp::nostr::topic_kind("topic-a") + 1);
    const std::string created_at = R"("created_at":1759700000)";
    const std::string forged_from(
        tp::nostr::event_message(*key, at, "topic-a", R"({"peerId":"forged"})").value());
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
    BOOST_TEST(!own_id(replaced(message, key->pubkey_hex(), uppercase(key->pubkey_hex()))));
    BOOST_TEST(!own_id(replaced(message, id_in(message), uppercase(id_in(message)))));
}

// The frames of Trystero 0.27.0's action wire are proven against its recorded frames in
// crypto_layer_test.cpp (action_frames_match); these are the rules no recorded frame shows.
void action_names() {
    BOOST_TEST(tp::valid_action_name("abcdefghijklmnopqrstuvwxyz012345"));
    BOOST_TEST(!tp::valid_action_name("abcdefghijklmnopqrstuvwxyz0123456"));
    BOOST_TEST(!tp::valid_action_name(""));
    BOOST_TEST(!tp::valid_action_name(std::string_view("a\0b", 3)));
}

// Only a chunk may be longer than 16 KiB, up to 64 KiB, and only an offer may carry the wide flag,
// as 0.27.0's handleData checks: any other frame, or any other flag, is invalid.
void wide_bulk_frames_are_bounded() {
    const auto is_invalid = [](const tp::bytes& frame) {
        const auto decoded = tp::decode_action(frame);
        return !decoded.frame.has_value() && decoded.error == tp::wire_error::invalid_frame;
    };
    const auto wide_offer =
        tp::encode_offer("file", tp::payload_format::binary, "", 1, 200'000, tp::chunk_width::wide);
    BOOST_TEST_EQ(wide_offer[1], 0x61U);
    const auto offer = tp::decode_action(wide_offer);
    if (BOOST_TEST(offer.frame.has_value())) {
        BOOST_TEST(std::get<tp::proposal>(*offer.frame).wide);
    }
    BOOST_TEST(!std::get<tp::proposal>(
                    *tp::decode_action(
                         tp::encode_offer("file", tp::payload_format::binary, "", 1, 200'000))
                         .frame)
                    .wide);
    auto high_flag = wide_offer;
    high_flag[1] = static_cast<std::uint8_t>(high_flag[1] | 0x80U);
    BOOST_TEST(is_invalid(high_flag));
    auto flagged_inline = tp::encode_inline("chat", tp::payload_format::text, "", tp::bytes{65});
    flagged_inline[1] = static_cast<std::uint8_t>(flagged_inline[1] | tp::wide_bulk_flag);
    BOOST_TEST(is_invalid(flagged_inline));
    auto flagged_accepted = tp::encode_accepted(1);
    flagged_accepted[1] = static_cast<std::uint8_t>(flagged_accepted[1] | tp::wide_bulk_flag);
    BOOST_TEST(is_invalid(flagged_accepted));

    const tp::bytes wide_data(tp::wide_chunk_size, 7);
    const auto widest = tp::encode_chunk(1, 0, wide_data);
    BOOST_TEST_EQ(widest.size(), tp::max_bulk_frame_bytes);
    const auto chunk = tp::decode_action(widest);
    if (BOOST_TEST(chunk.frame.has_value())) {
        BOOST_TEST_EQ(std::get<tp::chunk>(*chunk.frame).data.size(), tp::wide_chunk_size);
    }
    BOOST_TEST(is_invalid(tp::encode_chunk(1, 0, tp::bytes(tp::wide_chunk_size + 1, 7))));
    const tp::bytes long_payload(tp::max_action_frame_bytes - tp::inline_header_size + 1, 7);
    BOOST_TEST(is_invalid(tp::encode_inline("chat", tp::payload_format::binary, "", long_payload)));
}

void signals() {
    std::optional<wd::pmr::document> document;  // the views read_signal returns point into it
    const auto read = [&document](std::string_view text) -> std::optional<tp::signal_message> {
        auto parsed = parse_text(text, tp::max_parsed_signal_bytes);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        return tp::read_signal(std::as_const(document.emplace(std::move(parsed).value())).root());
    };
    const auto announce = read(R"({"peerId":"peerAAAAAAAAAAAAAAAA","nonce":"x1"})");
    if (!BOOST_TEST(announce.has_value())) {
        return;
    }
    BOOST_TEST(announce->is_announcement());
    BOOST_TEST(announce->offer_id.empty());
    BOOST_TEST(!read(R"({"peerId":"peerAAAAAAAAAAAAAAAA","offerId":"o","offer":""})").has_value());
    BOOST_TEST(!read(R"({"offerId":"o","offer":"x"})").has_value());
    BOOST_TEST(!read(R"(["peerId"])").has_value());
    // 0.27.0's rules: a peer id of 20 base62 characters, an offer id of 1 to 64 of [\w.~:!-],
    // required with an offer, answer or candidate and refusing whenever present and not one.
    BOOST_TEST(!read(R"({"peerId":"abc"})").has_value());
    BOOST_TEST(!read(R"({"peerId":"peerAAAAAAAAAAAAAAA-"})").has_value());
    BOOST_TEST(!read(R"({"peerId":"peerAAAAAAAAAAAAAAAA","offer":"x"})").has_value());
    BOOST_TEST(!read(R"({"peerId":"peerAAAAAAAAAAAAAAAA","offerId":""})").has_value());
    BOOST_TEST(!read(R"({"peerId":"peerAAAAAAAAAAAAAAAA","offerId":5})").has_value());
    BOOST_TEST(
        !read(R"({"peerId":"peerAAAAAAAAAAAAAAAA","offerId":"a b","answer":"x"})").has_value());
    const auto with_id = read(R"({"peerId":"peerAAAAAAAAAAAAAAAA","offerId":"a_.~:!-Z9"})");
    BOOST_TEST(with_id.has_value() && with_id->offer_id == "a_.~:!-Z9");
    const auto written = tp::write_signal(
        {.peer_id = "peerAAAAAAAAAAAAAAAA", .offer_id = "o1", .candidate = "1,2$AA=="}, "n0nce");
    BOOST_TEST_EQ(written,
                  R"({"peerId":"peerAAAAAAAAAAAAAAAA","offerId":"o1","candidate":"1,2$AA==",)"
                  R"("nonce":"n0nce"})");
    const auto back = read(written);
    if (BOOST_TEST(back.has_value())) {
        BOOST_TEST_EQ(back->candidate, "1,2$AA==");
        BOOST_TEST_EQ(back->offer_id, "o1");
    }
    // Escaping matches JSON.stringify, so a sealed text or a candidate with quotes survives the
    // round trip. The expected text is named outside the macro: MSVC 14.3's preprocessor, which
    // stringizes the macro's arguments, reads a raw string's \" as an escape and stops (C2017).
    constexpr std::string_view escaped = R"({"peerId":"q\"\\\n"})";
    BOOST_TEST_EQ(tp::write_signal({.peer_id = "q\"\\\n"}), escaped);
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
// oracle's frames have a short metadata, small ids, offsets and sizes.
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
        3, 2, 0x0a, 0x0b, 0x0c, 0x0d, 0x41, 0xb2, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 9,
    };
    BOOST_TEST_ALL_EQ(chunk.begin(), chunk.end(), expected_chunk.begin(), expected_chunk.end());
    const auto c = std::get<tp::chunk>(*tp::decode_action(chunk).frame);
    BOOST_TEST_EQ(c.id, 0x0a0b0c0dU);
    BOOST_TEST_EQ(c.offset, 305419896.0);
    const auto accepted = tp::encode_accepted(0xfffefdfc);
    const tp::bytes expected_accepted{3, 3, 0xff, 0xfe, 0xfd, 0xfc};
    BOOST_TEST_ALL_EQ(accepted.begin(), accepted.end(), expected_accepted.begin(),
                      expected_accepted.end());
}

// Each field of a signal is bounded as 0.27.0 bounds it, in UTF-16 code units: a candidate of
// 3044 bytes of plaintext, sealed, is the longest read; a text that is not ASCII counts as
// JavaScript counts it, a character beyond U+FFFF two units.
void signal_bounds() {
    std::optional<wd::pmr::document> document;
    const auto read = [&document](const tp::signal_message& m) {
        auto parsed = parse_text(tp::write_signal(m), tp::max_parsed_signal_bytes);
        if (!parsed.has_value()) {
            return false;
        }
        return tp::read_signal(std::as_const(document.emplace(std::move(parsed).value())).root())
            .has_value();
    };
    constexpr std::string_view peer = "peerAAAAAAAAAAAAAAAA";
    const auto key = tp::room_aes_key_without_password("my-app", "room-1");
    const std::array<std::uint8_t, tp::gcm_iv_size> iv{};
    const auto sealed_candidate = [&](std::size_t plaintext) {
        return std::string(tp::seal(key, iv, std::string(plaintext, 'c'),
                                    std::string_view("candidate|peerAAAAAAAAAAAAAAAA|o"))
                               .value());
    };
    const auto longest_candidate = sealed_candidate(3044);
    BOOST_TEST_EQ(longest_candidate.size(), tp::max_sealed_candidate_chars);
    BOOST_TEST(read({.peer_id = peer, .offer_id = "o", .candidate = longest_candidate}));
    BOOST_TEST(!read({.peer_id = peer, .offer_id = "o", .candidate = sealed_candidate(3045)}));

    const auto of = [](std::size_t n) { return std::string(n, 'a'); };
    const auto candidate = of(tp::max_sealed_candidate_chars);
    const auto description = of(tp::max_sealed_description_chars);
    const auto offer_id = of(tp::max_offer_id_chars);
    BOOST_TEST(read({.peer_id = peer, .offer_id = "o", .candidate = candidate}));
    BOOST_TEST(!read({.peer_id = peer, .offer_id = "o", .candidate = candidate + "a"}));
    BOOST_TEST(read({.peer_id = peer, .offer_id = "o", .offer = description}));
    BOOST_TEST(!read({.peer_id = peer, .offer_id = "o", .offer = description + "a"}));
    BOOST_TEST(read({.peer_id = peer, .offer_id = "o", .answer = description}));
    BOOST_TEST(!read({.peer_id = peer, .offer_id = "o", .answer = description + "a"}));
    BOOST_TEST(read({.peer_id = peer, .offer_id = offer_id, .offer = "x"}));
    BOOST_TEST(!read({.peer_id = peer, .offer_id = offer_id + "a", .offer = "x"}));
    // U+00E9 is 2 bytes and 1 unit; U+1F600 is 4 bytes and 2 units.
    const auto units = [](std::size_t n, std::string_view character) {
        std::string out;
        for (std::size_t i = 0; i < n; ++i) {
            out += character;
        }
        return out;
    };
    BOOST_TEST(read({.peer_id = peer, .offer_id = "o", .candidate = units(4096, "\xc3\xa9")}));
    BOOST_TEST(!read({.peer_id = peer, .offer_id = "o", .candidate = units(4097, "\xc3\xa9")}));
    BOOST_TEST(
        read({.peer_id = peer, .offer_id = "o", .candidate = units(2048, "\xf0\x9f\x98\x80")}));
    BOOST_TEST(!read(
        {.peer_id = peer, .offer_id = "o", .candidate = units(2048, "\xf0\x9f\x98\x80") + "a"}));
}

// Trystero's createEvent signed each of the oracle's events with its key, clock and auxiliary
// bytes: its topic and content hold every control character, `"`, `\`, U+2028, U+2029, DEL and
// astral characters. Each kind is the one the protocol gives the topic, each id the one it computes
// over the event's fields, each signature verifies, and the signer made from the oracle's secret
// has the event's public key. An event the protocol writes for the same topic and content holds the
// same fields, an id of its own and a signature that verifies.
void event_messages(const fixture_value& fixture) {
    const auto messages = fixture["event_messages"];
    const auto secret = hex(messages["secret_key"]);
    if (!BOOST_TEST_EQ(secret.size(), 32U)) {
        return;
    }
    const auto key =
        tp::nostr::signer::from_secret(std::span<const std::uint8_t, 32>(secret.data(), 32));
    if (!BOOST_TEST(key.has_value())) {
        return;
    }
    const auto created_at =
        std::chrono::sys_seconds(std::chrono::seconds(integer(messages["created_at"])));
    // Whether an event's signature verifies under its public key, over its id.
    const auto verifies = [](const tp::json::const_value& event) {
        const auto id = tp::from_hex(tp::json::text_of(event["id"])).value_or(tp::bytes());
        const auto sig = tp::from_hex(tp::json::text_of(event["sig"])).value_or(tp::bytes());
        const auto pub = tp::from_hex(tp::json::text_of(event["pubkey"])).value_or(tp::bytes());
        return id.size() == 32 && sig.size() == 64 && pub.size() == 32 &&
               tp::nostr::verify(std::span<const std::uint8_t, 64>(sig.data(), 64),
                                 std::span<const std::uint8_t, 32>(id.data(), 32),
                                 std::span<const std::uint8_t, 32>(pub.data(), 32));
    };
    BOOST_TEST_EQ(messages["events"].size(), 6U);
    for (const auto e : messages["events"]) {
        const auto topic = str(e["topic"]);
        const auto content = str(e["content"]);
        const auto theirs = parse_text(str(e["message"]), tp::nostr::max_parsed_relay_text_bytes);
        if (!BOOST_TEST(theirs.has_value())) {
            continue;
        }
        const auto event = theirs->root()[1];
        BOOST_TEST_EQ(tp::json::text_of(event["pubkey"]), key->pubkey_hex());
        const auto kind = static_cast<std::uint32_t>(integer_of(event["kind"]));
        BOOST_TEST_EQ(kind, tp::nostr::topic_kind(topic));
        const auto id = tp::nostr::event_id(key->pubkey_hex(), created_at, kind, topic, content);
        BOOST_TEST_EQ(tp::to_hex(id), str(e["id"]));
        BOOST_TEST(verifies(event));

        const auto written = tp::nostr::event_message(*key, created_at, topic, content);
        if (!BOOST_TEST(written.has_value())) {
            continue;
        }
        const auto ours = parse_text(*written, tp::nostr::max_parsed_relay_text_bytes);
        if (!BOOST_TEST(ours.has_value())) {
            continue;
        }
        const auto mine = ours->root()[1];
        BOOST_TEST_EQ(integer_of(mine["kind"]), std::int64_t{tp::nostr::topic_kind(topic)});
        BOOST_TEST_EQ(tp::json::text_of(mine["tags"][0][1]), topic);
        BOOST_TEST_EQ(tp::json::text_of(mine["content"]), content);
        BOOST_TEST_EQ(integer_of(mine["created_at"]), integer_of(event["created_at"]));
        BOOST_TEST(own_id(*written));
        BOOST_TEST(verifies(mine));
    }
}

// A signal's text is JSON.stringify's of 0.27.0's object, member for member, with the same
// strings, every byte of their quoting included, and the protocol reads the same fields back
// where 0.27.0's signal handler read the signal; it refused the three whose peer ids are none.
void signal_texts(const fixture_value& fixture) {
    BOOST_TEST_EQ(fixture["signal_texts"].size(), 7U);
    std::size_t read_back = 0;
    for (const auto entry : fixture["signal_texts"]) {
        const auto s = entry["signal"];
        const tp::signal_message m{
            .peer_id = str(s["peerId"]),
            .offer_id = str(s["offerId"]),
            .offer = str(s["offer"]),
            .answer = str(s["answer"]),
            .candidate = str(s["candidate"]),
        };
        const auto text = str(entry["text"]);
        BOOST_TEST_EQ(tp::write_signal(m, str(s["nonce"])), text);
        const auto document = parse_text(text, tp::max_parsed_signal_bytes).value();
        const auto read = tp::read_signal(document.root());
        // Read back where 0.27.0 read it, refused where its ids are none of 0.27.0's.
        if (!BOOST_TEST_EQ(read.has_value(), !entry["read"].is_null()) || !read.has_value()) {
            continue;
        }
        ++read_back;
        BOOST_TEST_EQ(read->peer_id, m.peer_id);
        BOOST_TEST_EQ(read->offer_id, m.offer_id);
        BOOST_TEST_EQ(read->offer, m.offer);
        BOOST_TEST_EQ(read->answer, m.answer);
        BOOST_TEST_EQ(read->candidate, m.candidate);
    }
    BOOST_TEST_EQ(read_back, 4U);
}

/** `text` read as an action of JSON reads its payload, then written back as one sends it: once
 * into an owned document, as a webcpp::document::document, and once into a resource, as a
 * webcpp::document::pmr::document; the two writings, which must agree. */
std::pmr::string payload_written(std::string_view text) {
    const auto owned = wd::parse(text);
    const auto in_resource =
        wd::parse(text, wd::parse_options(),
                  std::pmr::polymorphic_allocator<std::byte>(std::pmr::get_default_resource()));
    if (!BOOST_TEST(owned.has_value() && in_resource.has_value())) {
        return {};
    }
    std::pmr::string written;
    tp::json::append_value(written, owned->root());
    std::pmr::string written_in_resource;
    tp::json::append_value(written_in_resource, in_resource->root());
    BOOST_TEST_EQ(written, written_in_resource);
    return written;
}

// A JSON payload's text is JSON.stringify's of the same value for every value a double holds: its
// numbers as JavaScript writes them, its keys in JavaScript's order, array indices first in
// increasing order. A number a double cannot hold is written exact, as webcpp::document holds it,
// where Trystero JS writes the nearest double, or null for Infinity.
void payload_texts(const fixture_value& fixture) {
    BOOST_TEST_EQ(fixture["payload_texts"].size(), 5U);
    for (const auto entry : fixture["payload_texts"]) {
        BOOST_TEST_EQ(payload_written(str(entry["text"])), str(entry["written"]));
    }
    const auto beyond = fixture["payload_beyond_doubles"];
    if (!BOOST_TEST_EQ(beyond.size(), 1U)) {
        return;
    }
    const auto written = payload_written(str(beyond[0]["text"]));
    BOOST_TEST_NE(written, str(beyond[0]["written"]));
    BOOST_TEST_EQ(written,
                  "[9007199254740993,-9223372036854775808,0.1000000000000000055511151231257827,"
                  "1e+400]");
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

/**
 * Each signal of an oracle's list is read as 0.27.0's own signal handler read it: refused where
 * `read` is null, otherwise with the same peer id and sealed texts, and the offer id of what it
 * opened.
 */
void signals_read_as_0_27_0_read(const fixture_value& signals) {
    for (const auto entry : signals) {
        const auto document = parse_text(str(entry["text"]), tp::max_parsed_signal_bytes).value();
        const auto read = tp::read_signal(document.root());
        const auto expected = entry["read"];
        if (!BOOST_TEST_EQ(read.has_value(), !expected.is_null())) {
            std::cerr << "  the signal: " << str(entry["text"]).substr(0, 100) << '\n';
        }
        if (!read.has_value() || expected.is_null()) {
            continue;
        }
        BOOST_TEST_EQ(read->peer_id, str(expected["peer_id"]));
        BOOST_TEST_EQ(read->offer, str(expected["offer"]));
        BOOST_TEST_EQ(read->answer, str(expected["answer"]));
        BOOST_TEST_EQ(read->candidate, str(expected["candidate"]));
        // 0.27.0 shows the offer id of what it opens; an announcement's it never reads.
        if (expected.contains("offer_id")) {
            BOOST_TEST_EQ(read->offer_id, str(expected["offer_id"]));
        }
    }
}

/** Each relay event of an oracle's list is acted on, with the same sender, exactly when 0.27.0's
 * room acted on it. */
void events_acted_on_as_0_27_0_acted(const fixture_value& events, std::string_view root_topic) {
    for (const auto entry : events) {
        BOOST_TEST_EQ(answer_sender(str(entry["text"]), root_topic), str(entry["acted"]));
    }
}

// A key written twice keeps its last value, as JSON.parse keeps it, an escaped key included: a
// signal reads what 0.27.0's own signal handler read from the same text, and a relay's event is
// acted on, with the same sender, exactly when 0.27.0's room acted on it.
void duplicate_keys(const fixture_value& fixture) {
    const auto cases = fixture["duplicate_keys"];
    BOOST_TEST_EQ(cases["signals"].size(), 13U);
    signals_read_as_0_27_0_read(cases["signals"]);
    BOOST_TEST_EQ(cases["events"].size(), 6U);
    events_acted_on_as_0_27_0_acted(cases["events"], str(cases["root_topic"]));
}

// A room refuses every signal 0.27.0's parseSignalEnvelope refuses, as 0.27.0's handler and room
// showed on each: a peer id that is not 20 base62 characters, or no string; an offer id that is not
// 1 to 64 characters of [\w.~:!-], present or needed by an offer, answer or candidate; a candidate
// past 4,096 UTF-16 code units, a character beyond U+FFFF two. The last text of each bound is read.
void signal_envelopes(const fixture_value& fixture) {
    const auto cases = fixture["signal_envelopes"];
    BOOST_TEST_EQ(cases["signals"].size(), 26U);
    signals_read_as_0_27_0_read(cases["signals"]);
    BOOST_TEST_EQ(cases["events"].size(), 8U);
    events_acted_on_as_0_27_0_acted(cases["events"], str(cases["root_topic"]));
}

/** `text` with spaces after its value, which JSON allows, up to `size` bytes. */
std::string padded(std::string_view text, std::size_t size) {
    std::string out(text);
    out.resize(size, ' ');
    return out;
}

/** The longest signal the bounds of its fields admit: a sealed answer, whose key is the longest. */
std::string longest_signal() {
    const std::string peer(tp::peer_id_size, 'p');
    const std::string offer_id(tp::max_offer_id_chars, 'o');
    const std::string description(tp::max_sealed_description_chars, 'a');
    return std::string(
        tp::write_signal({.peer_id = peer, .offer_id = offer_id, .answer = description}));
}

// The bounds a room parses with refuse no text Trystero sends, and the relay's is under the length
// that closes a relay's connection, so that a text between the two is dropped and the room goes on.
static_assert(tp::max_parsed_signal_bytes == tp::max_sealed_description_chars + 1024);
static_assert(tp::max_parsed_signal_bytes == 66560);
static_assert(tp::max_sealed_description_chars + tp::peer_id_size + tp::max_offer_id_chars + 64 <=
              tp::max_parsed_signal_bytes);
static_assert(tp::nostr::max_parsed_relay_text_bytes == tp::max_parsed_signal_bytes + 2048);
static_assert(tp::nostr::max_parsed_relay_text_bytes == 68608);
static_assert(tp::nostr::max_parsed_relay_text_bytes < tp::max_relay_message_bytes);
// A parse reads at most its bound of text, into an image of at most twice that and 64 KiB more.
static_assert(tp::json::protocol_limits(1000).max_bytes == 1000);
static_assert(tp::json::protocol_limits(1000).max_document_bytes == 2000 + 65536);
// A protocol text nests at most 16 deep; a payload as deep as document's default.
static_assert(tp::json::protocol_limits(1000).max_depth == 16);
static_assert(tp::json::payload_limits(1000, std::uint64_t{64} << 20U).max_depth ==
              wd::parse_options().max_depth);
// A payload's image grows to 96 times its text and 64 KiB, at most the room's bound.
static_assert(tp::json::payload_limits(1000, std::uint64_t{64} << 20U).max_bytes == 1000);
static_assert(tp::json::payload_limits(1000, std::uint64_t{64} << 20U).max_document_bytes ==
              96000 + 65536);
static_assert(tp::json::payload_limits(1000, 100000).max_document_bytes == 100000);
static_assert(tp::json::payload_limits(1'000'000, std::uint64_t{64} << 20U).max_document_bytes ==
              std::uint64_t{64} << 20U);
static_assert(tp::json::payload_limits(0, 1000).max_document_bytes == 1000);

// Each text a room parses is read up to its bound, to the byte, and one byte more is refused
// before anything of it is read: no buffer is made, and a text that is not JSON at all is refused
// for its length, not its syntax. The relay's bound reads an event that carries the longest
// signal, which the signal's bound reads in turn.
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
        BOOST_TEST(read.has_value() && read->answer.size() == tp::max_sealed_description_chars &&
                   read->peer_id.size() == tp::peer_id_size &&
                   read->offer_id.size() == tp::max_offer_id_chars);
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
    const auto topic = tp::peer_topic("my-app", "room-1", std::string(tp::peer_id_size, 'p'));
    const auto message =
        tp::nostr::event_message(*key, std::chrono::sys_seconds(std::chrono::seconds(1759700000)),
                                 topic, signal)
            .value();
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
// some 200 bytes of image for 2 of text, is refused once its image has grown to the bound; and the
// resource never has more out at once than the image grown to the bound, with the image it grew
// from.
void parse_cost() {
    const auto limits = tp::json::protocol_limits(tp::nostr::max_parsed_relay_text_bytes);
    const auto bound = static_cast<std::size_t>(limits.max_document_bytes);
    const auto parse = [&limits](std::string_view text, counting_resource& memory) {
        return wd::parse(text, limits, std::pmr::polymorphic_allocator<std::byte>(&memory));
    };

    std::string keys;
    for (std::size_t i = 0; i < 256; ++i) {
        std::string key = std::to_string(i);
        key.resize(128, 'k');
        keys += ",\"" + key + "\":0";
    }
    counting_resource keyed;
    {
        const auto document = parse(event_with(keys), keyed);
        if (BOOST_TEST(document.has_value())) {
            BOOST_TEST_EQ(document->root()[2].size(), 3U + 256U);
            const auto message = tp::nostr::read_relay_message(document->root());
            const auto* e =
                message.has_value() ? std::get_if<tp::nostr::event>(&*message) : nullptr;
            BOOST_TEST(e != nullptr && e->topic == "topic" && e->content == "c");
        }
    }
    BOOST_TEST_LE(keyed.largest, bound);
    BOOST_TEST_LT(keyed.peak, 2 * bound);
    BOOST_TEST(keyed.live.empty());

    const auto values = [](std::size_t n) {
        std::string text = R"(,"values":[0)";
        for (std::size_t i = 1; i < n; ++i) {
            text += ",0";
        }
        return event_with(text + "]");
    };
    counting_resource few;
    BOOST_TEST(parse(values(100), few).has_value());
    // 4,000 values in 8 KB of text, and the most the bound reads: some 34,000 in 68 KB.
    const auto longest =
        values(((tp::nostr::max_parsed_relay_text_bytes - event_with("").size()) / 2) - 8);
    BOOST_TEST_LE(longest.size(), tp::nostr::max_parsed_relay_text_bytes);
    for (const auto& text : {values(4000), longest}) {
        counting_resource many;
        const auto refused = parse(text, many);
        BOOST_TEST(!refused.has_value() && refused.error() == wd::errc::capacity);
        BOOST_TEST_EQ(many.largest, bound);
        BOOST_TEST_LT(many.peak, 2 * bound);
        BOOST_TEST(many.live.empty());
    }
}

// A protocol text nested past its depth is refused: Boost.JSON's parser stops at the 17th level,
// and the resource has nothing out after it.
void parse_depth() {
    const auto limits = tp::json::protocol_limits(tp::nostr::max_parsed_relay_text_bytes);
    const auto depth = static_cast<std::size_t>(limits.max_depth);
    BOOST_TEST_EQ(depth, 16U);
    const auto nested = [](std::size_t levels) {
        std::string text(levels, '[');
        text.append(levels, ']');
        return text;
    };
    counting_resource deepest;
    BOOST_TEST(
        wd::parse(nested(depth), limits, std::pmr::polymorphic_allocator<std::byte>(&deepest))
            .has_value());
    BOOST_TEST(deepest.live.empty());
    counting_resource deeper;
    const auto refused =
        wd::parse(nested(depth + 1), limits, std::pmr::polymorphic_allocator<std::byte>(&deeper));
    BOOST_TEST(!refused.has_value() && refused.error() == wd::errc::capacity);
    BOOST_TEST(deeper.live.empty());
}

// Every valid payload fits the image its own length allows, 96 bytes a byte of text: a value of one
// character with its comma or bracket, two bytes, is a node of 192, whether a number, an array of
// one or an array nested 255 deep. The factor is tight: 95 bytes a byte refuses an array of zeros.
void payload_image_per_text_byte() {
    const auto parses = [](const std::string& text, std::uint64_t image) {
        wd::parse_options o;
        o.max_document_bytes = image;
        return wd::parse(
                   text, o,
                   std::pmr::polymorphic_allocator<std::byte>(std::pmr::get_default_resource()))
            .has_value();
    };
    const auto array_of = [](std::string_view value, std::size_t n) {
        std::string text = "[";
        for (std::size_t i = 0; i < n; ++i) {
            text += i == 0 ? "" : ",";
            text += value;
        }
        return text + "]";
    };
    const std::string deep = std::string(255, '[') + std::string(255, ']');
    for (const auto& text : {
             array_of("0", 1000),
             array_of("[0]", 1000),
             array_of("[0,0]", 1000),
             array_of(deep, 10),
             array_of("{}", 1000),
             array_of("\"\"", 1000),
         }) {
        const auto n = static_cast<std::uint64_t>(text.size());
        const auto limits = tp::json::payload_limits(n, std::uint64_t{64} << 20U);
        BOOST_TEST(parses(text, limits.max_document_bytes));
        // Within 1 KiB of the 96 bytes a byte: the 64 KiB the limits add are spare.
        BOOST_TEST(parses(text, (tp::json::image_bytes_per_text_byte * n) + 1024));
    }
    const std::string zeros = array_of("0", 1000);
    BOOST_TEST(!parses(zeros, ((tp::json::image_bytes_per_text_byte - 1) * zeros.size()) + 1024));
}

// A parse's image is one allocation of the resource it is given, the only thing the resource has
// out once the parse succeeds: the strings read from it point into it, a move construction takes
// it with the allocator, and the resource has nothing out once the document goes. A handle refers
// to the document object, so the moved-to document is read through its own.
void parse_resource() {
    constexpr std::string_view text =
        R"(["EVENT","sub",{"id":"abc","tags":[["x","topic-a"]],"content":"{\"peerId\":\"p\"}"}])";
    counting_resource memory;
    {
        auto parsed = parse_text(text, tp::nostr::max_parsed_relay_text_bytes, &memory);
        if (!BOOST_TEST(parsed.has_value())) {
            return;
        }
        wd::pmr::document document = std::move(parsed).value();
        BOOST_TEST(document.get_allocator().resource() == &memory);
        const auto image = document.image();
        if (!BOOST_TEST_EQ(memory.live.size(), 1U)) {
            return;
        }
        BOOST_TEST(memory.live.begin()->first == static_cast<const void*>(image.data()));
        BOOST_TEST_EQ(memory.live.begin()->second, document.capacity());
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
        const wd::pmr::document moved = std::move(document);
        // A document moved from holds no image.
        BOOST_TEST(document.image().empty());  // NOLINT(bugprone-use-after-move)
        BOOST_TEST(moved.get_allocator().resource() == &memory);
        BOOST_TEST(static_cast<const void*>(moved.image().data()) ==
                   static_cast<const void*>(image.data()));
        BOOST_TEST_EQ(tp::json::text_of(moved.root()[2]["content"]), R"({"peerId":"p"})");
        BOOST_TEST_EQ(memory.live.size(), 1U);
    }
    BOOST_TEST(memory.live.empty());
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
        BOOST_LIGHTWEIGHT_TEST_OSTREAM << "usage: protocol_test <fixtures/oracle/expected.json>\n";
        return boost::report_errors();
    }
    const auto loaded = load(argv[1]);
    if (!BOOST_TEST(loaded.has_value())) {
        return boost::report_errors();
    }
    const auto fixture = loaded->root();
    nostr_events(fixture);
    nostr_message_fields(fixture);
    event_ids_are_their_own(fixture);
    action_names();
    wide_bulk_frames_are_bounded();
    offer_sizes_at_two_to_the_53();
    wide_fields_are_big_endian();
    signals();
    signal_bounds();
    event_messages(fixture);
    signal_texts(fixture);
    payload_texts(fixture);
    duplicate_keys(fixture);
    signal_envelopes(fixture);
    parse_bounds();
    parse_cost();
    parse_depth();
    payload_image_per_text_byte();
    parse_resource();
    text();
    return boost::report_errors();
}
