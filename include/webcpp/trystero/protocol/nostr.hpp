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
#include <webcpp/trystero/protocol/topics.hpp>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_preallocated.h>
#include <secp256k1_schnorrsig.h>
#include <boost/hash2/sha2.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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

/**
 * The event kind a topic maps to, Trystero's `topicToKind`: 20000 to 29999, ephemeral, so relays
 * forward the events and keep none.
 *
 * @param topic The topic's name.
 * @return 20000 plus `str_to_num(topic, 10000)`, below 30000.
 */
inline std::uint32_t topic_kind(std::string_view topic) {
    return static_cast<std::uint32_t>(str_to_num(topic, 10000)) + 20000;
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
    json::append_string(out, text);
    return out;
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
 * A BIP-340 key pair over libsecp256k1, the one key a peer signs every event with, as Trystero's
 * Nostr strategy does with its `pubkey`.
 *
 * Tip: its signing context is made in the resource it is given
 * (`secp256k1_context_preallocated_create`), where `secp256k1_context_create` would take it from
 * `malloc`.
 */
class signer {
public:
    /**
     * Makes a key from 32 random bytes.
     *
     * @param secret The secret key.
     * @param memory The resource the signing context and the public key's text are allocated in;
     * it must outlive the signer.
     * @return The signer, or nothing when the bytes are not a valid secret key, which is
     * negligibly rare.
     */
    static std::optional<signer> from_secret(
        std::span<const std::uint8_t, 32> secret,
        std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
        signer s(memory);
        if (secp256k1_keypair_create(s.context_.get(), &s.keypair_, secret.data()) == 0) {
            return std::nullopt;
        }
        secp256k1_xonly_pubkey xonly;
        secp256k1_keypair_xonly_pub(s.context_.get(), &xonly, nullptr, &s.keypair_);
        std::array<std::uint8_t, 32> pub{};
        secp256k1_xonly_pubkey_serialize(s.context_.get(), pub.data(), &xonly);
        s.pubkey_ = to_hex(pub, memory);
        return s;
    }

    /**
     * The x-only public key, an event's `pubkey`.
     *
     * @return 64 lowercase hexadecimal digits, valid as long as the signer.
     */
    [[nodiscard]] std::string_view pubkey() const { return pubkey_; }

    /**
     * Signs a message with BIP-340 Schnorr.
     *
     * @param message The 32 bytes to sign, an event id.
     * @param aux 32 random bytes of auxiliary randomness.
     * @return The 64-byte signature, an event's `sig`.
     */
    [[nodiscard]] std::array<std::uint8_t, 64> sign(std::span<const std::uint8_t, 32> message,
                                                    std::span<const std::uint8_t, 32> aux) const {
        std::array<std::uint8_t, 64> sig{};
        secp256k1_schnorrsig_sign32(context_.get(), sig.data(), message.data(), &keypair_,
                                    aux.data());
        return sig;
    }

private:
    /** Destroys a context made in a resource, and gives its memory back there. */
    struct context_deleter {
        std::pmr::memory_resource* memory = nullptr;

        void operator()(secp256k1_context* c) const {
            secp256k1_context_preallocated_destroy(c);
            memory->deallocate(c, context_size(), alignof(std::max_align_t));
        }
    };

    // A context that signs and verifies needs no flag. libsecp256k1 spells it with a shift of a
    // signed constant, (1 << 0), inside its own macro, which is the dependency's to write.
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    static constexpr unsigned context_flags = SECP256K1_CONTEXT_NONE;

    static std::size_t context_size() { return secp256k1_context_preallocated_size(context_flags); }

    explicit signer(std::pmr::memory_resource* memory)
        : context_(secp256k1_context_preallocated_create(
                       memory->allocate(context_size(), alignof(std::max_align_t)), context_flags),
                   context_deleter{memory}),
          pubkey_(memory) {}

    std::unique_ptr<secp256k1_context, context_deleter> context_;
    secp256k1_keypair keypair_{};
    std::pmr::string pubkey_;
};

/**
 * Whether a BIP-340 signature over an event id verifies under an x-only public key.
 *
 * It uses libsecp256k1's static context, which allocates nothing.
 *
 * @param sig The signature.
 * @param id The event id.
 * @param pubkey The x-only public key.
 * @return Whether the signature is valid; false too when the key is not a valid point.
 */
inline bool verify(std::span<const std::uint8_t, 64> sig, std::span<const std::uint8_t, 32> id,
                   std::span<const std::uint8_t, 32> pubkey) {
    secp256k1_xonly_pubkey key;
    if (secp256k1_xonly_pubkey_parse(secp256k1_context_static, &key, pubkey.data()) == 0) {
        return false;
    }
    return secp256k1_schnorrsig_verify(secp256k1_context_static, sig.data(), id.data(), id.size(),
                                       &key) == 1;
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
 * @param aux 32 random bytes for the signature.
 * @param memory The resource the message is allocated in.
 * @return The message's JSON text.
 */
inline std::pmr::string event_message(
    const signer& key, std::chrono::sys_seconds created_at, std::string_view topic,
    std::string_view content, std::span<const std::uint8_t, 32> aux,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    const auto kind = topic_kind(topic);
    const auto quoted_topic = detail::quoted(topic, memory);
    const auto quoted_content = detail::quoted(content, memory);
    const auto id = detail::event_id(key.pubkey(), created_at, kind, quoted_topic, quoted_content);
    const auto sig = key.sign(id, aux);
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
    out += key.pubkey();
    out += R"(","id":")";
    append_hex(out, id);
    out += R"(","sig":")";
    append_hex(out, sig);
    out += R"("}])";
    return out;
}

/**
 * The client message subscribing to topics, Trystero's `subscribe`: `["REQ", id, {"kinds": [...],
 * "since": t, "#x": [...]}]`.
 *
 * @param subscription The subscription's id.
 * @param topics The topics, whose kinds the filter lists once each, in increasing order.
 * @param since The earliest `created_at` of the events wanted, in whole seconds since the Unix
 * epoch.
 * @param memory The resource the message is allocated in.
 * @return The message's JSON text.
 */
inline std::pmr::string subscribe_message(
    std::string_view subscription, std::span<const std::pmr::string> topics,
    std::chrono::sys_seconds since,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::set<std::uint32_t> kinds(memory);
    for (const auto& t : topics) {
        kinds.insert(topic_kind(t));
    }
    std::pmr::string out(memory);
    out.reserve(96 + subscription.size() + (topics.size() * 48));
    out += R"(["REQ",)";
    json::append_string(out, subscription);
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
        json::append_string(out, topics[i]);
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
 */
inline std::pmr::string close_message(
    std::string_view subscription,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(R"(["CLOSE",)", memory);
    json::append_string(out, subscription);
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
};

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
    // An array is its own range of elements, which the handle, outliving the loop, keeps.
    const json::const_value tags = o["tags"];
    for (const json::const_value tag : tags) {
        if (tag.is_array() && tag.size() >= 2 && json::text_of(tag[0]) == topic_tag &&
            tag[1].is_string()) {
            e.topic = json::text_of(tag[1]);
            break;
        }
    }
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
