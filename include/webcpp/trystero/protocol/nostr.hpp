// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_NOSTR_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_NOSTR_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/crypto/curves/secp256k1.hpp>
#include <webcpp/crypto/error.hpp>
#include <webcpp/crypto/random_device.hpp>  // lint-world: a signer's key and aux randomness
#include <webcpp/crypto/secret.hpp>

#include <boost/hash2/sha2.hpp>
#include <boost/system/result.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace webcpp::trystero::protocol::nostr {

/**
 * The longest text of a relay a room reads, in bytes: a longer one is dropped before it is read,
 * and the connection goes on; one longer than @ref max_relay_message_bytes closes the connection
 * instead.
 *
 * The longest text measured is an event that carries an answer, 1,244 bytes, among Trystero JS in
 * Chrome and both backends. The bound is an event that carries the longest signal a room reads
 * (@ref max_parsed_signal_bytes), with 2 KiB for the event's other fields, the subscription id and
 * the escaping of the signal's quotes, which take some 600 bytes.
 */
inline constexpr std::size_t max_parsed_relay_text_bytes = max_parsed_signal_bytes + 2048;
static_assert(max_parsed_relay_text_bytes < max_relay_message_bytes);

/** The tag a Trystero event carries its topic in, its `tag`: "x". */
inline constexpr std::string_view topic_tag = "x";

/** The most topics one subscription lists, Trystero's `maxTopicsPerSubscription`: a room with
 * more subscribes in several requests. */
inline constexpr std::size_t max_topics_per_subscription = 250;

/** How far back a subscription asks for events, Trystero's `subscribeClockSkewSeconds`: a peer
 * whose clock is behind ours by up to a minute is still heard. */
inline constexpr std::chrono::seconds subscribe_clock_skew{60};

/**
 * The event kind a topic maps to, Trystero's `topicToKind`: 20000 to 29999, ephemeral, so relays
 * forward the events and keep none.
 *
 * @param topic The topic's name, in UTF-8.
 * @return FNV-1a of 32 bits over the topic's UTF-16 code units, modulo 10000, plus 20000. Unlike
 * @ref hash32 it hashes code units, not bytes, and has no final mix.
 */
inline std::uint32_t topic_kind(std::string_view topic) {
    std::uint32_t h = 0x811c9dc5U;
    for_each_utf16_code_unit(topic, [&h](std::uint32_t unit) { h = (h ^ unit) * 0x01000193U; });
    return (h % 10000U) + 20000U;
}

/**
 * The earliest `created_at` a subscription asks for, Trystero's `subscribeSince`.
 *
 * @param now The time, in whole seconds since the Unix epoch.
 * @return `now` less @ref subscribe_clock_skew, and never before the epoch.
 */
inline std::chrono::sys_seconds subscribe_since(std::chrono::sys_seconds now) {
    const auto since = now - subscribe_clock_skew;
    return std::max(since, std::chrono::sys_seconds{});
}

namespace detail {

/**
 * The id of an event whose topic and content are already JSON string literals: SHA-256 of the
 * canonical
 * `[0,"<pubkey>",<created_at>,<kind>,[["x",<topic>]],<content>]`, fed to the hash piece by piece.
 */
inline std::array<std::uint8_t, 32> event_id(std::string_view pubkey,
                                             std::chrono::sys_seconds created_at,
                                             std::uint32_t kind, std::string_view quoted_topic,
                                             std::string_view quoted_content) {
    std::array<char, 48> buffer{};
    const auto seconds = created_at.time_since_epoch().count();
    char* end = std::to_chars(buffer.data(), buffer.data() + 24, seconds).ptr;
    *end++ = ',';
    end = std::to_chars(end, buffer.data() + buffer.size(), kind).ptr;
    const std::string_view numbers(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
    boost::hash2::sha2_256 h;
    for (const std::string_view part : {
             std::string_view(R"([0,")"),
             pubkey,
             std::string_view(R"(",)"),
             numbers,
             std::string_view(R"(,[["x",)"),
             quoted_topic,
             std::string_view("]],"),
             quoted_content,
             std::string_view("]"),
         }) {
        h.update(part.data(), part.size());
    }
    const auto digest = h.result();
    std::array<std::uint8_t, 32> out{};
    std::ranges::copy(digest, out.begin());
    return out;
}

/** `text` as a JSON string literal, quotes included, in `memory`. */
inline std::pmr::string quoted(std::string_view text, std::pmr::memory_resource* memory) {
    std::pmr::string out(memory);
    out.reserve(text.size() + 8);
    json::detail::append_utf8(out, text);
    return out;
}

/** Whether `text` is 32 bytes as NIP-01 writes an id or a public key: 64 lowercase hexadecimal
 * digits. */
inline bool is_nip01_hex(std::string_view text) {
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

/** Whether `text` is the lowercase hexadecimal of `data`, as `append_hex` writes it. */
inline bool is_hex_of(std::string_view text, std::span<const std::uint8_t> data) {
    constexpr std::string_view digits = "0123456789abcdef";
    if (text.size() != data.size() * 2) {
        return false;
    }
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (text[2 * i] != digits[data[i] >> 4U] || text[(2 * i) + 1] != digits[data[i] & 15U]) {
            return false;
        }
    }
    return true;
}

}  // namespace detail

/**
 * The id of a Trystero event, as NIP-01 defines it: the SHA-256 of the JSON array
 * `[0, pubkey, created_at, kind, [["x", topic]], content]`.
 *
 * @param pubkey The signer's x-only public key, in hexadecimal.
 * @param created_at The event's time, in whole seconds since the Unix epoch.
 * @param kind The event's kind, @ref topic_kind of its topic.
 * @param topic The topic, the value of its tag "x".
 * @param content The content.
 * @param memory The resource the escaped topic and content are allocated in, during the call.
 * @return The id.
 */
inline std::array<std::uint8_t, 32> event_id(
    std::string_view pubkey, std::chrono::sys_seconds created_at, std::uint32_t kind,
    std::string_view topic, std::string_view content,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return detail::event_id(pubkey, created_at, kind, detail::quoted(topic, memory),
                            detail::quoted(content, memory));
}

/**
 * A BIP-340 key pair over webcpp::crypto's secp256k1, the key a room signs its events with, as
 * Trystero's Nostr strategy does with a room's `NostrSigner`.
 *
 * Tip: its signing context is made in the resource it is given, the key pair's one allocation.
 */
class signer {
public:
    /** How many secrets @ref generate draws before it gives up, as webcpp::crypto's
     * `keypair::generate` does with its `max_draws`. */
    static constexpr std::size_t max_key_draws = 4;

    /**
     * Makes a key from a random secret, as Trystero's `createSigner` does.
     *
     * It stands in for `webcpp::crypto::curves::secp256k1::keypair::generate`, which
     * webcpp::crypto does not offer yet, and follows its contract so that it is replaced by it
     * when that reaches main: up to @ref max_key_draws secrets of 32 bytes from the system's
     * generator, through webcpp::crypto, drawn again only when one is not a valid secret key,
     * which is negligibly rare; each is wiped once it is used.
     *
     * @param memory The resource the signing context and the public key's text are allocated in;
     * it must outlive the signer.
     * @return The signer; `webcpp::crypto::error::entropy_unavailable` when the system's generator
     * fails or every draw is refused; any other error of `keypair::make`.
     */
    [[nodiscard]] static boost::system::result<signer> generate(
        std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
        for (std::size_t draw = 0; draw < max_key_draws; ++draw) {
            webcpp::crypto::secret<32> secret;
            if (!webcpp::crypto::random_device().fill(secret.writable())) {  // lint-world: the key
                return webcpp::crypto::error::entropy_unavailable;
            }
            auto made = from_secret(secret.bytes(), memory);
            if (made || made.error() != webcpp::crypto::error::invalid_secret_key) {
                return made;
            }
        }
        return webcpp::crypto::error::entropy_unavailable;
    }

    /**
     * Makes a key from a given secret.
     *
     * @param secret The secret key.
     * @param memory The resource the signing context and the public key's text are allocated in;
     * it must outlive the signer.
     * @return The signer; `webcpp::crypto::error::invalid_secret_key` when the bytes are not a
     * valid secret key; any other error of `keypair::make`.
     */
    [[nodiscard]] static boost::system::result<signer> from_secret(
        std::span<const std::uint8_t, 32> secret,
        std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
        auto key = webcpp::crypto::curves::secp256k1::keypair::make(secret, memory);
        if (!key) {
            return key.error();
        }
        const auto xonly = key->xonly_public_key();
        if (!xonly) {
            return xonly.error();
        }
        return signer(std::move(*key), to_hex(*xonly, memory));
    }

    /**
     * The x-only public key, an event's `pubkey`.
     *
     * @return 64 lowercase hexadecimal digits, valid as long as the signer.
     */
    [[nodiscard]] std::string_view pubkey_hex() const { return pubkey_; }

    /**
     * Signs an event id with BIP-340 Schnorr, with 32 bytes of auxiliary randomness from the
     * system's generator, through webcpp::crypto, as Trystero's `schnorr.signAsync` draws its own.
     *
     * @param event_id The 32 bytes to sign.
     * @return The 64-byte signature, an event's `sig`; `webcpp::crypto::error::entropy_unavailable`
     * when the generator fails; an error of `keypair::sign_schnorr`.
     */
    [[nodiscard]] boost::system::result<std::array<std::uint8_t, 64>> sign(
        std::span<const std::uint8_t, 32> event_id) const {
        webcpp::crypto::secret<32> aux;
        if (!webcpp::crypto::random_device().fill(aux.writable())) {  // lint-world: aux randomness
            return webcpp::crypto::error::entropy_unavailable;
        }
        return key_.sign_schnorr(event_id, aux.bytes());
    }

private:
    signer(webcpp::crypto::curves::secp256k1::keypair key, std::pmr::string pubkey)
        : key_(std::move(key)), pubkey_(std::move(pubkey)) {}

    webcpp::crypto::curves::secp256k1::keypair key_;
    std::pmr::string pubkey_;
};

/**
 * Whether a BIP-340 signature over an event id verifies under an x-only public key.
 *
 * It is webcpp::crypto's `verify_schnorr`, on libsecp256k1's static context, which allocates
 * nothing.
 *
 * @param sig The signature.
 * @param id The event id.
 * @param pubkey The x-only public key.
 * @return Whether the signature is valid; false too when the key is not a valid point.
 */
inline bool verify(std::span<const std::uint8_t, 64> sig, std::span<const std::uint8_t, 32> id,
                   std::span<const std::uint8_t, 32> pubkey) {
    return webcpp::crypto::curves::secp256k1::verify_schnorr(sig, id, pubkey).has_value();
}

/**
 * The client message publishing a signed event on a topic, `["EVENT", {...}]`, what Trystero's
 * `createEvent` makes.
 *
 * It is written once into one buffer. The topic and the content are escaped once; the id is hashed
 * over those same bytes.
 *
 * @param key The signer.
 * @param created_at The event's time, in whole seconds since the Unix epoch.
 * @param topic The topic, the value of the event's tag "x".
 * @param content The content, a signal's JSON text.
 * @param memory The resource the message is allocated in.
 * @return The message's JSON text; an error of @ref signer::sign.
 * @pre `topic` and `content` are UTF-8, as a room's are: a topic is ASCII, a content a signal's
 * text.
 */
[[nodiscard]] inline boost::system::result<std::pmr::string> event_message(
    const signer& key, std::chrono::sys_seconds created_at, std::string_view topic,
    std::string_view content,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    const auto kind = topic_kind(topic);
    const auto quoted_topic = detail::quoted(topic, memory);
    const auto quoted_content = detail::quoted(content, memory);
    const auto id =
        detail::event_id(key.pubkey_hex(), created_at, kind, quoted_topic, quoted_content);
    const auto sig = key.sign(id);
    if (!sig) {
        return sig.error();
    }
    std::pmr::string out(memory);
    out.reserve(256 + quoted_topic.size() + quoted_content.size());
    out += R"(["EVENT",{"kind":)";
    json::append_integer(out, kind);
    out += R"(,"tags":[["x",)";
    out += quoted_topic;
    out += R"(]],"created_at":)";
    json::append_integer(out, created_at.time_since_epoch().count());
    out += R"(,"content":)";
    out += quoted_content;
    out += R"(,"pubkey":")";
    out += key.pubkey_hex();
    out += R"(","id":")";
    append_hex(out, id);
    out += R"(","sig":")";
    append_hex(out, *sig);
    out += R"("}])";
    return out;
}

/**
 * The client message subscribing to topics, Trystero's `subscribe`: `["REQ", id, {"kinds": [...],
 * "since": t, "#x": [...]}]`.
 *
 * @param subscription The subscription's id.
 * @param topics The topics, at most @ref max_topics_per_subscription, whose kinds the filter lists
 * once each, in the order of their first topic, as Trystero's `new Set` keeps them.
 * @param since The earliest `created_at` of the events wanted, in whole seconds since the Unix
 * epoch, @ref subscribe_since of now in a room.
 * @param memory The resource the message is allocated in.
 * @return The message's JSON text.
 * @pre `subscription` and the topics are UTF-8, as a room's, which are ASCII, are.
 */
inline std::pmr::string subscribe_message(
    std::string_view subscription, std::span<const std::pmr::string> topics,
    std::chrono::sys_seconds since,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::vector<std::uint32_t> kinds(memory);
    kinds.reserve(topics.size());
    for (const auto& t : topics) {
        if (const auto k = topic_kind(t); std::ranges::find(kinds, k) == kinds.end()) {
            kinds.push_back(k);
        }
    }
    std::pmr::string out(memory);
    out.reserve(96 + subscription.size() + (topics.size() * 48));
    out += R"(["REQ",)";
    json::detail::append_utf8(out, subscription);
    out += R"(,{"kinds":[)";
    for (const std::uint32_t k : kinds) {
        if (out.back() != '[') {
            out += ',';
        }
        json::append_integer(out, k);
    }
    out += R"(],"since":)";
    json::append_integer(out, since.time_since_epoch().count());
    out += R"(,"#x":[)";
    for (std::size_t i = 0; i < topics.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        json::detail::append_utf8(out, topics[i]);
    }
    out += "]}]";
    return out;
}

/**
 * The client message ending a subscription: `["CLOSE", id]`.
 *
 * @param subscription The subscription's id.
 * @param memory The resource the message is allocated in.
 * @return The message's JSON text.
 * @pre `subscription` is UTF-8, as a room's, which is ASCII, is.
 */
inline std::pmr::string close_message(
    std::string_view subscription,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(R"(["CLOSE",)", memory);
    json::detail::append_utf8(out, subscription);
    out += ']';
    return out;
}

/** A relay's `["EVENT", <subscription>, <event>]`: an event of a subscription, as views. */
struct event {
    /** The subscription's id. */
    std::string_view subscription;
    /** The event's id. */
    std::string_view id;
    /** The value of the event's first "x" tag; empty when there is none. */
    std::string_view topic;
    /** The event's content. */
    std::string_view content;
    /** The signer's public key, as the event holds it; empty when it holds no string. */
    std::string_view pubkey{};
    /** The event's `created_at`, in whole seconds since the Unix epoch; nothing when it is not an
     * integer of 64 bits. */
    std::optional<std::chrono::sys_seconds> created_at{};
    /** The event's kind; nothing when it is not an integer of 32 bits. */
    std::optional<std::uint32_t> kind{};
    /** Whether the event's one tag is its topic's, `["x", <topic>]`, as Trystero writes it. */
    bool only_topic_tag = false;
};

/**
 * Whether an event's id is its own, the SHA-256 of NIP-01's serialization of the event
 * `[0,<pubkey>,<created_at>,<kind>,<tags>,<content>]`, which @ref event_id computes.
 *
 * A relay sends whatever id it chooses, so an id that is not its event's own may be another
 * event's. Only an event whose one tag is its topic's and whose public key is 64 lowercase
 * hexadecimal digits is checked, as Trystero writes every event; any other has no id of its own
 * here, whatever it says.
 *
 * @param e The event, as @ref read_relay_message reads it.
 * @param memory The resource the escaped topic and content are allocated in, during the call.
 * @return Whether the id is the hexadecimal of the SHA-256 of the event's serialization.
 */
inline bool has_own_id(const event& e,
                       std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    if (!e.only_topic_tag || !e.created_at.has_value() || !e.kind.has_value() ||
        !detail::is_nip01_hex(e.pubkey) || !detail::is_nip01_hex(e.id)) {
        return false;
    }
    return detail::is_hex_of(
        e.id, event_id(e.pubkey, *e.created_at, *e.kind, e.topic, e.content, memory));
}

/** A relay's `["OK", <id>, <accepted>, <reason>]`: whether it took a published event, as views. */
struct ok {
    /** The event's id. */
    std::string_view id;
    /** Whether the relay accepted the event. */
    bool accepted = false;
    /** Why, as `"rate-limited: ..."` or `"blocked: ..."`; may be empty. */
    std::string_view reason;
};

/** A relay's `["EOSE", <subscription>]`: the stored events of a subscription were all sent. */
struct eose {
    /** The subscription's id. */
    std::string_view subscription;
};

/** A relay's `["CLOSED", <subscription>, <reason>]`: the relay ended a subscription. */
struct closed {
    /** The subscription's id. */
    std::string_view subscription;
    /** Why. */
    std::string_view reason;
};

/** A relay's `["NOTICE", <text>]`: a message for humans. */
struct notice {
    /** The message. */
    std::string_view text;
};

/**
 * What a relay sends, as views into the parsed message; anything else is ignored.
 *
 * @see "The protocol", in the guide.
 */
using relay_message = std::variant<event, ok, eose, closed, notice>;

namespace detail {

/** The event of a relay's `["EVENT", <subscription>, <event>]`, as views into its document. */
inline std::optional<relay_message> read_event(const json::const_value& array) {
    const json::const_value o = array[2];
    if (!o.is_object()) {
        return std::nullopt;
    }
    event e;
    e.subscription = json::text_of(array[1]);
    e.id = json::text_of(o["id"]);
    e.content = json::text_of(o["content"]);
    e.pubkey = json::text_of(o["pubkey"]);
    if (const auto created_at = o["created_at"].get<std::int64_t>(); created_at.has_value()) {
        e.created_at = std::chrono::sys_seconds(std::chrono::seconds(*created_at));
    }
    if (const auto kind = o["kind"].get<std::uint32_t>(); kind.has_value()) {
        e.kind = *kind;
    }
    // An array is its own range of elements, which the handle, outliving the loop, keeps.
    const json::const_value tags = o["tags"];
    const auto is_topic_tag = [](const json::const_value& tag) {
        return tag.is_array() && tag.size() >= 2 && json::text_of(tag[0]) == topic_tag &&
               tag[1].is_string();
    };
    for (const json::const_value tag : tags) {
        if (is_topic_tag(tag)) {
            e.topic = json::text_of(tag[1]);
            break;
        }
    }
    // NIP-01 hashes the tags whole, so an id is checked only over tags event_id writes.
    e.only_topic_tag =
        tags.is_array() && tags.size() == 1 && is_topic_tag(tags[0]) && tags[0].size() == 2;
    return e;
}

}  // namespace detail

/**
 * The message a parsed relay text holds, as views into its document.
 *
 * A key written twice is read as JavaScript reads it, the last value, which the parse keeps.
 *
 * @param document The root of the parsed text, whose document the caller keeps alive and unchanged
 * while it reads the views.
 * @return The message, or nothing when Trystero would ignore it: not an array, or not one of the
 * messages of @ref relay_message.
 */
inline std::optional<relay_message> read_relay_message(const json::const_value& document) {
    if (!document.is_array() || !document[0].is_string()) {
        return std::nullopt;
    }
    const auto text_at = [&document](std::uint64_t i) { return json::text_of(document[i]); };
    const std::string_view type = text_at(0);
    if (type == "EVENT") {
        return detail::read_event(document);
    }
    if (type == "OK") {
        return ok{
            .id = text_at(1),
            .accepted = document[2].value_or(false),
            .reason = text_at(3),
        };
    }
    if (type == "EOSE") {
        return eose{text_at(1)};
    }
    if (type == "CLOSED") {
        return closed{.subscription = text_at(1), .reason = text_at(2)};
    }
    if (type == "NOTICE") {
        return notice{text_at(1)};
    }
    return std::nullopt;
}

}  // namespace webcpp::trystero::protocol::nostr

#endif
