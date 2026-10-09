// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_NOSTR_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_NOSTR_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_preallocated.h>
#include <secp256k1_schnorrsig.h>
#include <boost/hash2/sha2.hpp>
#include <boost/json/value.hpp>

#include <array>
#include <charconv>
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

/** The tag a Trystero event carries its topic in, and the event kind a topic maps to: 20000 to
 * 29999, ephemeral. */
inline constexpr std::string_view topic_tag = "x";

inline int topic_kind(std::string_view topic) {
    return static_cast<int>(str_to_num(topic, 10000)) + 20000;
}

namespace detail {

/**
 * The id of an event whose topic and content are already JSON string literals: SHA-256 of the
 * canonical
 * `[0,"<pubkey>",<created_at>,<kind>,[["x",<topic>]],<content>]`, fed to the hash piece by piece.
 */
inline std::array<std::uint8_t, 32> event_id(std::string_view pubkey, std::int64_t created_at,
                                             int kind, std::string_view quoted_topic,
                                             std::string_view quoted_content) {
    char buffer[48];
    char* end = std::to_chars(buffer, buffer + 24, created_at).ptr;
    *end++ = ',';
    end = std::to_chars(end, buffer + sizeof buffer, kind).ptr;
    const std::string_view numbers(buffer, static_cast<std::size_t>(end - buffer));
    boost::hash2::sha2_256 h;
    for (const std::string_view part :
         {std::string_view(R"([0,")"), pubkey, std::string_view(R"(",)"), numbers,
          std::string_view(R"(,[["x",)"), quoted_topic, std::string_view("]],"), quoted_content,
          std::string_view("]")}) {
        h.update(part.data(), part.size());
    }
    const auto digest = h.result();
    std::array<std::uint8_t, 32> out{};
    std::copy(digest.begin(), digest.end(), out.begin());
    return out;
}

inline std::pmr::string quoted(std::string_view text, std::pmr::memory_resource* memory) {
    std::pmr::string out(memory);
    out.reserve(text.size() + 8);
    json::append_string(out, text);
    return out;
}

}  // namespace detail

/** The id of an event: SHA-256 of the JSON array `[0, pubkey, created_at, kind, [["x", topic]],
 * content]`. */
inline std::array<std::uint8_t, 32> event_id(
    std::string_view pubkey, std::int64_t created_at, int kind, std::string_view topic,
    std::string_view content,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    return detail::event_id(pubkey, created_at, kind, detail::quoted(topic, memory),
                            detail::quoted(content, memory));
}

/**
 * A BIP-340 key pair over libsecp256k1, the one key a peer signs every event with.
 *
 * Tip: its signing context is made in the resource it is given
 * (`secp256k1_context_preallocated_create`), where `secp256k1_context_create` would take it from
 * `malloc`.
 */
class signer {
public:
    /** Makes a key from 32 random bytes; nothing when they are not a valid secret key, which is
     * negligibly rare. */
    static std::optional<signer> from_secret(
        std::span<const std::uint8_t, 32> secret,
        std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
        signer s(memory);
        if (secp256k1_keypair_create(s.context_.get(), &s.keypair_, secret.data()) == 0)
            return std::nullopt;
        secp256k1_xonly_pubkey xonly;
        secp256k1_keypair_xonly_pub(s.context_.get(), &xonly, nullptr, &s.keypair_);
        std::array<std::uint8_t, 32> pub{};
        secp256k1_xonly_pubkey_serialize(s.context_.get(), pub.data(), &xonly);
        s.pubkey_ = to_hex(pub, memory);
        return s;
    }

    std::string_view pubkey() const { return pubkey_; }

    std::array<std::uint8_t, 64> sign(std::span<const std::uint8_t, 32> message,
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

    static std::size_t context_size() {
        return secp256k1_context_preallocated_size(SECP256K1_CONTEXT_NONE);
    }

    explicit signer(std::pmr::memory_resource* memory)
        : context_(secp256k1_context_preallocated_create(
                       memory->allocate(context_size(), alignof(std::max_align_t)),
                       SECP256K1_CONTEXT_NONE),
                   context_deleter{memory}),
          pubkey_(memory) {}

    std::unique_ptr<secp256k1_context, context_deleter> context_;
    secp256k1_keypair keypair_{};
    std::pmr::string pubkey_;
};

/** Whether a 64-byte signature over an event id verifies under an x-only public key; libsecp256k1's
 * static context. */
inline bool verify(std::span<const std::uint8_t, 64> sig, std::span<const std::uint8_t, 32> id,
                   std::span<const std::uint8_t, 32> pubkey) {
    secp256k1_xonly_pubkey key;
    if (secp256k1_xonly_pubkey_parse(secp256k1_context_static, &key, pubkey.data()) == 0)
        return false;
    return secp256k1_schnorrsig_verify(secp256k1_context_static, sig.data(), id.data(), id.size(),
                                       &key) == 1;
}

/**
 * The client message publishing a signed event on a topic, `["EVENT", {...}]`, written once into
 * one buffer. The topic and the content are escaped once; the id is hashed over those same bytes.
 */
inline std::pmr::string event_message(
    const signer& key, std::int64_t created_at, std::string_view topic, std::string_view content,
    std::span<const std::uint8_t, 32> aux,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    const int kind = topic_kind(topic);
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
    json::append_integer(out, created_at);
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

/** The client message subscribing to topics: `["REQ", id, {"kinds": [...], "since": t, "#x":
 * [...]}]`. */
inline std::pmr::string subscribe_message(
    std::string_view subscription, std::span<const std::pmr::string> topics, std::int64_t since,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::set<int> kinds(memory);
    for (const auto& t : topics)
        kinds.insert(topic_kind(t));
    std::pmr::string out(memory);
    out.reserve(96 + subscription.size() + topics.size() * 48);
    out += R"(["REQ",)";
    json::append_string(out, subscription);
    out += R"(,{"kinds":[)";
    for (const int k : kinds) {
        if (out.back() != '[')
            out += ',';
        json::append_integer(out, k);
    }
    out += R"(],"since":)";
    json::append_integer(out, since);
    out += R"(,"#x":[)";
    for (std::size_t i = 0; i < topics.size(); ++i) {
        if (i != 0)
            out += ',';
        json::append_string(out, topics[i]);
    }
    out += "]}]";
    return out;
}

inline std::pmr::string close_message(
    std::string_view subscription,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::string out(R"(["CLOSE",)", memory);
    json::append_string(out, subscription);
    out += ']';
    return out;
}

/** What a relay sends, as views into the parsed message. Anything else is ignored. */
struct event {
    std::string_view subscription;
    std::string_view id;
    std::string_view topic;  // the value of the first "x" tag; empty when there is none
    std::string_view content;
};

struct ok {
    std::string_view id;
    bool accepted = false;
    std::string_view reason;
};

struct eose {
    std::string_view subscription;
};

struct closed {
    std::string_view subscription;
    std::string_view reason;
};

struct notice {
    std::string_view text;
};

using relay_message = std::variant<event, ok, eose, closed, notice>;

/** The message a parsed relay text holds, as views into `document`, which the caller keeps alive
 * while it reads them. */
inline std::optional<relay_message> read_relay_message(const boost::json::value& document) {
    const auto* array = document.if_array();
    if (array == nullptr || array->empty() || !array->at(0).is_string())
        return std::nullopt;
    auto view = [](const boost::json::value& v) -> std::string_view {
        const auto* s = v.if_string();
        return s != nullptr ? std::string_view(s->data(), s->size()) : std::string_view();
    };
    auto text_at = [array, &view](std::size_t i) {
        return i < array->size() ? view(array->at(i)) : std::string_view();
    };
    const std::string_view type = text_at(0);
    if (type == "EVENT") {
        if (array->size() < 3 || !array->at(2).is_object())
            return std::nullopt;
        const auto& o = array->at(2).get_object();
        event e;
        e.subscription = text_at(1);
        if (const auto* id = o.if_contains("id"))
            e.id = view(*id);
        if (const auto* c = o.if_contains("content"))
            e.content = view(*c);
        if (const auto* tags = o.if_contains("tags"); tags != nullptr && tags->is_array()) {
            for (const auto& tag : tags->get_array()) {
                const auto* pair = tag.if_array();
                if (pair != nullptr && pair->size() >= 2 && view(pair->at(0)) == topic_tag &&
                    pair->at(1).is_string()) {
                    e.topic = view(pair->at(1));
                    break;
                }
            }
        }
        return e;
    }
    if (type == "OK")
        return ok{text_at(1),
                  array->size() > 2 && array->at(2).is_bool() && array->at(2).get_bool(),
                  text_at(3)};
    if (type == "EOSE")
        return eose{text_at(1)};
    if (type == "CLOSED")
        return closed{text_at(1), text_at(2)};
    if (type == "NOTICE")
        return notice{text_at(1)};
    return std::nullopt;
}

}  // namespace webcpp::trystero::protocol::nostr

#endif
