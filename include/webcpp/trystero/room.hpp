// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_ROOM_HPP
#define WEBCPP_TRYSTERO_ROOM_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/action_wire.hpp>
#include <webcpp/trystero/detail/app_impl.hpp>
#include <webcpp/trystero/detail/connection.hpp>
#include <webcpp/trystero/detail/ice_servers.hpp>
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/detail/nostr_strategy.hpp>
#include <webcpp/trystero/detail/post.hpp>
#include <webcpp/trystero/detail/random_id.hpp>
#include <webcpp/trystero/detail/rebinding.hpp>
#include <webcpp/trystero/detail/signaling.hpp>
#include <webcpp/trystero/detail/user_table.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/options.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>
#include <webcpp/trystero/protocol/ids.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/room_keys.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/text.hpp>
#include <webcpp/trystero/protocol/topics.hpp>
#include <webcpp/trystero/room_error.hpp>
#include <webcpp/trystero/subscription.hpp>

#include <webcpp/asio/any_completion_handler.hpp>  // lint-world: posts handlers only
#include <webcpp/asio/any_io_executor.hpp>         // lint-world: posts handlers only
#include <webcpp/asio/append.hpp>                  // lint-world: posts handlers only
#include <webcpp/asio/associated_allocator.hpp>    // lint-world: posts handlers only
#include <webcpp/asio/associated_executor.hpp>     // lint-world: posts handlers only
#include <webcpp/asio/async_result.hpp>            // lint-world: posts handlers only
#include <webcpp/asio/bind_allocator.hpp>          // lint-world: posts handlers only

#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/storage.hpp>

#include <boost/assert.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace webcpp::trystero {

class app;

/**
 * A peer's id in a room, Trystero's `selfId` and the ids `getPeers` lists: 20 characters of
 * `protocol::base62_alphabet` unless the configuration names one.
 *
 * A room hands its ids out in its own resource.
 */
using peer_id = std::pmr::string;

/**
 * Binary data allocated with `std::allocator`, an action's binary payload when the user chose
 * standard containers.
 *
 * A message received as `bytes` is a copy of the payload, allocated with `std::allocator`.
 */
using bytes = std::vector<std::uint8_t>;

/**
 * Binary data in a memory resource, an action's binary payload in the room's resource.
 *
 * A message received as `pmr_bytes` uses the room's effective `room_options::memory`, which
 * must outlive the value. A received buffer in the same resource is moved into the value;
 * a buffer in the app's distinct resource is copied once into the room's. An inline payload
 * is taken out of its frame before being moved or copied.
 *
 * @see "Allocation", in the guide.
 */
using pmr_bytes = std::pmr::vector<std::uint8_t>;

/**
 * What an action carries when its type is not fixed: Trystero's text, JSON or binary payload.
 *
 * A message received as `payload` holds the alternative of its format: a `std::string` for text,
 * a `webcpp::document::document` for JSON, and `bytes` for binary data, each with the standard
 * allocator.
 *
 * Tip: a handle of the document's values refers to the document object, as every handle of
 * webcpp::document does: moving the document, or the variant that holds it, invalidates it.
 */
using payload = std::variant<std::string, webcpp::document::document, bytes>;

/**
 * Who sent a message, and the metadata it carried, the second and third arguments of Trystero's
 * action handler, in the receiving room's resource.
 */
struct message_info {
    /** The sender's id. */
    peer_id from;
    /** The metadata the sender passed, as JSON text, or empty when it passed none. */
    std::pmr::string metadata;
};

/**
 * A peer that joined or left a room, what `room::on_peer_join` and `room::on_peer_leave` report,
 * Trystero's `PeerJoinContext` and `PeerLeaveContext` without their roles.
 *
 * The views are valid for the duration of the listener's call.
 */
struct peer_event {
    /** The peer's id. */
    std::string_view peer_id;
    /** The id of the peer's user; until identities are derived from keys, the peer's own id. */
    std::string_view user_id;
};

/**
 * A user that joined or left a room, what `room::on_user_join` and `room::on_user_leave` report,
 * Trystero's `UserJoinContext` and `UserLeaveContext` without their roles.
 *
 * The views are valid for the duration of the listener's call.
 */
struct user_event {
    /** The user's id. */
    std::string_view user_id;
    /** The peer whose join or departure made the user join or leave: its first or its last. */
    std::string_view peer_id;
};

/**
 * A user of a room, an entry of `room::users`, Trystero's `UserInfo`.
 */
struct user_info {
    /** The user's active peers, in the order they became active. */
    std::pmr::vector<std::pmr::string> peer_ids;
    /** The user's roles; none until roles are ported. */
    std::pmr::vector<std::pmr::string> roles;
};

/**
 * An active peer of a room, an entry of `room::peers`, Trystero's `PeerInfo` without its
 * `connection`: the peer connection is the backend's, which a room does not hand out.
 */
struct peer_info {
    /** The id of the peer's user. */
    std::pmr::string user_id;
    /** Whether the room reaches the peer over a connection of its own; always, until relayed
     * peers are ported. */
    bool direct = true;
    /** How many connections away the peer is: 1 for a direct one. */
    std::uint32_t hops = 1;
    /** The peer's roles; none until roles are ported. */
    std::pmr::vector<std::pmr::string> roles;
};

/**
 * Where this peer's identity comes from, Trystero's `IdentityPersistence`.
 */
enum class identity_persistence {
    /** Made for this app and lost with it, as every identity is until identities are derived from
     * keys. */
    ephemeral,
    /** Imported from a text the program kept. */
    imported,
};

/**
 * This peer in a room, what `room::self` reports, Trystero's `SelfInfo`.
 *
 * The views are valid as long as the room.
 */
struct self_info {
    /** This peer's id, the app's. */
    std::string_view peer_id;
    /** This peer's user's id; until identities are derived from keys, the peer's own id. */
    std::string_view user_id;
    /** Where the identity comes from. */
    identity_persistence persistence = identity_persistence::ephemeral;
    /** The id of the room as it runs now. */
    std::string_view room_id;
    /** The id the room was joined as; the same as `room_id` until rooms that scale are ported. */
    std::string_view base_room_id;
    /** Whether this peer administers the room; never, until roles are ported. */
    bool is_admin = false;
    /** This peer's roles; none until roles are ported. */
    std::span<const std::pmr::string> roles;
};

/**
 * What an action's receive gate is asked about, Trystero's `ActionReceiveContext` without its
 * roles, its signal and its kind: a message, before it is received.
 *
 * The views are valid for the duration of the gate's call.
 */
struct receive_info {
    /** The sender's id. */
    std::string_view peer_id;
    /** The id of the sender's user. */
    std::string_view user_id;
    /** The size of the message's payload, in bytes. */
    std::size_t byte_length = 0;
    /** The metadata the sender passed, as JSON text, or empty when it passed none. */
    std::string_view metadata;
};

/**
 * Whose message an action's progress concerns, Trystero's `ActionProgressContext`.
 *
 * The views are valid for the duration of the listener's call.
 */
struct progress_info {
    /** The sender's id. */
    std::string_view peer_id;
    /** The id of the sender's user. */
    std::string_view user_id;
    /** The metadata the sender passed, as JSON text, or empty when it passed none. */
    std::string_view metadata;
};

/**
 * Where a request goes, what it carries beside its payload and how long it waits, Trystero's
 * `RequestOptions` without `onProgress` and `signal`.
 *
 * Tip: it owns its strings because a completion token such as `asio::deferred` starts the
 * operation later, after the caller's views may be gone.
 */
struct request_options {
    /** The peer asked, which must be active, or a user, whose active peer of the lowest id is
     * asked, as 0.27.0 picks one. */
    peer_id target = {};
    /** The metadata, as JSON text, or empty for none. */
    std::pmr::string metadata = {};
    /** How long the request waits for its answer, Trystero's `timeoutMs`; none waits as long as
     * the peer stays. */
    std::optional<std::chrono::milliseconds> timeout = std::nullopt;
};

/**
 * Where an action's message goes and what it carries beside its payload, the target and the
 * metadata of Trystero's `send`.
 *
 * Tip: it owns its strings because a completion token such as `asio::deferred` starts the
 * operation later, after the caller's views may be gone.
 */
struct send_options {
    /** Where to send: each an active peer's id, or a user's, which stands for each of its active
     * peers (a peer reached twice is sent to once); empty means every active peer. */
    std::pmr::vector<peer_id> targets = {};
    /** The metadata, as JSON text, or empty for none. */
    std::pmr::string metadata = {};
};

namespace detail {

/** A payload ready to send: its format, and its bytes kept alive by what owns them. */
struct encoded_payload {
    /** The payload's format, which its frame carries. */
    protocol::payload_format format = protocol::payload_format::text;
    /** The payload's bytes and their owner. */
    payload_ref ref;
};

/** An encoded payload over the bytes `owner` holds, which it keeps alive. */
template <class Owner>
encoded_payload own(protocol::payload_format format, std::shared_ptr<const Owner> owner) {
    const auto data =
        std::span(reinterpret_cast<const std::uint8_t*>(owner->data()), owner->size());
    return {format, {data, std::move(owner)}};
}

/** What keeps a payload alive, in `memory`: a shared object there, whose allocator also builds it
 * when it can. */
template <class Owner, class... Args>
std::shared_ptr<const Owner> keep(std::pmr::memory_resource* memory, Args&&... args) {
    return std::allocate_shared<Owner>(std::pmr::polymorphic_allocator<>(memory),
                                       std::forward<Args>(args)...);
}

/** Bytes as text, a view of the same storage. */
inline std::string_view as_text(std::span<const std::uint8_t> data) {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

/**
 * What a room decodes a received payload with: its resource, and its bound on a JSON payload's
 * image.
 */
struct decoding {
    /** The room's resource, which the `std::pmr` types are built in. */
    std::pmr::memory_resource* memory = nullptr;
    /** The largest image a JSON payload may take, `room_options::max_json_document_bytes`. */
    std::uint64_t max_json_document_bytes = 0;
};

/**
 * How an action's type travels. `encode` takes the value at the call and keeps it alive without
 * copying it when the type owns its storage; a view is copied once, there, into the room's
 * resource. `decode` reads a received message with the room's @ref decoding. A view type refers to
 * the message's buffer and is valid for the duration of the handler, so only owning types can be
 * awaited. The `std::pmr` types are built in the receiving room's resource,
 * `webcpp::document::pmr::document` too; the standard types allocate with `std::allocator`, as the
 * user chose, and so does `webcpp::document::document`. A JSON text is parsed
 * within @ref json_payload_limits.
 *
 * | Type                             | Trystero format | Receiving                                |
 * | std::string                      | text            | a copy of the bytes                      |
 * | std::pmr::string                 | text            | a copy of the bytes, in the room's       |
 * |                                  |                 | resource                                 |
 * | std::string_view                 | text            | a view of the frame, in a handler        |
 * | webcpp::document::document       | JSON            | parsed, with `std::allocator`            |
 * | webcpp::document::pmr::document  | JSON            | parsed, in the room's resource           |
 * | trystero::bytes                  | binary          | a copy of the bytes                      |
 * | trystero::pmr_bytes              | binary          | moved or copied once into the room's     |
 * |                                  |                 | resource                                 |
 * | std::span<const std::uint8_t>    | binary          | a view of the frame, in a handler        |
 * | trystero::payload                | any             | by format                                |
 */
template <class T>
struct payload_traits;

/** How an action of `std::string`, Trystero's text payload, travels. */
template <>
struct payload_traits<std::string> {
    /** Whether a decoded value owns its storage, so that it may be awaited. */
    static constexpr bool owning = true;

    /** Keeps the text alive, moved into a shared object of the room's resource. */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    static encoded_payload encode(std::string v, std::pmr::memory_resource* memory) {
        // NOLINTEND(performance-unnecessary-value-param)
        return own(protocol::payload_format::text, keep<std::string>(memory, std::move(v)));
    }

    /**
     * The message's payload as this type: a copy of the text, allocated with `std::allocator`;
     * nothing for another format.
     */
    static std::optional<std::string> decode(incoming& m, const decoding& /*to*/) {
        if (m.format != protocol::payload_format::text) {
            return std::nullopt;
        }
        return std::string(as_text(m.payload()));
    }
};

/** How an action of `std::pmr::string`, Trystero's text payload, travels. */
template <>
struct payload_traits<std::pmr::string> {
    /** Whether a decoded value owns its storage, so that it may be awaited. */
    static constexpr bool owning = true;

    /** Keeps the text alive, moved into a shared object of the room's resource. */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    static encoded_payload encode(std::pmr::string v, std::pmr::memory_resource* memory) {
        // NOLINTEND(performance-unnecessary-value-param)
        return own(protocol::payload_format::text, keep<std::pmr::string>(memory, std::move(v)));
    }

    /**
     * The message's payload as this type: a copy of the text, in the room's resource; nothing for
     * another format.
     */
    static std::optional<std::pmr::string> decode(incoming& m, const decoding& to) {
        if (m.format != protocol::payload_format::text) {
            return std::nullopt;
        }
        return std::pmr::string(as_text(m.payload()), to.memory);
    }
};

/** How an action of `std::string_view`, Trystero's text payload, travels. */
template <>
struct payload_traits<std::string_view> {
    /** Whether a decoded value owns its storage, so that it may be awaited. */
    static constexpr bool owning = false;

    /** Copies the viewed text once, into a shared string of the room's resource. */
    static encoded_payload encode(std::string_view v, std::pmr::memory_resource* memory) {
        return own(protocol::payload_format::text, keep<std::pmr::string>(memory, v));
    }

    /**
     * The message's payload as this type: a view of the message, valid for the handler's call;
     * nothing for another format.
     */
    static std::optional<std::string_view> decode(incoming& m, const decoding& /*to*/) {
        if (m.format != protocol::payload_format::text) {
            return std::nullopt;
        }
        return as_text(m.payload());
    }
};

/**
 * The limits of a received JSON payload's parse: its own length, and an image that grows to at
 * most 96 times that and 64 KiB, which every valid text fits, or the room's bound, the smaller
 * (@ref protocol::json::payload_limits).
 */
inline webcpp::document::parse_options json_payload_limits(std::string_view text,
                                                           const decoding& to) noexcept {
    return protocol::json::payload_limits(text.size(), to.max_json_document_bytes);
}

/** How an action of `webcpp::document::document`, Trystero's JSON payload, travels. */
template <>
struct payload_traits<webcpp::document::document> {
    /** Whether a decoded value owns its storage, so that it may be awaited. */
    static constexpr bool owning = true;

    /** Writes the value once, as `JSON.stringify` writes what JavaScript can hold, into a shared
     * string of the room's resource. */
    static encoded_payload encode(const webcpp::document::document& v,
                                  std::pmr::memory_resource* memory) {
        std::pmr::string text(memory);
        protocol::json::append_value(text, v.root());
        return own(protocol::payload_format::json, keep<std::pmr::string>(memory, std::move(text)));
    }

    /**
     * The message's payload as this type: the parsed text, an owned document in `std::allocator`,
     * as the user chose (@ref json_payload_limits); nothing for another format or a text that does
     * not parse within them.
     */
    static std::optional<webcpp::document::document> decode(incoming& m, const decoding& to) {
        if (m.format != protocol::payload_format::json) {
            return std::nullopt;
        }
        const std::string_view text = as_text(m.payload());
        auto v = webcpp::document::parse(text, json_payload_limits(text, to));
        if (!v.has_value()) {
            return std::nullopt;
        }
        return std::move(v).unsafe_value();
    }
};

/** How an action of `webcpp::document::pmr::document`, Trystero's JSON payload, travels. */
template <>
struct payload_traits<webcpp::document::pmr::document> {
    /** Whether a decoded value owns its storage, so that it may be awaited. */
    static constexpr bool owning = true;

    /** Writes the value once, as `JSON.stringify` writes what JavaScript can hold, into a shared
     * string of the room's resource. */
    static encoded_payload encode(const webcpp::document::pmr::document& v,
                                  std::pmr::memory_resource* memory) {
        std::pmr::string text(memory);
        protocol::json::append_value(text, v.root());
        return own(protocol::payload_format::json, keep<std::pmr::string>(memory, std::move(text)));
    }

    /**
     * The message's payload as this type: the parsed text, its image and the parse's working state
     * in the room's resource (@ref json_payload_limits); nothing for another format or a text that
     * does not parse within them.
     */
    static std::optional<webcpp::document::pmr::document> decode(incoming& m, const decoding& to) {
        if (m.format != protocol::payload_format::json) {
            return std::nullopt;
        }
        const std::string_view text = as_text(m.payload());
        auto v = webcpp::document::parse(text, json_payload_limits(text, to),
                                         std::pmr::polymorphic_allocator<std::byte>(to.memory));
        if (!v.has_value()) {
            return std::nullopt;
        }
        return std::move(v).unsafe_value();
    }
};

/** How an action of `trystero::bytes`, Trystero's binary payload, travels. */
template <>
struct payload_traits<bytes> {
    /** Whether a decoded value owns its storage, so that it may be awaited. */
    static constexpr bool owning = true;

    /** Keeps the bytes alive, moved into a shared object of the room's resource. */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    static encoded_payload encode(bytes v, std::pmr::memory_resource* memory) {
        // NOLINTEND(performance-unnecessary-value-param)
        return own(protocol::payload_format::binary, keep<bytes>(memory, std::move(v)));
    }

    /**
     * The message's payload as this type: a copy of the bytes, allocated with `std::allocator`;
     * nothing for another format.
     */
    static std::optional<bytes> decode(incoming& m, const decoding& /*to*/) {
        if (m.format != protocol::payload_format::binary) {
            return std::nullopt;
        }
        const auto p = m.payload();
        return bytes(p.begin(), p.end());
    }
};

/** How an action of `trystero::pmr_bytes`, Trystero's binary payload, travels. */
template <>
struct payload_traits<pmr_bytes> {
    /** Whether a decoded value owns its storage, so that it may be awaited. */
    static constexpr bool owning = true;

    /** Keeps the bytes alive, moved into a shared object of the room's resource. */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    static encoded_payload encode(pmr_bytes v, std::pmr::memory_resource* memory) {
        // NOLINTEND(performance-unnecessary-value-param)
        return own(protocol::payload_format::binary, keep<pmr_bytes>(memory, std::move(v)));
    }

    /**
     * The message's payload as this type: the message's buffer, moved when the payload starts it
     * and no other listener reads the message, in the room's resource; nothing for another format.
     */
    static std::optional<pmr_bytes> decode(incoming& m, const decoding& to) {
        if (m.format != protocol::payload_format::binary) {
            return std::nullopt;
        }
        if (m.shared || m.buffer.get_allocator().resource() != to.memory) {
            const auto payload = m.payload();
            return pmr_bytes(payload.begin(), payload.end(), to.memory);
        }
        return pmr_bytes(std::move(m).take_payload(), to.memory);
    }
};

/** How an action of `std::span<const std::uint8_t>`, Trystero's binary payload, travels. */
template <>
struct payload_traits<std::span<const std::uint8_t>> {
    /** Whether a decoded value owns its storage, so that it may be awaited. */
    static constexpr bool owning = false;

    /** Copies the viewed bytes once, into a shared buffer of the room's resource. */
    static encoded_payload encode(std::span<const std::uint8_t> v,
                                  std::pmr::memory_resource* memory) {
        return own(protocol::payload_format::binary, keep<pmr_bytes>(memory, v.begin(), v.end()));
    }

    /**
     * The message's payload as this type: a view of the message, valid for the handler's call;
     * nothing for another format.
     */
    static std::optional<std::span<const std::uint8_t>> decode(incoming& m,
                                                               const decoding& /*to*/) {
        if (m.format != protocol::payload_format::binary) {
            return std::nullopt;
        }
        return m.payload();
    }
};

/** How an action of `trystero::payload`, any of Trystero's payloads, travels. */
template <>
struct payload_traits<payload> {
    /** Whether a decoded value owns its storage, so that it may be awaited. */
    static constexpr bool owning = true;

    /** Encodes the alternative the payload holds, as its own type does. */
    static encoded_payload encode(payload v, std::pmr::memory_resource* memory) {
        return std::visit(
            [memory](auto&& x) {
                return payload_traits<std::decay_t<decltype(x)>>::encode(
                    std::forward<decltype(x)>(x), memory);
            },
            std::move(v));
    }

    /**
     * The message's payload as this type: the alternative of the message's format; nothing for
     * another format.
     */
    static std::optional<payload> decode(incoming& m, const decoding& to) {
        switch (m.format) {
            case protocol::payload_format::text:
                if (auto v = payload_traits<std::string>::decode(m, to)) {
                    return payload(std::move(*v));
                }
                return std::nullopt;
            case protocol::payload_format::json:
                if (auto v = payload_traits<webcpp::document::document>::decode(m, to)) {
                    return payload(std::move(*v));
                }
                return std::nullopt;
            case protocol::payload_format::binary:
                if (auto v = payload_traits<bytes>::decode(m, to)) {
                    return payload(std::move(*v));
                }
                return std::nullopt;
        }
        return std::nullopt;
    }
};

/**
 * The value `async_receive` completes with when it decodes no message, which neither allocates nor
 * raises: an empty container in the room's resource for a `std::pmr` type, and for a JSON document
 * one that holds nothing, whose root reads `undefined`, `webcpp::document::pmr::document`'s in the
 * room's resource.
 */
template <class T>
T empty_payload(std::pmr::memory_resource* memory) noexcept {
    if constexpr (std::is_same_v<T, webcpp::document::pmr::document>) {
        return T::nothing(std::pmr::polymorphic_allocator<std::byte>(memory));
    } else if constexpr (std::is_same_v<T, webcpp::document::document>) {
        return T::nothing();
    } else if constexpr (std::is_same_v<T, std::pmr::string> || std::is_same_v<T, pmr_bytes>) {
        // An empty std::pmr container allocates nothing.
        return T(memory);
    } else {
        return T{};
    }
}

/**
 * What a request's metadata holds, `{"r":<id>,"m":<metadata>}` as 0.27.0's request actions write
 * it: the request's id, and the sender's own metadata, as JSON text, or empty when it passed none.
 */
struct request_envelope {
    /** The request's id, which its answer names. */
    std::pmr::string id;
    /** The sender's metadata, as JSON text, or empty. */
    std::pmr::string metadata;
};

/**
 * What an answer's metadata holds, `{"r":<id>}` or `{"r":<id>,"e":<reason>}`: the id of the
 * request it answers, and the reason of a refusal.
 */
struct response_envelope {
    /** The id of the request answered. */
    std::pmr::string id;
    /** Whether the answer refuses the request. */
    bool refused = false;
    /** The reason of a refusal, or empty. */
    std::pmr::string reason;
};

/** The object a request's or an answer's metadata holds, within a JSON payload's limits; a text
 * that is no object, or does not parse, holds nothing. */
inline std::optional<webcpp::document::pmr::document> envelope_of(std::string_view metadata,
                                                                  const decoding& to) {
    if (metadata.empty()) {
        return std::nullopt;
    }
    auto v = webcpp::document::parse(metadata, json_payload_limits(metadata, to),
                                     std::pmr::polymorphic_allocator<std::byte>(to.memory));
    if (!v.has_value() || !v->root().is_object() || !v->root()["r"].is_string()) {
        return std::nullopt;
    }
    return std::move(v).unsafe_value();
}

/** A request's metadata read, as 0.27.0's `getRequestMetadata` reads it; nothing when it names no
 * request. */
inline std::optional<request_envelope> read_request(std::string_view metadata, const decoding& to) {
    const auto v = envelope_of(metadata, to);
    if (!v) {
        return std::nullopt;
    }
    request_envelope out{
        .id = std::pmr::string(protocol::json::text_of(v->root()["r"]), to.memory),
        .metadata = std::pmr::string(to.memory),
    };
    protocol::json::append_value(out.metadata, v->root()["m"]);  // `undefined` writes nothing
    return out;
}

/** The value of a hexadecimal digit, either case, or nothing for another character. */
inline std::optional<std::uint32_t> hex_digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint32_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint32_t>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint32_t>(c - 'A' + 10);
    }
    return std::nullopt;
}

/** The UTF-16 code unit of the `\\uXXXX` escape at `at` of a JSON text, or nothing when none
 * starts there. */
inline std::optional<std::uint32_t> unicode_escape_at(std::string_view json, std::size_t at) {
    if (at + 6 > json.size() || json[at] != '\\' || json[at + 1] != 'u') {
        return std::nullopt;
    }
    std::uint32_t unit = 0;
    for (std::size_t i = at + 2; i < at + 6; ++i) {
        const auto digit = hex_digit_value(json[i]);
        if (!digit) {
            return std::nullopt;
        }
        unit = (unit << 4U) | *digit;
    }
    return unit;
}

/**
 * A JSON text with each lone surrogate escape, `\\ud83d` without its low half, written as the
 * replacement character's, `\\ufffd`, which webcpp::document's parser can read: it refuses a lone
 * surrogate, which JavaScript's `JSON.parse` reads.
 *
 * Tip: outside a string JSON holds no backslash, so every backslash met starts an escape.
 */
inline std::pmr::string without_lone_surrogates(std::string_view json,
                                                std::pmr::memory_resource* memory) {
    const auto high = [](std::uint32_t u) { return u >= 0xD800U && u <= 0xDBFFU; };
    const auto low = [](std::uint32_t u) { return u >= 0xDC00U && u <= 0xDFFFU; };
    std::pmr::string out(memory);
    out.reserve(json.size());
    for (std::size_t i = 0; i < json.size();) {
        if (json[i] != '\\') {
            out += json[i++];
            continue;
        }
        const auto unit = unicode_escape_at(json, i);
        if (!unit) {
            out.append(json.substr(i, std::min<std::size_t>(2, json.size() - i)));  // \n, \", ...
            i += 2;
            continue;
        }
        if (const auto next = unicode_escape_at(json, i + 6); high(*unit) && next && low(*next)) {
            out.append(json.substr(i, 12));  // a pair
            i += 12;
        } else if (high(*unit) || low(*unit)) {
            out += "\\ufffd";
            i += 6;
        } else {
            out.append(json.substr(i, 6));
            i += 6;
        }
    }
    return out;
}

/** An answer's metadata read, as 0.27.0's `getResponseMetadata` reads it: only a text `e`
 * refuses; nothing when it names no request. A reason that ends in a lone surrogate, as 0.27.0's
 * cut at 512 UTF-16 code units may leave it, is read with the replacement character in its place.
 */
inline std::optional<response_envelope> read_response(std::string_view metadata,
                                                      const decoding& to) {
    auto v = envelope_of(metadata, to);
    if (!v && metadata.find("\\u") != std::string_view::npos) {
        v = envelope_of(without_lone_surrogates(metadata, to.memory), to);
    }
    if (!v) {
        return std::nullopt;
    }
    const auto e = v->root()["e"];
    return response_envelope{
        .id = std::pmr::string(protocol::json::text_of(v->root()["r"]), to.memory),
        .refused = e.is_string(),
        .reason = std::pmr::string(e.is_string() ? protocol::json::text_of(e) : std::string_view{},
                                   to.memory),
    };
}

/**
 * A user's completion handler, type-erased, and whether its associated executor is the room's,
 * which only its concrete type tells cheaply: a type-erased executor is compared through objects
 * Asio allocates.
 */
template <class Signature>
struct completion {
    /** The user's handler. */
    webcpp::asio::any_completion_handler<Signature> handler;
    /** Whether the handler's associated executor is the room's. */
    bool on_room_executor = false;

    /** The handler's associated allocator type. */
    using allocator_type =
        webcpp::asio::associated_allocator_t<webcpp::asio::any_completion_handler<Signature>>;

    /** The handler's associated allocator, which Asio allocates what carries it with. */
    [[nodiscard]] allocator_type get_allocator() const noexcept {
        return webcpp::asio::get_associated_allocator(handler);
    }

    /** Calls the handler once, with the completion's arguments. */
    template <class... Args>
    void operator()(Args&&... args) && {
        std::move(handler)(std::forward<Args>(args)...);
    }
};

/**
 * Everything a joined room is: the strategy, the negotiations, the peers bound to the app's
 * connections and their handshakes, the action wire and the actions the application made. `room`
 * and `action` are handles to it. Every allocation goes through its resource; what it shares with
 * the app's other rooms (the configuration, the self id, the connections) is the app's.
 *
 * | Internal action   | Meaning                                                           |
 * | \@_hsdata         | a handshake message: the password challenge and its response      |
 * | \@_hsready        | the sender's side of the handshake passed                         |
 * | \@_ping, \@_pong  | round-trip measurement                                            |
 * | \@_leave          | the sender leaves the room                                        |
 * | \@_response       | the answer to a request, `{"r":<id>}` or `{"r":<id>,"e":<reason>}`  |
 * |                   | in its metadata, as 0.27.0's request actions answer               |
 */
class room_impl final : public std::enable_shared_from_this<room_impl>, public room_endpoint {
public:
    /** The executor every handler and completion of the room runs on. */
    using executor_type = webcpp::asio::any_io_executor;
    /** What an action's `on_message` becomes: a listener of each received message. */
    using message_handler = handler<void(incoming&)>;
    /** What a request action's `on_request` becomes: the listener that answers each request. */
    using request_handler = handler<void(incoming&, const request_envelope&)>;

    /** A room of `app`, with its own options copied into `memory`. */
    room_impl(std::shared_ptr<app_impl> app, room_options options, std::string_view room_id,
              std::pmr::memory_resource* memory)
        : memory_(memory),
          app_(std::move(app)),
          ex_(app_->get_executor()),
          factory_(app_->factory()),
          log_(app_->log()),
          options_{
              .max_receive_bytes = options.max_receive_bytes,
              .max_json_document_bytes = options.max_json_document_bytes,
              .memory = memory_,
          },
          handshake_timeout_(app_->handshake_timeout()),
          room_id_(room_id, memory_),
          token_(protocol::room_token(app_->app_id(), room_id_, memory_)),
          root_topic_(protocol::root_topic(app_->app_id(), room_id_, memory_)),
          self_topic_(protocol::peer_topic(app_->app_id(), room_id_, app_->self_id(), memory_)),
          peers_(memory_),
          users_(memory_),
          rebinds_(memory_),
          actions_(memory_),
          requests_(memory_),
          peer_join_(allocator()),
          peer_leave_(allocator()),
          user_join_(allocator()),
          user_leave_(allocator()),
          errors_(allocator()),
          leave_completions_(memory_) {}

    /** Room state represents one membership and cannot be copied. */
    room_impl(const room_impl&) = delete;
    /** Room state cannot be replaced by copying. */
    room_impl& operator=(const room_impl&) = delete;

    /** A room of `app`, in its chosen resource, joined: `start` has run. */
    // NOLINTBEGIN(performance-unnecessary-value-param): a sink, moved on
    static std::shared_ptr<room_impl> make(std::shared_ptr<app_impl> app, room_options options,
                                           std::string_view room_id) {
        // NOLINTEND(performance-unnecessary-value-param)
        std::pmr::memory_resource* const memory =
            options.memory != nullptr ? options.memory : app->memory();
        auto impl = std::allocate_shared<room_impl>(std::pmr::polymorphic_allocator<>(memory),
                                                    std::move(app), options, room_id, memory);
        impl->start();
        return impl;
    }

    /**
     * Leaves the app's registry and lets go quietly: it may run while the io_context is destroyed,
     * so it ends no connection, idles none, calls nothing back and releases no user of the app.
     * Its peers' bindings leave their connections as they go.
     */
    ~room_impl() { app_->unregister_room(token_, this); }

    /** Joins: makes the action wire, the signaling and the strategy, registers the room with its
     * app, and asks for the room's keys, the strategy starting once they exist. */
    void start() {
        const auto weak = weak_from_this();
        make_wire(weak);
        make_signaling(weak);
        make_strategy(weak);
        // A room joined while the app's relays are offline opens none until they are online.
        if (app_->relays_offline()) {
            strategy_->hold();
        }
        register_and_advertise();
    }

    /** Makes the action wire, whose callbacks reach the room while it lives. */
    void make_wire(const std::weak_ptr<room_impl>& weak) {
        wire_ = std::allocate_shared<action_wire>(
            allocator(), factory_, token_, options_.max_receive_bytes,
            action_wire::callbacks{
                .find_link = bind<std::optional<action_wire::link>(std::string_view, bool)>(
                    [weak](std::string_view peer,
                           bool include_pending) -> std::optional<action_wire::link> {
                        const auto self = weak.lock();
                        return self ? self->link_to(peer, include_pending) : std::nullopt;
                    }),
                .peers = bind<std::pmr::vector<std::string_view>(bool)>(
                    [weak, memory = memory_](bool include_pending) {
                        const auto self = weak.lock();
                        return self ? self->peer_views(include_pending)
                                    : std::pmr::vector<std::string_view>(memory);
                    }),
                .may_receive = bind<bool(std::string_view, std::string_view)>(
                    [weak](std::string_view peer, std::string_view name) {
                        const auto self = weak.lock();
                        return self && self->may_receive(peer, name);
                    }),
                .has_action = bind<bool(std::string_view)>([weak](std::string_view name) {
                    const auto self = weak.lock();
                    return self && self->has_action(name);
                }),
                .has_listener = bind<bool(std::string_view)>([weak](std::string_view name) {
                    const auto self = weak.lock();
                    return self && self->listens(name);
                }),
                .deliver = bind<void(incoming&&)>([weak](incoming&& m) {
                    if (const auto self = weak.lock()) {
                        self->on_incoming(std::move(m));
                    }
                }),
                .fail_peer = bind<void(std::string_view, std::string_view)>(
                    [weak](std::string_view peer, std::string_view reason) {
                        if (const auto self = weak.lock()) {
                            self->kick(peer, reason);
                        }
                    }),
                .admit = bind<admission(std::string_view, std::string_view, std::size_t,
                                        std::string_view)>(
                    [weak](std::string_view peer, std::string_view name, std::size_t size,
                           std::string_view metadata) {
                        const auto self = weak.lock();
                        return self ? self->admit(peer, name, size, metadata) : admission::open;
                    }),
                .rejected = bind<void(std::string_view, std::string_view, std::string_view,
                                      std::string_view)>(
                    [weak](std::string_view peer, std::string_view name, std::string_view metadata,
                           std::string_view reason) {
                        if (const auto self = weak.lock()) {
                            self->on_rejected(peer, name, metadata, reason);
                        }
                    }),
                .progress =
                    bind<void(std::string_view, std::string_view, std::string_view, double)>(
                        [weak](std::string_view peer, std::string_view name,
                               std::string_view metadata, double share) {
                            if (const auto self = weak.lock()) {
                                self->post_progress(peer, name, metadata, share);
                            }
                        }),
            },
            memory_);
    }

    /** Makes the signaling, whose callbacks reach the room while it lives. */
    void make_signaling(const std::weak_ptr<room_impl>& weak) {
        signaling_ = std::allocate_shared<signaling>(
            allocator(), factory_, app_->app_id(), room_id_, app_->self_id(), app_->ice_servers(),
            signaling::callbacks{
                .claim = bind<bool(std::string_view)>([weak](std::string_view peer) {
                    const auto self = weak.lock();
                    return self && self->claim(peer);
                }),
                .publish = bind<void(std::size_t, std::string_view, std::string_view)>(
                    [weak](std::size_t relay, std::string_view topic, std::string_view content) {
                        if (const auto self = weak.lock()) {
                            self->strategy_->publish(relay, topic, content);
                        }
                    }),
                .connected = bind<void(std::string_view, std::shared_ptr<backend::peer_connection>,
                                       std::shared_ptr<backend::data_channel>)>(
                    [weak](std::string_view peer, std::shared_ptr<backend::peer_connection> pc,
                           std::shared_ptr<backend::data_channel> channel) {
                        if (const auto self = weak.lock()) {
                            self->on_connected(peer, std::move(pc), std::move(channel));
                        }
                    }),
                .failed = bind<void(const room_error&)>([weak](const room_error& e) {
                    if (const auto self = weak.lock()) {
                        self->report(e);
                    }
                }),
            },
            log_, memory_, app_->memory());
    }

    /** Makes the strategy on the app's relays and the room's topics, whose callbacks reach the
     * room while it lives. */
    void make_strategy(const std::weak_ptr<room_impl>& weak) {
        // The self topic before the root topic, as Trystero's rooms subscribe them, so that the
        // REQ lists them, and their kinds, in 0.27.0's order.
        std::pmr::vector<std::pmr::string> topics(memory_);
        topics.push_back(self_topic_);
        topics.push_back(root_topic_);
        strategy_ = std::allocate_shared<nostr_strategy>(
            allocator(), factory_, app_->relay_urls(), std::move(topics), root_topic_,
            bind<std::pmr::string(std::string_view)>(
                [weak, memory = memory_](std::string_view nonce) {
                    const auto self = weak.lock();
                    return self ? protocol::write_signal({.peer_id = self->app_->self_id()}, nonce,
                                                         memory)
                                : std::pmr::string(memory);
                }),
            bind<void(std::size_t, std::string_view, std::string_view)>(
                [weak](std::size_t relay, std::string_view, std::string_view content) {
                    if (const auto self = weak.lock()) {
                        self->signaling_->handle(relay, content);
                    }
                }),
            log_, memory_, bind<void()>([weak] {
                if (const auto self = weak.lock()) {
                    const auto code = make_error_code(error::relay_failed);
                    std::array<char, 64> buffer{};  // the message without a std::string
                    self->report({}, code, code.message(buffer.data(), buffer.size()));
                }
            }));
    }

    /** Registers the membership, asks for the keys that start discovery, and advertises it on
     * live connections. */
    void register_and_advertise() {
        // By a weak reference: the room leaves the registry before it ends, its destructor
        // included.
        app_->register_room(room_id_, token_,
                            std::static_pointer_cast<room_endpoint>(shared_from_this()));
        app_->add_user();
        // The registry reserves this room before the sink can reenter app::join.
        log_("room", "joining room ", room_id_, " of app ", app_->app_id(), " as ", app_->self_id(),
             " over ", app_->relay_urls().size(), " relays");
        request_keys();
        // As Trystero joins a room: bound over every live connection whose remote advertised it,
        // then announced on every live connection, so that each remote binds in turn.
        const auto connections = app_->live_connections();
        for (const auto& c : connections) {
            try_bind(c->peer(), bind_event::joined);
        }
        for (const auto& c : connections) {
            c->send_presence(token_, true);
        }
    }

    /**
     * Asks the app for the room's keys, which complete after the join returns: a room with a
     * password derives them off its thread. The strategy starts once they exist, so that nothing
     * is announced before the room can seal, as 0.27.0's room waits for `genKey`.
     */
    void request_keys() {
        app_->room_keys().get(room_id_,
                              bind<void(boost::system::error_code, const protocol::room_keys&)>(
                                  [weak = weak_from_this()](boost::system::error_code ec,
                                                            const protocol::room_keys& keys) {
                                      if (const auto self = weak.lock()) {
                                          self->on_keys(ec, keys);
                                      }
                                  }));
    }

    /**
     * Starts the strategy under the room's keys; a room left meanwhile starts nothing. A failed
     * derivation is reported as `error::backend_failure`, 0.27.0 having no room error for it
     * (design D6), and the room announces nothing: a rejoin derives again.
     */
    void on_keys(boost::system::error_code ec, const protocol::room_keys& keys) {
        if (left_) {
            return;
        }
        if (ec) {
            report({}, make_error_code(error::backend_failure),
                   "the room's keys could not be derived from its password, so it announces "
                   "nothing; join it again to retry");
            return;
        }
        signaling_->set_key(keys.aes);
        strategy_->start();
    }

    /** The room's executor. */
    const executor_type& get_executor() const { return ex_; }

    /** The room's resource. */
    std::pmr::memory_resource* memory() const { return memory_; }

    /** What the room decodes a received payload with. */
    detail::decoding decoding() const {
        return {.memory = memory_, .max_json_document_bytes = options_.max_json_document_bytes};
    }

    /** An allocator of the room's resource. */
    std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    /** This peer's id, the app's. */
    std::string_view self_id() const { return app_->self_id(); }

    /** The relays the room uses, the app's. */
    std::span<const std::pmr::string> relay_urls() const { return app_->relay_urls(); }

    /** This peer's user's id; until identities are derived from keys, the peer's own. */
    std::string_view self_user_id() const { return app_->self_id(); }

    /** The id of an active peer's user, or the peer's own id for a peer not active. */
    std::string_view user_of(std::string_view peer) const {
        return users_.user_of(peer).value_or(peer);
    }

    /** The room's users but this peer's own, each with its active peers, in the room's resource:
     * 0.27.0's `getUsers` leaves its own user out. */
    std::pmr::map<std::pmr::string, user_info, std::less<>> users() const {
        std::pmr::map<std::pmr::string, user_info, std::less<>> out(memory_);
        for (const auto& [user, peer_ids] : users_.users()) {
            if (user == self_user_id()) {
                continue;
            }
            out.try_emplace(std::pmr::string(user, memory_),
                            user_info{
                                .peer_ids = std::pmr::vector<std::pmr::string>(peer_ids, memory_),
                                .roles = std::pmr::vector<std::pmr::string>(memory_),
                            });
        }
        return out;
    }

    /** The peers whose handshake completed, each with its user, in the room's resource. */
    std::pmr::map<std::pmr::string, peer_info, std::less<>> peers() const {
        std::pmr::map<std::pmr::string, peer_info, std::less<>> out(memory_);
        for (const auto& [id, p] : peers_) {
            if (p.active) {
                out.try_emplace(std::pmr::string(id, memory_),
                                peer_info{
                                    .user_id = std::pmr::string(user_of(id), memory_),
                                    .direct = true,
                                    .hops = 1,
                                    .roles = std::pmr::vector<std::pmr::string>(memory_),
                                });
            }
        }
        return out;
    }

    /** This peer in the room. */
    self_info self() const {
        return {
            .peer_id = app_->self_id(),
            .user_id = self_user_id(),
            .persistence = identity_persistence::ephemeral,
            .room_id = room_id_,
            .base_room_id = room_id_,
            .is_admin = false,
            .roles = {},
        };
    }

    /** Adds a listener of each peer that becomes active; `replay` calls it first with each active
     * peer. */
    subscription on_peer_join(handler<void(const peer_event&)> f, listen_options options) {
        // Copies: the listener's code may end the room, and free the peers' entries.
        const auto ids = peers();
        std::pmr::vector<peer_event> current(memory_);
        current.reserve(ids.size());
        for (const auto& [id, info] : ids) {
            current.push_back({.peer_id = id, .user_id = info.user_id});
        }
        return peer_join_.add(std::move(f), options, current);
    }

    /** Adds a listener of each active peer that leaves. */
    subscription on_peer_leave(handler<void(const peer_event&)> f, listen_options options) {
        return peer_leave_.add(std::move(f), options);
    }

    /** Adds a listener of each user whose first peer becomes active; `replay` calls it first with
     * each user joined, and its first peer. This peer's own user is no other user. */
    subscription on_user_join(handler<void(const user_event&)> f, listen_options options) {
        // Copies: the listener's code may end the room, and free the users' entries.
        const auto joined = users();
        std::pmr::vector<user_event> current(memory_);
        current.reserve(joined.size());
        for (const auto& [user, info] : joined) {
            current.push_back({.user_id = user, .peer_id = info.peer_ids.front()});
        }
        return user_join_.add(std::move(f), options, current);
    }

    /** Adds a listener of each user whose last active peer leaves. */
    subscription on_user_leave(handler<void(const user_event&)> f, listen_options options) {
        return user_leave_.add(std::move(f), options);
    }

    /** Adds a listener of each problem the room handled itself. */
    subscription on_error(handler<void(const room_error&)> f, listen_options options) {
        return errors_.add(std::move(f), options);
    }

    /** Binds advertised peers when allowed, or detaches a withdrawn token. */
    void presence_changed(std::string_view peer, bool present) override {
        if (present) {
            const auto it = rebinds_.find(peer);
            if (it != rebinds_.end() && it->second.dropping) {
                it->second.dropping = false;
                try_bind(peer, bind_event::reuse);
                return;
            }
            try_bind(peer, bind_event::presence);
        } else if (peers_.contains(peer)) {
            remove_peer(peer, "peer left the room", end_kind::withdrawn);
        }
    }

    /** Whether this peer's unbound frames survive the higher id's dropping window. */
    [[nodiscard]] bool keeps_frames_from(std::string_view peer) const override {
        const auto it = rebinds_.find(peer);
        return it == rebinds_.end() || !it->second.dropping;
    }

    /** Tries to bind a peer after its frame for this room was kept, which advertised the room. */
    void frame_kept(std::string_view peer) override { try_bind(peer, bind_event::frame); }

    /** Holds the room's relays closed, or reconnects them; a room that left has none. */
    void relays_offline(bool offline) override {
        if (left_) {
            return;
        }
        if (offline) {
            strategy_->hold();
        } else {
            strategy_->reconnect();
        }
    }

    /** Forgets history and removes only the binding belonging to the ended connection. */
    void connection_ended(const std::shared_ptr<connection>& ended, std::string_view peer,
                          std::string_view reason) override {
        if (const auto it = rebinds_.find(peer);
            it != rebinds_.end() && it->second.on.lock() == ended) {
            rebinds_.erase(it);
        }
        // The peer may be bound to another connection by now: a late end of an older one is not
        // its end.
        if (const auto it = peers_.find(peer);
            it != peers_.end() && it->second.link->is_on(ended)) {
            remove_peer(peer, reason, end_kind::connection_end);
        }
    }

    /** Registers an action, a request action when `request` says so: from now on its frames are
     * kept for its listeners, where frames of an action not made are dropped, as Trystero drops
     * them. The first make of a name sets its kind; whether this one asks for the same. */
    [[nodiscard]] bool add_action(std::string_view name, bool request) {
        if (const auto* const slot = made(name)) {
            return slot->request == request;
        }
        slot_for(name).request = request;
        return true;
    }

    /** Adds a listener of an action's messages, posts it every message that queued for
     * `async_receive` meanwhile, and delivers what the wire kept for the action. */
    subscription add_message_listener(std::string_view name, message_handler f,
                                      listen_options options) {
        auto& slot = slot_for(name);
        auto out = slot.messages.add(std::move(f), options);
        while (!slot.queue.empty()) {
            post_message(slot, std::move(slot.queue.front()));
            slot.queue.pop_front();
        }
        if (wire_) {
            wire_->listener_added();
        }
        return out;
    }

    /** Sets the listener that answers a request action's requests, unless one is in place. */
    [[nodiscard]] boost::system::result<subscription> add_request_listener(std::string_view name,
                                                                           request_handler f,
                                                                           listen_options options) {
        auto& slot = slot_for(name);
        if (!slot.requests.empty()) {
            return error::already_subscribed;
        }
        auto out = slot.requests.add(std::move(f), options);
        if (wire_) {
            wire_->listener_added();
        }
        return out;
    }

    /** Sets an action's receive gate, unless one is in place. */
    [[nodiscard]] boost::system::result<subscription> add_receive_gate(
        std::string_view name, handler<bool(const receive_info&)> f, listen_options options) {
        auto& slot = slot_for(name);
        if (!slot.gate.empty()) {
            return error::already_subscribed;
        }
        auto out = slot.gate.add(std::move(f), options);
        if (wire_) {
            wire_->listener_added();
        }
        return out;
    }

    /** Adds a listener of the progress of an action's messages. */
    subscription add_progress_listener(std::string_view name,
                                       handler<void(double, const progress_info&)> f,
                                       listen_options options) {
        return slot_for(name).progress.add(std::move(f), options);
    }

    /** Completes with an action's next message: a queued one at once, else the next to arrive;
     * the action listens from then on. */
    void receive(std::string_view name,
                 completion<void(boost::system::error_code, incoming)> handler) {
        if (left_) {
            complete(std::move(handler), make_error_code(error::room_left), incoming{});
            return;
        }
        auto& slot = slot_for(name);
        if (!slot.queue.empty()) {
            auto m = std::move(slot.queue.front());
            slot.queue.pop_front();
            complete(std::move(handler), boost::system::error_code{}, std::move(m));
            return;
        }
        slot.receivers.push_back(std::move(handler));
        if (!std::exchange(slot.awaited, true) && wire_) {
            wire_->listener_added();
        }
    }

    /**
     * Sends a request action's payload to one active peer, and completes with its answer: the
     * message the answer is, `error::request_rejected` when it refuses, or the first failure.
     */
    void request(std::string_view name, encoded_payload payload, const request_options& options,
                 completion<void(boost::system::error_code, incoming)> handler) {
        if (left_) {
            complete(std::move(handler), make_error_code(error::room_left), incoming{});
            return;
        }
        auto peer = peers_.find(options.target);
        if (peer == peers_.end() || !peer->second.active) {
            // A user's peer of the lowest id, as 0.27.0's resolveRequestTarget picks it.
            if (const auto of_user = users_.peers_of(options.target); !of_user.empty()) {
                peer = peers_.find(*std::ranges::min_element(of_user));
            }
        }
        if (peer == peers_.end() || !peer->second.active) {
            complete(std::move(handler), make_error_code(error::not_connected), incoming{});
            return;
        }
        // 0.27.0's crypto.randomUUID(), from the backend's randomness.
        std::array<std::uint8_t, 16> random{};
        factory_.random(random);
        const auto uuid = protocol::uuid_v4(random);
        std::pmr::string id(uuid.data(), uuid.size(), memory_);
        std::pmr::string metadata(R"({"r":)", memory_);
        protocol::json::detail::append_utf8(metadata, id);
        if (!options.metadata.empty()) {
            metadata += R"(,"m":)";
            metadata += options.metadata;
        }
        metadata += '}';
        const auto weak = weak_from_this();
        auto& waiter = requests_
                           .try_emplace(std::pmr::string(id, memory_),
                                        request_waiter{
                                            .peer = std::pmr::string(peer->first, memory_),
                                            .handler = std::move(handler),
                                            .timer = nullptr,
                                        })
                           .first->second;
        if (options.timeout) {
            waiter.timer = factory_.make_timer(memory_);
            waiter.timer->start(
                *options.timeout, bind<void()>([weak, id = std::pmr::string(id, memory_)] {
                    if (const auto self = weak.lock()) {
                        self->finish_request(id, make_error_code(error::request_timed_out), {});
                    }
                }));
        }
        const std::string_view target = waiter.peer;
        wire_->send(name, payload.format, metadata, std::move(payload.ref), std::span(&target, 1),
                    false,
                    bind<void(boost::system::error_code)>(
                        [weak, id = std::move(id)](boost::system::error_code ec) {
                            const auto self = weak.lock();
                            if (ec && self) {
                                self->finish_request(id, ec, {});
                            }
                        }));
    }

    /** Answers a request of `peer` with a payload. */
    void respond(std::string_view peer, std::string_view id, encoded_payload payload) {
        std::pmr::string metadata(R"({"r":)", memory_);
        protocol::json::detail::append_utf8(metadata, id);
        metadata += '}';
        send_response(peer, metadata, std::move(payload));
    }

    /** Refuses a request of `peer` with a reason and a JSON `null` payload. The reason is cut at
     * 512 UTF-16 code units, as 0.27.0's `.slice(0, 512)` cuts it: a character beyond the basic
     * plane counts two, and one cut in two leaves its high surrogate, which `JSON.stringify` writes
     * as `\\udXXX`. */
    void respond_error(std::string_view peer, std::string_view id, std::string_view reason) {
        constexpr std::size_t max_reason_units = 512;
        std::size_t units = 0;
        std::size_t cut = 0;
        std::uint32_t high_surrogate = 0;  // of a character cut in two, or 0
        while (cut < reason.size()) {
            const auto lead = static_cast<unsigned char>(reason[cut]);
            std::size_t length = 1;  // an ASCII byte, or one that leads no sequence
            if (lead >= 0xF0U) {
                length = 4;
            } else if (lead >= 0xE0U) {
                length = 3;
            } else if (lead >= 0xC0U) {
                length = 2;
            }
            length = std::min(length, reason.size() - cut);
            const std::size_t width = length == 4 ? 2 : 1;
            if (units + width > max_reason_units) {
                if (width == 2 && units + 1 == max_reason_units) {
                    const auto byte = [&](std::size_t i) {
                        return static_cast<std::uint32_t>(
                            static_cast<unsigned char>(reason[cut + i]) & 0x3FU);
                    };
                    const std::uint32_t code_point =
                        ((lead & 0x07U) << 18U) | (byte(1) << 12U) | (byte(2) << 6U) | byte(3);
                    high_surrogate = 0xD800U + ((code_point - 0x10000U) >> 10U);
                }
                break;
            }
            units += width;
            cut += length;
        }
        std::pmr::string metadata(R"({"r":)", memory_);
        protocol::json::detail::append_utf8(metadata, id);
        metadata += R"(,"e":)";
        protocol::json::detail::append_utf8(metadata, reason.substr(0, cut));
        if (high_surrogate != 0) {
            static constexpr std::string_view digits = "0123456789abcdef";
            metadata.pop_back();  // the closing quote
            metadata += "\\u";
            for (const unsigned shift : {12U, 8U, 4U, 0U}) {
                metadata += digits[(high_surrogate >> shift) & 15U];
            }
            metadata += '"';
        }
        metadata += '}';
        static constexpr std::string_view null = "null";
        send_response(
            peer, metadata,
            {
                .format = protocol::payload_format::json,
                .ref =
                    {
                        .data = std::span(reinterpret_cast<const std::uint8_t*>(null.data()),
                                          null.size()),
                        .owner = nullptr,
                    },
            });
    }

    /**
     * Sends an action's payload through the wire, and completes once every target's data channel
     * has taken it.
     */
    void send(std::string_view name, encoded_payload payload, const send_options& options,
              completion<void(boost::system::error_code)> handler) {
        if (left_) {
            complete(std::move(handler), make_error_code(error::room_left));
            return;
        }
        const auto weak = weak_from_this();
        const bool include_pending = is_internal_to_pending(name);
        const auto peers = resolve_targets(options.targets, include_pending);
        const std::pmr::vector<std::string_view> targets(peers.begin(), peers.end(), memory_);
        wire_->send(name, payload.format, options.metadata, std::move(payload.ref), targets,
                    include_pending,
                    bind<void(boost::system::error_code)>(
                        [weak, handler = std::move(handler)](boost::system::error_code ec) mutable {
                            if (const auto self = weak.lock()) {
                                self->complete(std::move(handler), ec);
                            }
                        }));
    }

    /**
     * The peers a send's targets name, as 0.27.0's resolveSendTarget reads them: an active peer,
     * or a pending one when the action goes to those, is itself; a user stands for each of its
     * active peers, even for an action that goes to pending ones, where 0.27.0 adds its pending
     * peers too (only the room's own actions go to pending peers, and they name peers); another
     * target stays, which the wire fails as not connected. Each peer once, copied: a send may end
     * a peer while it goes through them.
     */
    std::pmr::vector<std::pmr::string> resolve_targets(std::span<const peer_id> targets,
                                                       bool include_pending) const {
        std::pmr::vector<std::pmr::string> out(memory_);
        const auto add = [&out](std::string_view id) {
            if (std::ranges::find(out, id) == out.end()) {
                out.emplace_back(id);
            }
        };
        for (const auto& target : targets) {
            const auto peer = peers_.find(target);
            if (peer != peers_.end() && (peer->second.active || include_pending)) {
                add(target);
                continue;
            }
            const auto of_user = users_.peers_of(target);
            if (of_user.empty()) {
                add(target);
            }
            for (const auto& id : of_user) {
                add(id);
            }
        }
        return out;
    }

    /** Sends `\@_ping` to an active peer, and completes with the time until its `\@_pong`. */
    void ping(std::string_view peer,
              completion<void(boost::system::error_code, std::chrono::milliseconds)> handler) {
        const auto it = peers_.find(peer);
        if (left_ || it == peers_.end() || !it->second.active) {
            complete(std::move(handler),
                     make_error_code(left_ ? error::room_left : error::no_such_peer),
                     std::chrono::milliseconds{});
            return;
        }
        it->second.pings.push_back(
            {.started = steady_now(factory_), .handler = std::move(handler)});
        send_internal("@_ping", it->first);
    }

    /** Says `\@_leave` to every peer, then ends the room after Trystero's 99 ms. A leave asked
     * during the farewell completes with the first; one asked once the room ended, at once. */
    void leave(completion<void(boost::system::error_code)> handler) {
        if (ended_) {
            complete(std::move(handler), boost::system::error_code{});
            return;
        }
        leave_completions_.push_back(std::move(handler));
        if (std::exchange(left_, true)) {
            return;
        }
        log_("room", "leaving room ", room_id_);
        if (ended_) {
            return;  // a rejoin or destruction in the sink already finished this farewell
        }
        for (const auto& [id, p] : peers_) {
            send_internal("@_leave", id);
        }
        // Trystero gives the farewell 99 ms to leave before closing the connections. The timer
        // holds the room weakly: a handle destroyed meanwhile ends the farewell itself (abandon).
        leave_timer_ = factory_.make_timer(memory_);
        leave_timer_->start(std::chrono::milliseconds(99), bind<void()>([weak = weak_from_this()] {
                                if (const auto self = weak.lock()) {
                                    self->finish_farewell();
                                }
                            }));
    }

    /** The room handle was destroyed: leaves at once, without the farewell's grace, or ends the
     * farewell that runs. */
    void abandon() {
        if (!std::exchange(left_, true)) {
            for (const auto& [id, p] : peers_) {
                send_internal("@_leave", id);
            }
        }
        finish_farewell();
    }

    /** Whether this room is still in its farewell interval. */
    [[nodiscard]] bool leaving() const override { return left_ && !ended_; }

    /** Ends the room now: its presence(0) sent, the room unregistered and the app's user released
     * (close_everything), then every pending leave completed, once. */
    void finish_farewell() override {
        if (std::exchange(ended_, true)) {
            return;
        }
        close_everything();
        auto completions = std::move(leave_completions_);
        leave_completions_.clear();
        for (auto& h : completions) {
            complete(std::move(h), boost::system::error_code{});
        }
    }

    /**
     * Posts a user's completion, allocated as its own associated allocator says.
     *
     * Tip: a completion whose associated executor is the room's runs from an operation posted
     * there; one bound to another executor is handed to it by Asio, which reaches a type-erased
     * executor through a function object it allocates with `std::allocator` (measured:
     * `std::aligned_alloc` through its recycling cache).
     */
    template <class Handler, class... Args>
    void complete(Handler&& handler, Args&&... args) {
        if (on_room_executor(handler)) {
            const auto allocator = webcpp::asio::get_associated_allocator(handler);
            post_handler(ex_, webcpp::asio::bind_allocator(
                                  allocator, [handler = std::forward<Handler>(handler),
                                              ... args = std::forward<Args>(args)]() mutable {
                                      std::move(handler)(std::move(args)...);
                                  }));
            return;
        }
        if constexpr (is_completion<std::decay_t<Handler>>) {
            webcpp::asio::post(
                ex_, webcpp::asio::append(std::move(handler.handler), std::forward<Args>(args)...));
        } else {
            webcpp::asio::post(ex_, webcpp::asio::append(std::forward<Handler>(handler),
                                                         std::forward<Args>(args)...));
        }
    }

    /** A user's completion handler, type-erased, with whether it runs on the room's executor. */
    template <class Signature, class Handler>
    completion<Signature> erase(Handler&& handler) const {
        const bool here = on_room_executor(handler);
        return {
            webcpp::asio::any_completion_handler<Signature>(std::forward<Handler>(handler)),
            here,
        };
    }

    /** `f` as a handler of the room's resource. */
    template <class Signature, class F>
    handler<Signature> bind(F&& f) const {
        return handler<Signature>(std::allocator_arg, allocator(), std::forward<F>(f));
    }

private:
    template <class T>
    static constexpr bool is_completion = false;
    template <class Signature>
    static constexpr bool is_completion<completion<Signature>> = true;

    /** Whether a handler's associated executor is the room's, told by its concrete type. */
    template <class Handler>
    bool on_room_executor(const Handler& handler) const {
        if constexpr (is_completion<Handler>) {
            return handler.on_room_executor;
        } else if constexpr (std::is_same_v<
                                 webcpp::asio::associated_executor_t<Handler, executor_type>,
                                 executor_type>) {
            return webcpp::asio::get_associated_executor(handler, ex_) == ex_;
        } else {
            return false;
        }
    }

    /** How a binding ended: the peer's \@_leave, its presence(0), a kick of this room, the
     * connection's end, or this room's own leave. */
    enum class end_kind { peer_left, withdrawn, kick, connection_end, room_end };

    struct pending_ping {
        detail::time_point started{};
        completion<void(boost::system::error_code, std::chrono::milliseconds)> handler;
    };

    struct peer_state {
        explicit peer_state(std::pmr::polymorphic_allocator<> a) : challenge(a), pings(a) {}

        std::shared_ptr<binding> link;
        bool active = false;
        bool local_passed = false;
        bool remote_ready = false;
        bool challenge_sent = false;
        std::pmr::string challenge;  // the initiator's, while it waits for the response
        std::shared_ptr<backend::timer> handshake_timer;
        std::pmr::deque<pending_ping> pings;
    };

    struct action_slot {
        explicit action_slot(std::pmr::polymorphic_allocator<> a)
            : messages(a), requests(a), gate(a), progress(a), queue(a), receivers(a) {}

        /** Whether its messages are requests, which its request listener answers. */
        bool request = false;
        /** Whether `async_receive` was called: the action listens from then on, its messages
         * queuing for the next receive. */
        bool awaited = false;
        listeners<void(incoming&)> messages;
        /** One at a time. */
        listeners<void(incoming&, const request_envelope&)> requests;
        /** One at a time. */
        listeners<bool(const receive_info&)> gate;
        listeners<void(double, const progress_info&)> progress;
        /** The messages that arrived while no listener and no receive waited, for the next. */
        std::pmr::deque<incoming> queue;
        std::pmr::deque<completion<void(boost::system::error_code, incoming)>> receivers;
    };

    /** A request sent, waiting for its answer. */
    struct request_waiter {
        std::pmr::string peer;
        completion<void(boost::system::error_code, incoming)> handler;
        std::shared_ptr<backend::timer> timer;
    };

    static constexpr std::size_t max_queued_messages = 1024;

    /** The slot of an action the room made, or null. */
    action_slot* made(std::string_view name) {
        const auto it = actions_.find(name);
        return it == actions_.end() ? nullptr : &it->second;
    }

    template <class F>
    void post(F&& f) {
        detail::post(ex_, memory_, std::forward<F>(f));
    }

    action_slot& slot_for(std::string_view name) {
        auto it = actions_.find(name);
        if (it == actions_.end()) {
            it = actions_.try_emplace(std::pmr::string(name, memory_), allocator()).first;
        }
        return it->second;
    }

    static bool is_internal_to_pending(std::string_view name) {
        return name == "@_leave" || name == "@_hsdata" || name == "@_hsready";
    }

    /** Whether the room has the action: it was made, or it is internal. */
    bool has_action(std::string_view name) const {
        return name.starts_with("@_") || actions_.contains(name);
    }

    /** Whether an action the room has listens, 0.27.0's receiver: an internal one always; a
     * request action while its request listener is in place; another while it has a message
     * listener, and from its first `async_receive` on. Its frames wait in the wire meanwhile. */
    bool listens(std::string_view name) const {
        if (name.starts_with("@_")) {
            return true;
        }
        const auto it = actions_.find(name);
        if (it == actions_.end()) {
            return false;
        }
        const auto& slot = it->second;
        return slot.request ? !slot.requests.empty() : !slot.messages.empty() || slot.awaited;
    }

    /** The metadata a listener of an action reads: a request's own, inside its envelope; nothing
     * when a request's names none. */
    std::optional<std::pmr::string> user_metadata(const action_slot& slot,
                                                  std::string_view metadata) const {
        if (!slot.request) {
            return std::pmr::string(metadata, memory_);
        }
        auto envelope = read_request(metadata, decoding());
        if (!envelope) {
            return std::nullopt;
        }
        return std::move(envelope->metadata);
    }

    /** Asks an action's receive gate about a message, as the wire receives it. The gate runs
     * during the call, and may end the peer or the room. */
    admission admit(std::string_view peer, std::string_view name, std::size_t size,
                    std::string_view metadata) {
        auto* const slot = made(name);
        if (slot == nullptr || slot->gate.empty()) {
            return admission::open;
        }
        const auto own = user_metadata(*slot, metadata);
        if (!own) {
            return admission::refused;  // a request without its envelope, which nothing answers
        }
        // A copy: the gate may end the room, and free the users' entries.
        const std::pmr::string user(user_of(peer), memory_);
        const receive_info info{
            .peer_id = peer,
            .user_id = user,
            .byte_length = size,
            .metadata = *own,
        };
        return slot->gate.call_first(info) ? admission::admitted : admission::refused;
    }

    /** The wire refused a message of an action: a request is answered with the reason, as 0.27.0's
     * request actions answer it, so that the requester need not wait. */
    void on_rejected(std::string_view peer, std::string_view name, std::string_view metadata,
                     std::string_view reason) {
        const auto* const slot = made(name);
        if (slot == nullptr || !slot->request || left_) {
            return;
        }
        if (const auto envelope = read_request(metadata, decoding())) {
            respond_error(peer, envelope->id, reason);
        }
    }

    /** Posts the progress of a message of an action to its progress listeners. */
    void post_progress(std::string_view peer, std::string_view name, std::string_view metadata,
                       double share) {
        auto* const slot = made(name);
        if (slot == nullptr || slot->progress.empty()) {
            return;
        }
        auto own = user_metadata(*slot, metadata);
        if (!own) {
            return;
        }
        post([weak = weak_from_this(), slot, share, peer = std::pmr::string(peer, memory_),
              user = std::pmr::string(user_of(peer), memory_), metadata = std::move(*own)] {
            // Held for the whole emission: a listener may release the room's last handles.
            if (const auto self = weak.lock()) {
                slot->progress.emit(share,
                                    {.peer_id = peer, .user_id = user, .metadata = metadata});
            }
        });
    }

    /**
     * Posts a message to an action's listeners, or, when none is left once the post runs, to its
     * next receive. A slot lives as long as its room.
     *
     * Tip: `hand_over` posts here again when listeners came back meanwhile, a loop through the
     * executor and not a recursion, since each post runs once the call that made it has returned.
     * It is called through a pointer, so that the analysis of calls (misc-no-recursion) sees no
     * cycle through Asio's post.
     */
    void post_message(action_slot& slot, incoming&& m) {
        post([weak = weak_from_this(), slot = &slot, m = std::move(m),
              hand_over_to = &room_impl::hand_over]() mutable {
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            if (slot->messages.empty()) {
                ((*self).*hand_over_to)(*slot, std::move(m));
                return;
            }
            m.shared = slot->messages.size() > 1;
            slot->messages.emit(m);
        });
    }

    bool may_receive(std::string_view peer, std::string_view name) const {
        const auto it = peers_.find(peer);
        return it != peers_.end() && (it->second.active || is_internal_to_pending(name));
    }

    std::pmr::vector<std::string_view> peer_views(bool include_pending) const {
        std::pmr::vector<std::string_view> out(memory_);
        out.reserve(peers_.size());
        for (const auto& [id, p] : peers_) {
            if (p.active || include_pending) {
                out.emplace_back(id);
            }
        }
        return out;
    }

    std::optional<action_wire::link> link_to(std::string_view peer, bool include_pending) {
        const auto it = peers_.find(peer);
        if (it == peers_.end() || (!it->second.active && !include_pending)) {
            return std::nullopt;
        }
        return action_wire::link{std::weak_ptr<channel_link>(it->second.link)};
    }

    /** An internal action, inline and to one peer: the text is written into the frame during the
     * call. */
    void send_internal(std::string_view name, std::string_view peer,
                       protocol::payload_format format = protocol::payload_format::text,
                       std::string_view text = {}) {
        if (!wire_) {
            return;
        }
        const payload_ref ref{
            .data = std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
            .owner = nullptr,
        };
        wire_->send(name, format, {}, ref, std::span(&peer, 1), true,
                    bind<void(boost::system::error_code)>([](boost::system::error_code) {}));
    }

    /** Reports a problem with `peer`, or with none, to the room's error listeners. */
    void report(std::string_view peer, boost::system::error_code ec, std::string_view message) {
        room_error e;
        e.code = ec;
        e.message = message;
        e.peer_id = peer;
        report(e);
    }

    /**
     * Logs a problem the room handled itself and posts its report to the error listeners, with
     * the app's and the room's ids: what is in place when the post runs, unless the room was left
     * by then, as 0.27.0 reports nothing once its room is leaving.
     */
    void report(const room_error& e) {
        // Copies: the post runs after the views' storage may be gone.
        struct owned {
            boost::system::error_code code;
            std::pmr::string message;
            std::pmr::string peer_id;
            std::optional<signal_stage> stage;
            bool turn_configured;
            std::pmr::string reason;
            std::pmr::string plugin;
        };

        owned copy{
            .code = e.code,
            .message = std::pmr::string(e.message, memory_),
            .peer_id = std::pmr::string(e.peer_id, memory_),
            .stage = e.stage,
            .turn_configured = e.turn_configured,
            .reason = std::pmr::string(e.reason, memory_),
            .plugin = std::pmr::string(e.plugin, memory_),
        };
        if (copy.peer_id.empty()) {
            log_("room", copy.message);
        } else {
            log_("room", "peer ", copy.peer_id, ": ", copy.message);
        }
        post([weak = weak_from_this(), copy = std::move(copy)] {
            const auto self = weak.lock();
            if (!self || self->left_) {
                return;
            }
            self->errors_.emit({
                .code = copy.code,
                .message = copy.message,
                .app_id = self->app_->app_id(),
                .room_id = self->room_id_,
                .peer_id = copy.peer_id,
                .stage = copy.stage,
                .turn_configured = copy.turn_configured,
                .reason = copy.reason,
                .plugin = copy.plugin,
            });
        });
    }

    /** What makes a room bind a peer over a live connection: its own join, the peer's presence, a
     * frame of the room the connection kept, the reuse of the connection by a signal, or the room's
     * rebinding time. */
    enum class bind_event { joined, presence, frame, reuse, rebinding };

    /** A live app connection consumes signals; only the first meeting may bind by reuse. */
    bool claim(std::string_view peer) {
        if (!app_->find(peer)) {
            return false;
        }
        if (!met(peer)) {
            try_bind(peer, bind_event::reuse);
        }
        return true;
    }

    /** Binds `peer` over the app's live connection to it when every condition holds: the peer has
     * itself advertised this room's token on that connection, this room is not leaving, no backoff
     * of this room runs for the peer, and the peer is not bound already. Whether it bound.
     *
     * Tip: the advertisement is the peer's own, its presence or a room frame of the token, on a
     * connection only it can write to, so that an announcement or a signal anyone publishes in its
     * name binds nothing. */
    bool try_bind(std::string_view peer, bind_event event) {
        if (left_ || peers_.contains(peer)) {
            return false;
        }
        const auto c = app_->find(peer);
        if (!c || !c->advertised(token_) || refusing(peer)) {
            return false;
        }
        if (event == bind_event::reuse || event == bind_event::rebinding) {
            c->send_presence(token_, true);
        }
        bind_peer(peer, c);
        return true;
    }

    /** Whether this room has already met `peer` over the current shared connection. */
    [[nodiscard]] bool met(std::string_view peer) const {
        const auto it = rebinds_.find(peer);
        return it != rebinds_.end() && it->second.met;
    }

    /** Whether the lower id is refusing a binding during its backoff. */
    [[nodiscard]] bool refusing(std::string_view peer) const {
        const auto it = rebinds_.find(peer);
        return it != rebinds_.end() && it->second.refusing;
    }

    /** Whether a kept frame is a `\@_leave`, which ends a binding of the other side. */
    static bool is_leave(std::span<const std::uint8_t> frame) {
        const auto e = protocol::decode_envelope(frame);
        const auto d = e ? protocol::decode_action(e->payload) : protocol::decode_result{};
        const auto* p = d.frame ? std::get_if<protocol::proposal>(&*d.frame) : nullptr;
        return p != nullptr && p->name == "@_leave";
    }

    /** Delivers the frames `c` kept for this room, in order, but a kept \@_leave and everything
     * kept before it, which belong to a binding of the other side that has ended. Stops once the
     * peer's entry is gone or holds another binding. */
    void deliver_kept(const std::pmr::string& peer, connection& c, const detail::binding* mine) {
        auto frames = c.take_kept(token_);
        std::size_t first = 0;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            if (is_leave(frames[i])) {
                first = i + 1;
            }
        }
        for (std::size_t i = first; i < frames.size(); ++i) {
            if (const auto it = peers_.find(peer);
                it == peers_.end() || it->second.link.get() != mine) {
                return;
            }
            const auto envelope = protocol::decode_envelope(frames[i]);
            BOOST_ASSERT(envelope);
            const auto action = envelope->payload;
            wire_->handle(peer, std::move(frames[i]), action);
        }
    }

    /** A negotiation of this room opened a channel to `peer`: the app adopts the connection, or
     * keeps the one it has to the peer, and the room binds the peer to it.
     *
     * Tip: the adoption may end a stale connection to the peer, whose end reaches this room
     * synchronously and may remove the peer; nothing of the room is held across it, and the id is
     * this call's own copy. */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    void on_connected(std::string_view peer_view, std::shared_ptr<backend::peer_connection> pc,
                      std::shared_ptr<backend::data_channel> channel) {
        // NOLINTEND(performance-unnecessary-value-param)
        if (left_) {
            channel->set_handlers({});
            pc->set_handlers({});
            channel->close();
            pc->close();
            return;
        }
        const std::pmr::string peer(peer_view, memory_);
        const auto c = app_->adopt(peer, std::move(pc), std::move(channel));
        // Adoption logs synchronously: the sink may leave or destroy this room, or end the
        // connection. A farewell still counts as leaving and must not gain a new binding.
        if (left_) {
            if (ended_) {
                c->idle_if_unused(app_->idle_timeout());
            } else {
                departing_connection_ = c;
            }
        } else if (c->live()) {
            bind_peer(peer, c);
        } else {
            // A native transport can close while adoption calls the log sink. No binding
            // exists yet, so its queued end cannot remove_peer and forget this negotiation.
            signaling_->forget(peer);
            strategy_->restart_announcements();
        }
    }

    /** Binds this room to `c` for `peer`, in the order the specification sets: the negotiation
     * marked connected, the peer's state and handshake timer, the frames the connection kept for
     * this room, then the handshake's first message, as Trystero adds a peer before it flushes.
     *
     * Tip: the peer's id is copied first, since every step may run code that removes the peer's
     * entry (a kick); the entry is never held by reference across such a step, and each step
     * looks it up again. Each lambda keeps its own copy in the room's resource, named: a plain copy
     * of a std::pmr::string takes the default resource. */
    void bind_peer(std::string_view peer_view, const std::shared_ptr<connection>& c) {
        // A peer is bound once until its removal, as the connection's attach requires: the
        // signaling hands over no channel while its entry is connected, which `on_channel_open`
        // and `bound` set and only `forget` clears, after `remove_peer` has taken the peer out.
        BOOST_ASSERT(!peers_.contains(peer_view));
        const std::pmr::string peer(peer_view, memory_);
        const auto weak = weak_from_this();
        signaling_->bound(peer);
        auto& history =
            rebinds_.try_emplace(std::pmr::string(peer, memory_), allocator()).first->second;
        history.met = true;
        history.on = c;
        // This binding, held until the call returns: a kept frame may remove the peer's entry and
        // free it, and another made meanwhile must not be taken for it at the same address.
        std::shared_ptr<const binding> mine;
        {
            auto& state = peers_.try_emplace(peer, allocator()).first->second;
            state.link = std::allocate_shared<binding>(
                allocator(), c, token_,
                bind<void(backend::bytes&&, std::span<const std::uint8_t>)>(
                    [weak, peer = std::pmr::string(peer, memory_)](
                        backend::bytes&& frame, std::span<const std::uint8_t> action) {
                        if (const auto self = weak.lock()) {
                            self->wire_->handle(peer, std::move(frame), action);
                        }
                    }),
                memory_);
            state.handshake_timer = factory_.make_timer(memory_);
            state.handshake_timer->start(
                handshake_timeout_, bind<void()>([weak, peer = std::pmr::string(peer, memory_)] {
                    const auto self = weak.lock();
                    if (!self) {
                        return;
                    }
                    self->report(peer, make_error_code(error::handshake_timed_out),
                                 "handshake timed out");
                    self->kick(peer, "handshake timed out");
                }));
            mine = state.link;
        }
        deliver_kept(peer, *c, mine.get());
        if (const auto it = peers_.find(peer);
            it != peers_.end() && it->second.link.get() == mine.get()) {
            start_handshake(it->first, it->second);
        }
    }

    void start_handshake(std::string_view peer, peer_state& state) {
        if (app_->password().empty()) {
            pass_local(peer);
            return;
        }
        if (app_->self_id() < peer) {
            state.challenge = detail::random_id(factory_, 36, memory_);  // Trystero's genId(36)
            if (const auto history = rebinds_.find(peer); history != rebinds_.end()) {
                history->second.challenges.push(std::pmr::string(state.challenge, memory_));
            }
            std::pmr::string text(R"({"__trystero_pw":"challenge","c":)", memory_);
            protocol::json::detail::append_utf8(text,
                                                state.challenge);  // drawn from base62_alphabet
            text += '}';
            send_internal("@_hsdata", peer, protocol::payload_format::json, text);
            state.challenge_sent = true;
        }
    }

    void on_handshake_data(std::string_view peer, std::string_view data) {
        const auto it = peers_.find(peer);
        if (it == peers_.end() || it->second.active || it->second.local_passed) {
            return;
        }
        // A message past its bound fails the handshake as one of the wrong shape does.
        const auto value = webcpp::document::parse(
            data, protocol::json::protocol_limits(protocol::max_handshake_bytes),
            std::pmr::polymorphic_allocator<std::byte>(memory_));
        const protocol::json::const_value o = value.has_value() && value->root().is_object()
                                                  ? value->root()
                                                  : protocol::json::const_value();
        const auto field = [&o](std::string_view key) { return protocol::json::text_of(o[key]); };
        if (app_->self_id() < peer) {
            const auto h = field("h");
            const auto r = rebinds_.find(peer);
            const bool proves = field("__trystero_pw") == "response" && r != rebinds_.end() &&
                                r->second.challenges.any_of(
                                    [&](std::string_view c) { return h == handshake_proof(c); });
            if (proves) {
                pass_local(peer);
                return;
            }
            if (!it->second.challenge_sent) {
                return;  // a late answer of an ended binding, before this one's challenge
            }
            fail_handshake(peer);
            return;
        }
        if (field("__trystero_pw") != "challenge" || !o["c"].is_string()) {
            fail_handshake(peer);
            return;
        }
        std::pmr::string text(R"({"__trystero_pw":"response","h":)", memory_);
        protocol::json::detail::append_utf8(text, handshake_proof(field("c")));
        text += '}';
        send_internal("@_hsdata", peer, protocol::payload_format::json, text);
        pass_local(peer);
    }

    /**
     * The proof of the password handshake over a shared connection, 0.26.0's `hashChallenge`: the
     * hexadecimal SHA-256 of "<challenge>:<password>:<app>:<room>". Provisional: 0.27.0 has no
     * such handshake, and Phase 2's dual-signature handshake (Task 2.4) replaces it.
     */
    std::pmr::string handshake_proof(std::string_view challenge) const {
        const auto digest = protocol::sha256_of(
            {challenge, ":", app_->password(), ":", app_->app_id(), ":", room_id_});
        return protocol::to_hex(digest, memory_);
    }

    void fail_handshake(std::string_view peer_view) {
        const std::pmr::string peer(peer_view, memory_);
        report(peer, make_error_code(error::wrong_password),
               "incorrect password for overlapping room");
        kick(peer, "handshake failed");
    }

    void pass_local(std::string_view peer) {
        const auto it = peers_.find(peer);
        if (it == peers_.end()) {
            return;
        }
        it->second.local_passed = true;
        send_internal("@_hsready", it->first);
        maybe_activate(it->first);
    }

    void maybe_activate(std::string_view peer) {
        const auto it = peers_.find(peer);
        if (it == peers_.end() || it->second.active || !it->second.local_passed ||
            !it->second.remote_ready) {
            return;
        }
        it->second.active = true;
        // Its user joins with its first peer, as 0.27.0 counts a user's peers; this peer's own user
        // is no other user.
        std::pmr::string user(app_->user_of(peer), memory_);
        const bool first_of_user = users_.add(peer, user) && user != self_user_id();
        if (const auto history = rebinds_.find(peer); history != rebinds_.end()) {
            history->second.ends = 0;
            history->second.challenges.clear();
            history->second.timer.reset();
        }
        it->second.handshake_timer.reset();
        std::pmr::string peer_copy(it->first, memory_);
        const auto link = it->second.link;
        log_("room", "peer joined: ", peer_copy);
        // The sink may leave the room or remove this binding before notification is posted.
        const auto current = peers_.find(peer_copy);
        if (!left_ && current != peers_.end() && current->second.link == link) {
            // A user joins after its first peer, as 0.27.0 reports it.
            // A room left before the post runs reports no join, as 0.27.0 reports none once its
            // room is leaving.
            post([weak = weak_from_this(), peer = std::move(peer_copy), user = std::move(user),
                  first_of_user] {
                const auto self = weak.lock();
                if (!self || self->left_) {
                    return;
                }
                self->peer_join_.emit({.peer_id = peer, .user_id = user});
                if (first_of_user) {
                    self->user_join_.emit({.user_id = user, .peer_id = peer});
                }
            });
        }
    }

    void on_incoming(incoming&& m) {
        const auto name = m.name();
        const auto it_peer = peers_.find(m.peer);
        if (name == "@_hsready") {
            if (it_peer != peers_.end() && !it_peer->second.active) {
                it_peer->second.remote_ready = true;
                maybe_activate(it_peer->first);
            }
        } else if (name == "@_hsdata") {
            if (it_peer != peers_.end()) {
                on_handshake_data(it_peer->first, as_text(m.payload()));
            }
        } else if (name == "@_ping") {
            if (it_peer != peers_.end()) {
                send_internal("@_pong", it_peer->first);
            }
        } else if (name == "@_pong") {
            if (it_peer != peers_.end() && !it_peer->second.pings.empty()) {
                auto ping = std::move(it_peer->second.pings.front());
                it_peer->second.pings.pop_front();
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    steady_now(factory_) - ping.started);
                complete(std::move(ping.handler), boost::system::error_code{}, elapsed);
            }
        } else if (name == "@_leave") {
            remove_peer(m.peer, "peer left the room", end_kind::peer_left);
        } else if (name == "@_response") {
            on_response(std::move(m));
        } else if (name.starts_with("@_")) {
            // Media and renegotiation actions: not implemented yet (doc: #trystero-roadmap).
        } else if (const auto it = actions_.find(name); it != actions_.end()) {
            deliver_action(it->first, it->second, std::move(m));
        }
    }

    /** Delivers an application message: its progress is complete, and it goes to the request
     * listener, the message listeners, or the next receive, in that order. */
    void deliver_action(std::string_view name, action_slot& slot, incoming&& message) {
        post_progress(message.peer, name, message.metadata(), 1.0);
        if (slot.request) {
            deliver_request(slot, std::move(message));
        } else if (!slot.messages.empty()) {
            post_message(slot, std::move(message));
        } else {
            hand_over(slot, std::move(message));
        }
    }

    /** Hands a message to the next receive, or queues it for one, rechecking consumers after a
     * queue-limit log sink. */
    void hand_over(action_slot& slot, incoming&& message) {
        // A message posted before a leave that finds no listener once it runs goes with the room,
        // as the rest of what waited at the leave went: a receive or listener added since gets
        // nothing.
        if (left_) {
            return;
        }
        if (slot.messages.empty() && slot.receivers.empty() &&
            slot.queue.size() >= max_queued_messages) {
            log_("action", "dropping the oldest unread message of ", message.name());
            if (left_) {
                return;
            }
            // Adding a listener or receiving from the sink can drain the queue synchronously.
            if (slot.queue.size() >= max_queued_messages) {
                slot.queue.pop_front();
            }
        }
        if (!slot.receivers.empty()) {
            auto receiver = std::move(slot.receivers.front());
            slot.receivers.pop_front();
            complete(std::move(receiver), boost::system::error_code{}, std::move(message));
        } else if (!slot.messages.empty()) {
            post_message(slot, std::move(message));
        } else {
            slot.queue.push_back(std::move(message));
        }
    }

    /** Posts a request to the listener that answers it; one whose metadata names no request is
     * dropped, as 0.27.0 drops it, and one that finds no listener once the post runs is refused. */
    void deliver_request(action_slot& slot, incoming&& message) {
        auto envelope = read_request(message.metadata(), decoding());
        if (!envelope) {
            return;
        }
        post([weak = weak_from_this(), slot = &slot, envelope = std::move(*envelope),
              m = std::move(message)]() mutable {
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            if (slot->requests.empty()) {
                self->respond_error(m.peer, envelope.id, "no active request handler");
                return;
            }
            slot->requests.call_first(m, envelope);
        });
    }

    /** An answer to a request: it completes the request of its id, if the peer it was sent to
     * answered it. */
    void on_response(incoming&& m) {
        const auto envelope = read_response(m.metadata(), decoding());
        if (!envelope) {
            return;
        }
        const auto it = requests_.find(envelope->id);
        if (it == requests_.end() || it->second.peer != m.peer) {
            return;
        }
        if (envelope->refused) {
            // The request completes with the code alone; the reason the peer gave is logged.
            log_("action", "request ", envelope->id, " refused by ", m.peer, ": ",
                 envelope->reason);
        }
        finish_request(envelope->id,
                       envelope->refused ? make_error_code(error::request_rejected)
                                         : boost::system::error_code{},
                       std::move(m));
    }

    /** Completes a request once, with its answer or its failure. */
    void finish_request(std::string_view id, boost::system::error_code ec, incoming&& m) {
        const auto it = requests_.find(id);
        if (it == requests_.end()) {
            return;
        }
        // A node handle's mapped value is mutable through a const handle.
        const auto node = requests_.extract(it);
        if (node.mapped().timer) {
            node.mapped().timer->cancel();
        }
        complete(std::move(node.mapped().handler), ec, std::move(m));
    }

    /** Fails every request sent to `peer`, or every request when it is empty. */
    void fail_requests(std::string_view peer, boost::system::error_code ec) {
        std::pmr::vector<std::pmr::string> ids(memory_);
        for (const auto& [id, w] : requests_) {
            if (peer.empty() || w.peer == peer) {
                ids.emplace_back(id);  // in the vector's resource, the room's
            }
        }
        for (const auto& id : ids) {
            finish_request(id, ec, {});
        }
    }

    /** Sends an answer, inline or as a transfer, to one peer, the room not left. */
    void send_response(std::string_view peer, std::string_view metadata, encoded_payload payload) {
        if (left_ || !wire_) {
            return;
        }
        wire_->send("@_response", payload.format, metadata, std::move(payload.ref),
                    std::span(&peer, 1), false,
                    bind<void(boost::system::error_code)>([](boost::system::error_code) {}));
    }

    void kick(std::string_view peer, std::string_view reason) {
        const auto it = peers_.find(peer);
        if (it == peers_.end()) {
            return;
        }
        send_internal("@_leave", it->first);
        remove_peer(it->first, reason, end_kind::kick);
    }

    /** Detaches a peer's binding as its end says: the connection idles after a departure, this
     * room's own leave among them, and ends after a kick of its last room; at the connection's
     * end, the connection has let the binding go already. */
    void let_go(peer_state& state, end_kind kind) {
        if (kind == end_kind::connection_end) {
            return;
        }
        state.link->detach(kind == end_kind::kick, app_->idle_timeout());
    }

    /** After a binding with `peer` ended over a live connection, by a kick of either side: the
     * next binding at one step (detail::rebinding_step); meanwhile the lower id refuses after its
     * own kick, and the higher id drops the peer's frames after its own kick, until it sends
     * presence. */
    void schedule_rebinding(std::string_view peer, bool own_kick) {
        auto& r = rebinds_.try_emplace(std::pmr::string(peer, memory_), allocator()).first->second;
        ++r.ends;
        const bool lower = app_->self_id() < peer;
        r.refusing = own_kick && lower;
        r.dropping = own_kick && !lower;
        r.timer = factory_.make_timer(memory_);
        r.timer->start(
            rebinding_step(handshake_timeout_, r.ends),
            bind<void()>([weak = weak_from_this(), peer = std::pmr::string(peer, memory_)] {
                if (const auto self = weak.lock()) {
                    self->on_rebinding_time(peer);
                }
            }));
    }

    void on_rebinding_time(std::string_view peer) {
        const auto it = rebinds_.find(peer);
        if (it == rebinds_.end()) {
            return;
        }
        it->second.refusing = false;
        if (peers_.contains(peer)) {
            return;  // something bound it before its time
        }
        if (const auto c = app_->find(peer); c && c->advertised(token_)) {
            it->second.dropping = false;
            try_bind(peer, bind_event::rebinding);
        }
    }

    void remove_peer(std::string_view peer_view, std::string_view reason, end_kind kind) {
        const auto it = peers_.find(peer_view);
        if (it == peers_.end()) {
            return;
        }
        // The id is moved out of the map before the entry goes: `peer_view` may refer to it.
        const auto node = peers_.extract(it);
        std::pmr::string peer = std::move(node.key());
        auto& state = node.mapped();
        // Its user leaves with its last peer, as 0.27.0 counts a user's peers.
        std::pmr::string user(user_of(peer), memory_);
        const bool last_of_user =
            state.active && users_.remove(peer).has_value() && user != self_user_id();
        log_("room", "peer ", peer, " gone: ", reason);
        let_go(state, kind);
        for (auto& ping : state.pings) {
            complete(std::move(ping.handler), make_error_code(error::peer_disconnected),
                     std::chrono::milliseconds{});
        }
        wire_->clear_peer(peer, make_error_code(error::peer_disconnected));
        fail_requests(peer, make_error_code(error::peer_disconnected));
        if (left_) {
            return;
        }
        if ((kind == end_kind::kick || kind == end_kind::peer_left) && app_->find(peer)) {
            schedule_rebinding(peer, kind == end_kind::kick);
        }
        signaling_->forget(peer);
        // Announcements find peers without a connection; over a live one every rebinding goes
        // through it.
        if (!app_->find(peer)) {
            strategy_->restart_announcements();
        }
        if (state.active) {
            // As a join, a leave is not reported once the room is left.
            post([weak = weak_from_this(), peer = std::move(peer), user = std::move(user),
                  last_of_user] {
                const auto self = weak.lock();
                if (!self || self->left_) {
                    return;
                }
                self->peer_leave_.emit({.peer_id = peer, .user_id = user});
                if (last_of_user) {
                    self->user_leave_.emit({.user_id = user, .peer_id = peer});
                }
            });
        }
    }

    /** Says the room is left on every live connection of the app, lets every peer go as a
     * departure, and leaves the app: its registry, and once, its users.
     *
     * Tip: the presence goes first, since a departure of a connection's last room ends it at once
     * when the app's idle time is 0. */
    void close_everything() {
        leave_timer_.reset();
        rebinds_.clear();
        const auto connections = app_->live_connections();
        for (const auto& c : connections) {
            c->send_presence(token_, false);
        }
        auto peers = std::move(peers_);
        peers_.clear();
        users_.clear();  // without leaves, as 0.27.0 reports none once its room is leaving
        for (auto& [id, state] : peers) {
            let_go(state, end_kind::room_end);
            for (auto& ping : state.pings) {
                complete(std::move(ping.handler), make_error_code(error::room_left),
                         std::chrono::milliseconds{});
            }
            if (wire_) {
                wire_->clear_peer(id, make_error_code(error::room_left));
            }
        }
        // A log callback may have left this room during adoption, before it had a binding
        // to detach. Its connection still gets an idle deadline after the farewell withdrawal.
        if (const auto c = departing_connection_.lock()) {
            c->idle_if_unused(app_->idle_timeout());
        }
        departing_connection_.reset();
        // Every listener goes, as 0.27.0 clears its emitters and its actions' listeners once its
        // room is left. From a post: an emission under way finishes, as 0.27.0's leave clears only
        // once it completes, and what was posted to the listeners before is delivered, as 0.27.0
        // delivers it at once; a join or a leave posted before checks `left_` itself.
        post([weak = weak_from_this()] {
            if (const auto self = weak.lock()) {
                self->clear_listeners();
            }
        });
        for (auto& [name, slot] : actions_) {
            for (auto& receiver : slot.receivers) {
                complete(std::move(receiver), make_error_code(error::room_left), incoming{});
            }
            slot.receivers.clear();
            // What waited for a receive or a listener goes too, as 0.27.0 drops each peer's
            // pending data when its room is left: a listener added later gets none of it.
            slot.queue.clear();
        }
        fail_requests({}, make_error_code(error::room_left));
        if (signaling_) {
            signaling_->close();
        }
        if (strategy_) {
            strategy_->stop();
        }
        app_->unregister_room(token_, this);
        if (!std::exchange(released_, true)) {
            app_->release_user();
        }
    }

    /** Removes every listener of the room and of its actions: their subscriptions are inactive. */
    void clear_listeners() noexcept {
        peer_join_.clear();
        peer_leave_.clear();
        user_join_.clear();
        user_leave_.clear();
        errors_.clear();
        for (auto& [name, slot] : actions_) {
            slot.messages.clear();
            slot.requests.clear();
            slot.gate.clear();
            slot.progress.clear();
        }
    }

    std::pmr::memory_resource* memory_;
    std::shared_ptr<app_impl> app_;
    executor_type ex_;
    backend::factory& factory_;
    logger log_;
    room_options options_;
    // How long a peer has to finish the handshake: the app's, 10 s.
    std::chrono::milliseconds handshake_timeout_;
    std::pmr::string room_id_;
    std::pmr::string token_;
    std::pmr::string root_topic_;
    std::pmr::string self_topic_;
    std::shared_ptr<action_wire> wire_;
    std::shared_ptr<signaling> signaling_;
    std::shared_ptr<nostr_strategy> strategy_;
    std::pmr::map<std::pmr::string, peer_state, std::less<>> peers_;
    user_table users_;  // the active peers', by user
    std::pmr::map<std::pmr::string, rebind_state, std::less<>> rebinds_;
    std::pmr::map<std::pmr::string, action_slot, std::less<>> actions_;
    std::pmr::map<std::pmr::string, request_waiter, std::less<>> requests_;
    listeners<void(const peer_event&)> peer_join_;
    listeners<void(const peer_event&)> peer_leave_;
    listeners<void(const user_event&)> user_join_;
    listeners<void(const user_event&)> user_leave_;
    listeners<void(const room_error&)> errors_;
    std::shared_ptr<backend::timer> leave_timer_;
    std::weak_ptr<connection> departing_connection_;
    std::pmr::vector<completion<void(boost::system::error_code)>> leave_completions_;
    bool ended_ = false;
    bool left_ = false;
    bool released_ = false;  // the app's user count no longer counts this room
};

}  // namespace detail

template <class T>
class action;

template <class T, class R>
class request_action;

/**
 * A room joined over Trystero's Nostr strategy, what Trystero's `joinRoom` returns.
 *
 * `app::join` makes it, joined; `async_leave` leaves with Trystero's farewell, and destroying it
 * leaves at once. Its peers meet over the relays, connect over WebRTC through the connections its
 * app shares between its rooms, and exchange the messages of the actions it makes. On the branch
 * `port/trystero-0.27` it is not interoperable with Trystero 0.27.0 before Phase 2: it keeps
 * 0.26.0's envelope and handshake, and has no ECDSA P-256.
 *
 * Every room uses its app's immutable password. Initial sealed signaling authenticates that
 * domain before a connection is adopted. The unchanged JS room handshake verifies only at the
 * lower id; different passwords require separate apps with distinct peer ids.
 *
 * Tip: every handler the room calls, and every completion, runs on its app's executor. The views a
 * handler receives are valid for the duration of the call. Every allocation of the room goes
 * through its effective `room_options::memory`, the app's resource when null; shared connections
 * and received frames use `app_config::memory`. A completion handler is allocated as its own
 * associated allocator says. Destroying the room leaves operations pending on its executor, so
 * both resources must outlive those operations and every retained value allocated in them.
 *
 * @see "Rooms and actions", in the guide.
 */
class room {
public:
    /** The executor every handler and completion of the room runs on. */
    using executor_type = webcpp::asio::any_io_executor;

    /** Not copyable: a room is one membership. */
    room(const room&) = delete;

    /** Not copy-assignable: a room is one membership. */
    room& operator=(const room&) = delete;

    /**
     * Takes the membership of the room moved from, which may then only be destroyed or assigned.
     */
    room(room&&) noexcept = default;

    // Leaving sends each peer Trystero's farewell, whose frames are allocated in the room's
    // resource: a resource that fails then ends the program, as in the destructor.
    /**
     * Leaves the membership this room held at once, as the destructor does, then takes the
     * membership of the room moved from, which may then only be destroyed or assigned.
     *
     * Every peer of the membership left is told, the room lets go of the app's connections, which
     * the app's other rooms may go on using, and every operation pending on it completes with
     * `error::room_left`, even while an `action` of it lives.
     *
     * @param other The room moved from.
     * @return This room.
     */
    room& operator=(room&& other) noexcept {  // NOLINT(bugprone-exception-escape)
        if (this != &other) {
            if (impl_) {
                impl_->abandon();
            }
            impl_ = std::move(other.impl_);
        }
        return *this;
    }

    // Leaving sends each peer Trystero's farewell, whose frames are allocated in the room's
    // resource: a resource that fails then ends the program, as in any destructor.
    /**
     * Leaves at once, without the farewell's grace period: every peer is told, the room lets go of
     * the app's connections, which the app's other rooms may go on using, and every pending
     * operation completes with `error::room_left`.
     */
    ~room() {  // NOLINT(bugprone-exception-escape)
        if (impl_) {
            impl_->abandon();
        }
    }

    /**
     * The executor of the room's app.
     *
     * @return The executor every handler and completion of the room runs on.
     */
    [[nodiscard]] const executor_type& get_executor() const { return impl_->get_executor(); }

    /**
     * This peer's id in the room, Trystero's `selfId`: its app's.
     *
     * @return The id, valid as long as the room.
     */
    [[nodiscard]] std::string_view self_id() const { return impl_->self_id(); }

    /**
     * The Nostr relays this room uses, its app's: derived from the app id unless the app's
     * configuration named them.
     *
     * @return The relays' URLs, valid as long as the room.
     */
    [[nodiscard]] std::span<const std::pmr::string> relay_urls() const {
        return impl_->relay_urls();
    }

    /**
     * This peer in the room, Trystero's `getSelf`, at once: the app's identity exists from its
     * construction on.
     *
     * @return This peer's and its user's ids, the room's ids and roles, whose views are valid as
     * long as the room.
     */
    [[nodiscard]] self_info self() const { return impl_->self(); }

    /**
     * The users with a peer in the room, Trystero's `getUsers`: this peer's own user is left out.
     *
     * @return Each user by id, with its active peers in the order they became active, in the
     * room's resource.
     */
    [[nodiscard]] std::pmr::map<std::pmr::string, user_info, std::less<>> users() const {
        return impl_->users();
    }

    /**
     * The peers whose handshake completed, Trystero's `getPeers` without their connections.
     *
     * @return Each peer by id, with its user, in the room's resource.
     */
    [[nodiscard]] std::pmr::map<std::pmr::string, peer_info, std::less<>> peers() const {
        return impl_->peers();
    }

    /**
     * Adds a listener of each peer that joins, once its handshake completes, Trystero's
     * `onPeerJoin`.
     *
     * Listeners run in the order added, each join reported from the room's executor; one added
     * while a join is reported waits for the next, and one removed before its turn does not run.
     *
     * @tparam F A callable with a `const peer_event&`, whose views are valid for the call.
     * @param f The listener, kept in the room's resource beside those added before.
     * @param options `once` removes it before its first run; `replay` calls it first, within this
     * call, with each peer already joined.
     * @return Its subscription, which requires the room's resource to outlive it; destroying it
     * leaves the listener.
     */
    template <class F>
        requires std::is_invocable_v<F&, const peer_event&>
    subscription on_peer_join(F&& f, listen_options options = {}) {
        const auto impl = impl_;  // the replay may destroy or reassign this handle
        return impl->on_peer_join(impl->bind<void(const peer_event&)>(std::forward<F>(f)), options);
    }

    /**
     * Adds a listener of each joined peer that leaves, Trystero's `onPeerLeave`.
     *
     * @tparam F A callable with a `const peer_event&`, whose views are valid for the call.
     * @param f The listener, kept in the room's resource beside those added before.
     * @param options `once` removes it before its first run; `replay` is ignored.
     * @return Its subscription, which requires the room's resource to outlive it; destroying it
     * leaves the listener.
     */
    template <class F>
        requires std::is_invocable_v<F&, const peer_event&>
    subscription on_peer_leave(F&& f, listen_options options = {}) {
        return impl_->on_peer_leave(impl_->bind<void(const peer_event&)>(std::forward<F>(f)),
                                    options);
    }

    /**
     * Adds a listener of each user that joins, when its first peer joins, after that peer's
     * `on_peer_join`, Trystero's `onUserJoin`; a peer of this peer's own user makes no user join.
     *
     * Until identities are derived from keys, a peer's user is the peer's own id.
     *
     * @tparam F A callable with a `const user_event&`, whose views are valid for the call.
     * @param f The listener, kept in the room's resource beside those added before.
     * @param options `once` removes it before its first run; `replay` calls it first, within this
     * call, with each user already joined.
     * @return Its subscription, which requires the room's resource to outlive it; destroying it
     * leaves the listener.
     */
    template <class F>
        requires std::is_invocable_v<F&, const user_event&>
    subscription on_user_join(F&& f, listen_options options = {}) {
        const auto impl = impl_;  // the replay may destroy or reassign this handle
        return impl->on_user_join(impl->bind<void(const user_event&)>(std::forward<F>(f)), options);
    }

    /**
     * Adds a listener of each user that leaves, when its last peer leaves, after that peer's
     * `on_peer_leave`, Trystero's `onUserLeave`; this peer's own user never leaves.
     *
     * @tparam F A callable with a `const user_event&`, whose views are valid for the call.
     * @param f The listener, kept in the room's resource beside those added before.
     * @param options `once` removes it before its first run; `replay` is ignored.
     * @return Its subscription, which requires the room's resource to outlive it; destroying it
     * leaves the listener.
     */
    template <class F>
        requires std::is_invocable_v<F&, const user_event&>
    subscription on_user_leave(F&& f, listen_options options = {}) {
        return impl_->on_user_leave(impl_->bind<void(const user_event&)>(std::forward<F>(f)),
                                    options);
    }

    /**
     * Adds a listener of each problem the room handled itself, Trystero's `onError`: each report
     * is a @ref room_error, with its code, its message, the app's and the room's ids, and the
     * context Trystero gives that code.
     *
     * Nothing is reported once the room is being left.
     *
     * | Code                  | When                                                       |
     * | `decrypt_failed`      | a peer's offer or answer did not open with the room key:   |
     * |                       | another password, or a corrupted or forged signal; at most |
     * |                       | once a minute per peer and stage, as Trystero reports it   |
     * | `peer_connect_failed` | the connection with a peer did not open after the exchange |
     * |                       | of descriptions; once per offer or answer                  |
     * | `relay_failed`        | the first connection of every relay failed before any      |
     * |                       | opened; once                                               |
     * | `wrong_password`      | a peer sharing a connection failed the room's password     |
     * |                       | handshake                                                  |
     * | `handshake_timed_out` | a peer sharing a connection did not finish it in time      |
     * | `backend_failure`     | the backend made no connection for a peer, or the room's   |
     * |                       | keys could not be derived                                  |
     *
     * A part of the configuration the app set aside is reported to `app::on_error`, not here.
     *
     * @tparam F A callable with a `const room_error&`, whose views are valid for the call.
     * @param f The listener, kept in the room's resource beside those added before.
     * @param options `once` removes it before its first run; `replay` is ignored.
     * @return Its subscription, which requires the room's resource to outlive it; destroying it
     * leaves the listener.
     */
    template <class F>
        requires std::is_invocable_v<F&, const room_error&>
    subscription on_error(F&& f, listen_options options = {}) {
        return impl_->on_error(impl_->bind<void(const room_error&)>(std::forward<F>(f)), options);
    }

    /**
     * Makes an action of this room, Trystero's `makeAction`: a named channel of messages.
     *
     * Frames of the action that arrived before it was made are dropped, and its transfers offered
     * meanwhile rejected, as Trystero's are: a room makes the actions it receives right after it
     * is joined. A name is made again as often as wanted, each handle the same action; a name made
     * first by `make_request_action` cannot be made a message action, where Trystero throws "action
     * type cannot be redefined": the handle's sends and receives complete with
     * `error::action_redefined`, its `on_receive` returns it, and its other listeners are not
     * added.
     *
     * @tparam T The type of its messages: `std::string`, `std::pmr::string`, `std::string_view`,
     * `webcpp::document::document`, `webcpp::document::pmr::document`, @ref bytes, @ref pmr_bytes,
     * `std::span<const std::uint8_t>`, or @ref payload for any of Trystero's formats.
     * @param name The action's name, 1 to 32 UTF-8 bytes without NUL; with another, every send
     * fails with `error::invalid_action_name`.
     * @return The action.
     */
    template <class T>
    action<T> make_action(std::string_view name);

    /**
     * Makes a request action of this room, Trystero's `makeAction` with `kind: 'request'`: a named
     * channel of requests, each answered by the peer asked.
     *
     * A request travels as a message of the action whose metadata is `{"r":<id>,"m":<metadata>}`,
     * and its answer as the internal action `\@_response`, as 0.27.0's do. A name is a request
     * action or a message action, not both: a name made first by `make_action` cannot be made a
     * request action, where Trystero throws "action type cannot be redefined": the handle's
     * requests complete with `error::action_redefined`, its `on_request` and `on_receive` return
     * it, and its progress listeners are not added.
     *
     * @tparam T The type of its requests, one of those `make_action` lists.
     * @tparam R The type of its answers, one of them that owns its storage.
     * @param name The action's name, 1 to 32 UTF-8 bytes without NUL; with another, every request
     * fails with `error::invalid_action_name`.
     * @return The action.
     */
    template <class T, class R = T>
    request_action<T, R> make_request_action(std::string_view name);

    /**
     * Measures the round trip to a peer, Trystero's `ping`.
     *
     * Completes with `(error_code, std::chrono::milliseconds)`: the time from sending `\@_ping`
     * to receiving its `\@_pong`, or `error::no_such_peer` when the peer is not active,
     * `error::peer_disconnected` when it leaves first, and `error::room_left`.
     *
     * @tparam CompletionToken An Asio completion token for `void(error_code, milliseconds)`.
     * @param peer The peer's id, copied at the call, so a deferred operation does not depend on
     * the caller's string.
     * @param token The completion token.
     * @return What the token's initiation returns.
     */
    template <class CompletionToken>
    auto async_ping(std::string_view peer, CompletionToken&& token) {
        return webcpp::asio::async_initiate<CompletionToken, void(boost::system::error_code,
                                                                  std::chrono::milliseconds)>(
            [impl = impl_](auto handler, const peer_id& peer) {
                impl->ping(peer, impl->template erase<void(boost::system::error_code,
                                                           std::chrono::milliseconds)>(
                                     std::move(handler)));
            },
            token, peer_id(peer, impl_->memory()));
    }

    /**
     * Says goodbye to every peer and closes everything, Trystero's `leave`.
     *
     * Each peer is sent `\@_leave`, and 99 ms later, as Trystero waits, the room lets go of the
     * app's connections, which the app's other rooms may go on using, and every pending operation
     * completes with `error::room_left`. Completes with `(error_code)`, empty, also when the room
     * was already left.
     *
     * @tparam CompletionToken An Asio completion token for `void(error_code)`.
     * @param token The completion token.
     * @return What the token's initiation returns.
     */
    template <class CompletionToken>
    auto async_leave(CompletionToken&& token) {
        return webcpp::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
            [impl = impl_](auto handler) {
                impl->leave(
                    impl->template erase<void(boost::system::error_code)>(std::move(handler)));
            },
            token);
    }

private:
    friend class app;

    /** The handle of a room `app::join` made and started. */
    explicit room(std::shared_ptr<detail::room_impl> impl) : impl_(std::move(impl)) {}

    std::shared_ptr<detail::room_impl> impl_;
};

/**
 * A named channel of messages of type `T` within a room, what Trystero's `makeAction` returns
 * (not interoperable before Phase 2, as `room` says).
 *
 * Messages arrive at the listeners `on_message` adds, or, without one, at `async_receive`. Until
 * the action has a listener or was first awaited, its messages wait as Trystero's wait for a
 * listener, 32 per sender for 10 s, the oldest dropped first; once awaited, those no receive
 * waits for queue for the next, up to 1024, the oldest dropped first. A message
 * of another format than `T`'s is not delivered to a listener. A room makes it with
 * `room::make_action`, and it keeps the room's state alive.
 *
 * @tparam T The type of its messages, one of those `room::make_action` lists.
 * @see "Rooms and actions", in the guide.
 */
template <class T>
class action {
public:
    /**
     * The action's name.
     *
     * @return The name, valid as long as the action.
     */
    [[nodiscard]] std::string_view name() const { return name_; }

    /**
     * Adds a listener of every message, Trystero's `onMessage`.
     *
     * Listeners run in the order added, each message reported from the room's executor; one added
     * while a message is reported waits for the next, and one removed before its turn does not
     * run. Messages that queued for `async_receive`, and those that waited for a listener, are
     * delivered first. Each listener decodes the message: with several, each takes its own copy.
     *
     * @tparam F A callable with a `T` and a `const message_info&`; with a view type, the view is
     * valid for the duration of the call. A JSON document is the listener's to keep: a handle of
     * its values refers to the document object, as every handle of webcpp::document does, so moving
     * the document invalidates it.
     * @param f The listener, kept in the room's resource beside those added before.
     * @param options `once` removes it before its first run; `replay` is ignored.
     * @return Its subscription, which requires the room's resource to outlive it; destroying it
     * leaves the listener. It is inactive, and nothing is added, when the name was made a request
     * action first.
     */
    template <class F>
        requires std::is_invocable_v<F&, T, const message_info&>
    subscription on_message(F&& f, listen_options options = {}) {
        if (redefined_) {
            return {};
        }
        // Held for the call: delivering what waited may run a receive gate, which may release the
        // room's last handles, this one among them.
        const auto impl = impl_;
        return impl->add_message_listener(
            name_,
            impl->template bind<void(detail::incoming&)>(
                [f = std::forward<F>(f), to = impl->decoding()](detail::incoming& m) mutable {
                    // The metadata is read before decoding: a binary payload takes the buffer the
                    // metadata lives in.
                    const message_info info{
                        .from = m.shared ? std::pmr::string(m.peer, to.memory) : std::move(m.peer),
                        .metadata = std::pmr::string(m.metadata(), to.memory),
                    };
                    if (auto value = detail::payload_traits<T>::decode(m, to)) {
                        f(std::move(*value), info);
                    }
                }),
            options);
    }

    /**
     * Sets the action's receive gate, Trystero's `onReceive`: asked about each message as it
     * arrives, before the action receives it, with its size and metadata.
     *
     * It runs during the frame's processing, as Trystero's runs, rather than from a post: its
     * answer decides at once. A refused inline message is dropped; a refused transfer is rejected
     * with "payload rejected by receiver", and its sender's send completes with
     * `error::transfer_rejected`. A message that waited for a listener before the gate was set is
     * asked before it is delivered.
     *
     * @tparam F A callable with a `const receive_info&`, whose views are valid for the call,
     * returning whether to receive the message.
     * @param f The gate, kept in the room's resource.
     * @param options `once` removes it before its first run; `replay` is ignored.
     * @return Its subscription, which requires the room's resource to outlive it;
     * `error::already_subscribed` while a gate is in place, or `error::action_redefined` when the
     * name was made the other kind of action first.
     */
    template <class F>
        requires std::is_invocable_r_v<bool, F&, const receive_info&>
    [[nodiscard]] boost::system::result<subscription> on_receive(F&& f,
                                                                 listen_options options = {}) {
        if (redefined_) {
            return error::action_redefined;
        }
        const auto impl = impl_;  // delivering what waited may release this handle
        return impl->add_receive_gate(
            name_, impl->template bind<bool(const receive_info&)>(std::forward<F>(f)), options);
    }

    /**
     * Adds a listener of the progress of each message received, Trystero's `onReceiveProgress`:
     * the share of a transfer received after each chunk but the last, then 1 once the message is
     * whole, reported from the room's executor.
     *
     * @tparam F A callable with a `double` from 0 to 1 and a `const progress_info&`, whose views
     * are valid for the call.
     * @param f The listener, kept in the room's resource beside those added before.
     * @param options `once` removes it before its first run; `replay` is ignored.
     * @return Its subscription, which requires the room's resource to outlive it; destroying it
     * leaves the listener. It is inactive, and nothing is added, when the name was made the other
     * kind of action first.
     */
    template <class F>
        requires std::is_invocable_v<F&, double, const progress_info&>
    subscription on_receive_progress(F&& f, listen_options options = {}) {
        if (redefined_) {
            return {};
        }
        return impl_->add_progress_listener(
            name_, impl_->bind<void(double, const progress_info&)>(std::forward<F>(f)), options);
    }

    // T is the action's type, taken by value and moved into its encoding, which keeps a value
    // that owns its storage; a JSON document is written there at once, from a reference.
    /**
     * Sends a message to `options.targets`, each a peer or a user, or to every active peer,
     * Trystero's `send`; to a room without peers, it succeeds.
     *
     * A payload that fits one frame is sent inline; a larger one is offered, accepted by each
     * receiver, and sent in chunks. Completes with `(error_code)` once each target's local data
     * channel has taken the inline frame, or the last chunk of a transfer the receiver accepted,
     * as Trystero's `send` resolves: nothing confirms that the receiver has the message. It
     * completes with the first failure: `error::invalid_action_name`, `error::metadata_too_large`,
     * `error::not_connected` for a target that is neither an active peer nor a user of one,
     * `error::transfer_rejected`, `error::transfer_timed_out`,
     * `error::channel_stalled`, `error::peer_disconnected` or `error::room_left`; at once with
     * `error::action_redefined` when the name was made a request action first.
     *
     * @tparam CompletionToken An Asio completion token for `void(error_code)`.
     * @param value The message, taken at the call: moved when `T` owns its storage, copied once
     * into the room's resource when it is a view, and serialized at once into the room's resource
     * when it is a JSON document, or a @ref payload that holds one.
     * @param options The targets and the metadata.
     * @param token The completion token.
     * @return What the token's initiation returns.
     */
    template <class CompletionToken>
    auto async_send(T value, send_options options,  // NOLINT(performance-unnecessary-value-param)
                    CompletionToken&& token) {
        return webcpp::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
            [impl = impl_, name = std::pmr::string(name_, impl_->memory()), redefined = redefined_](
                auto handler, detail::encoded_payload payload, const send_options& options) {
                if (redefined) {
                    impl->complete(std::move(handler), make_error_code(error::action_redefined));
                    return;
                }
                impl->send(
                    name, std::move(payload), options,
                    impl->template erase<void(boost::system::error_code)>(std::move(handler)));
            },
            token, detail::payload_traits<T>::encode(std::move(value), impl_->memory()),
            std::move(options));
    }

    /**
     * Sends a message to every active peer, without metadata.
     *
     * @tparam CompletionToken An Asio completion token for `void(error_code)`.
     * @param value The message, taken at the call as the other overload takes it.
     * @param token The completion token.
     * @return What the token's initiation returns.
     */
    template <class CompletionToken>
    auto async_send(T value, CompletionToken&& token) {
        return async_send(std::move(value), send_options(), std::forward<CompletionToken>(token));
    }

    /**
     * Receives the next message, when no listener is in place: the oldest queued, or the next to
     * arrive.
     *
     * From the first call on, the action listens: its messages queue for the next.
     *
     * Completes with `(error_code, T, message_info)`; a message of another payload format than
     * `T`'s, or a JSON text that does not parse, completes with `error::transfer_rejected` and an
     * empty value, which allocates nothing (a JSON document that holds nothing, whose root reads
     * `undefined`), leaving the room with `error::room_left`, and at once with
     * `error::action_redefined` when the name was made a request action first.
     * `T` must own its storage: a view would outlive the message. A JSON document is the
     * handler's: a handle of its values refers to the document object, so moving the document
     * invalidates it.
     *
     * @tparam CompletionToken An Asio completion token for `void(error_code, T, message_info)`.
     * @param token The completion token.
     * @return What the token's initiation returns.
     */
    template <class CompletionToken>
    auto async_receive(CompletionToken&& token) {
        static_assert(detail::payload_traits<T>::owning,
                      "a received view would outlive the message: await an owning type");
        return webcpp::asio::async_initiate<CompletionToken,
                                            void(boost::system::error_code, T, message_info)>(
            [impl = impl_, name = std::pmr::string(name_, impl_->memory()),
             redefined = redefined_](auto handler) {
                if (redefined) {
                    impl->complete(std::move(handler), make_error_code(error::action_redefined),
                                   detail::empty_payload<T>(impl->memory()),
                                   message_info{
                                       .from = std::pmr::string(impl->memory()),
                                       .metadata = std::pmr::string(impl->memory()),
                                   });
                    return;
                }
                // The decoding step is allocated as the user's handler is, which it carries.
                const auto allocator = webcpp::asio::get_associated_allocator(handler);
                impl->receive(
                    name, impl->template erase<void(boost::system::error_code, detail::incoming)>(
                              webcpp::asio::bind_allocator(
                                  allocator,
                                  [impl, handler = std::move(handler)](boost::system::error_code ec,
                                                                       detail::incoming m) mutable {
                                      const detail::decoding to = impl->decoding();
                                      message_info info{
                                          .from = std::move(m.peer),
                                          .metadata = std::pmr::string(m.metadata(), to.memory),
                                      };
                                      auto value = ec ? std::nullopt
                                                      : detail::payload_traits<T>::decode(m, to);
                                      if (!ec && !value) {
                                          ec = make_error_code(error::transfer_rejected);
                                      }
                                      impl->complete(std::move(handler), ec,
                                                     value ? std::move(*value)
                                                           : detail::empty_payload<T>(to.memory),
                                                     std::move(info));
                                  })));
            },
            token);
    }

private:
    friend class room;

    action(std::shared_ptr<detail::room_impl> impl, std::string_view name)
        : impl_(std::move(impl)),
          name_(name, impl_->memory()),
          redefined_(!impl_->add_action(name_, false)) {}

    std::shared_ptr<detail::room_impl> impl_;
    std::pmr::string name_;
    /** Whether the name was made a request action first: every operation fails. */
    bool redefined_;
};

/**
 * A named channel of requests of type `T`, each answered with an `R` by the peer asked, within a
 * room, what Trystero's `makeAction` returns for `kind: 'request'` (not interoperable before
 * Phase 2, as `room` says).
 *
 * The peer asked answers with the listener `on_request` sets. While it has none, requests wait as
 * Trystero's messages wait for a listener, 32 per sender for 10 s. A room makes it with
 * `room::make_request_action`, and it keeps the room's state alive.
 *
 * @tparam T The type of its requests, one of those `room::make_action` lists.
 * @tparam R The type of its answers, one of those that own their storage.
 * @see "Rooms and actions", in the guide.
 */
template <class T, class R>
class request_action {
public:
    static_assert(detail::payload_traits<R>::owning,
                  "an answer completes a request later: answer with an owning type");

    /**
     * The action's name.
     *
     * @return The name, valid as long as the action.
     */
    [[nodiscard]] std::string_view name() const { return name_; }

    /**
     * Sets the listener that answers each request, Trystero's `onRequest`.
     *
     * It is called from the room's executor with the request and who sent it, and its result is
     * the answer: an `R`, or a `boost::system::result<R>` whose error refuses the request with the
     * error's message, which the requester's request completes with as `error::request_rejected`.
     * A request of another format than `T`'s is refused.
     *
     * @tparam F A callable with a `T` and a `const message_info&`, whose metadata is the sender's
     * own, returning an `R` or a `boost::system::result<R>`; with a view type, the view is valid
     * for the duration of the call.
     * @param f The listener, kept in the room's resource.
     * @param options `once` removes it before its first run; `replay` is ignored.
     * @return Its subscription, which requires the room's resource to outlive it;
     * `error::already_subscribed` while a listener is in place, or `error::action_redefined` when
     * the name was made a message action first.
     */
    template <class F>
        requires std::is_invocable_v<F&, T, const message_info&>
    [[nodiscard]] boost::system::result<subscription> on_request(F&& f,
                                                                 listen_options options = {}) {
        using answer = std::invoke_result_t<F&, T, const message_info&>;
        static_assert(
            std::is_convertible_v<answer, R> || std::is_same_v<answer, boost::system::result<R>>,
            "a request listener returns an R or a boost::system::result<R>");
        if (redefined_) {
            return error::action_redefined;
        }
        // Held for the call: delivering what waited may run a receive gate, which may release the
        // room's last handles, this one among them.
        const auto impl = impl_;
        // The listener is the room's own: it reaches its room through a plain pointer, since a
        // shared one would keep the room alive through itself.
        detail::room_impl* const room = impl.get();
        return impl->add_request_listener(
            name_,
            impl->template bind<void(detail::incoming&, const detail::request_envelope&)>(
                [f = std::forward<F>(f), room, to = impl->decoding()](
                    detail::incoming& m, const detail::request_envelope& envelope) mutable {
                    const message_info info{
                        .from = std::pmr::string(m.peer, to.memory),
                        .metadata = std::pmr::string(envelope.metadata, to.memory),
                    };
                    auto value = detail::payload_traits<T>::decode(m, to);
                    if (!value) {
                        room->respond_error(info.from, envelope.id,
                                            make_error_code(error::transfer_rejected).message());
                        return;
                    }
                    if constexpr (std::is_same_v<answer, boost::system::result<R>>) {
                        auto r = f(std::move(*value), info);
                        if (!r) {
                            room->respond_error(info.from, envelope.id, r.error().message());
                            return;
                        }
                        room->respond(info.from, envelope.id,
                                      detail::payload_traits<R>::encode(std::move(*r), to.memory));
                    } else {
                        room->respond(info.from, envelope.id,
                                      detail::payload_traits<R>::encode(f(std::move(*value), info),
                                                                        to.memory));
                    }
                }),
            options);
    }

    /**
     * Sets the action's receive gate, Trystero's `onReceive`: asked about each request as it
     * arrives, as `action::on_receive` asks.
     *
     * A refused request is answered with "payload rejected by receiver".
     *
     * @tparam F A callable with a `const receive_info&`, whose metadata is the sender's own,
     * returning whether to receive the request.
     * @param f The gate, kept in the room's resource.
     * @param options `once` removes it before its first run; `replay` is ignored.
     * @return Its subscription, which requires the room's resource to outlive it;
     * `error::already_subscribed` while a gate is in place, or `error::action_redefined` when the
     * name was made the other kind of action first.
     */
    template <class F>
        requires std::is_invocable_r_v<bool, F&, const receive_info&>
    [[nodiscard]] boost::system::result<subscription> on_receive(F&& f,
                                                                 listen_options options = {}) {
        if (redefined_) {
            return error::action_redefined;
        }
        const auto impl = impl_;  // delivering what waited may release this handle
        return impl->add_receive_gate(
            name_, impl->template bind<bool(const receive_info&)>(std::forward<F>(f)), options);
    }

    /**
     * Adds a listener of the progress of each request received, Trystero's `onReceiveProgress`,
     * as `action::on_receive_progress` reports it.
     *
     * @tparam F A callable with a `double` from 0 to 1 and a `const progress_info&`.
     * @param f The listener, kept in the room's resource beside those added before.
     * @param options `once` removes it before its first run; `replay` is ignored.
     * @return Its subscription, which requires the room's resource to outlive it; destroying it
     * leaves the listener. It is inactive, and nothing is added, when the name was made the other
     * kind of action first.
     */
    template <class F>
        requires std::is_invocable_v<F&, double, const progress_info&>
    subscription on_receive_progress(F&& f, listen_options options = {}) {
        if (redefined_) {
            return {};
        }
        return impl_->add_progress_listener(
            name_, impl_->bind<void(double, const progress_info&)>(std::forward<F>(f)), options);
    }

    // T is the action's type, taken by value and moved into its encoding, as `action::async_send`
    // takes it.
    /**
     * Asks one peer, or one peer of a user, Trystero's `request`.
     *
     * Completes with `(error_code, R)`: the answer, as Trystero's promise resolves with it; or
     * `error::not_connected` when the target is neither an active peer nor a user of one,
     * `error::request_rejected` when the peer refused the request (its listener's error, no
     * listener, or a refused transfer), `error::request_timed_out` after `options.timeout`,
     * `error::transfer_rejected` for an answer of another format than `R`'s,
     * `error::peer_disconnected` when the peer leaves first, `error::room_left`, the failure of
     * the request's send, or at once `error::action_redefined` when the name was made a message
     * action first. An unsuccessful completion carries an empty answer.
     *
     * Unlike Trystero's rejection, which carries the reason the peer gave, a refusal completes with
     * the code alone: the room logs the reason, under "action".
     *
     * @tparam CompletionToken An Asio completion token for `void(error_code, R)`.
     * @param value The request, taken at the call as `action::async_send` takes a message.
     * @param options The peer asked, the metadata, and the timeout.
     * @param token The completion token.
     * @return What the token's initiation returns.
     */
    template <class CompletionToken>
    auto async_request(T value,
                       request_options options,  // NOLINT(performance-unnecessary-value-param)
                       CompletionToken&& token) {
        return webcpp::asio::async_initiate<CompletionToken, void(boost::system::error_code, R)>(
            [impl = impl_, name = std::pmr::string(name_, impl_->memory()), redefined = redefined_](
                auto handler, detail::encoded_payload payload, const request_options& options) {
                if (redefined) {
                    impl->complete(std::move(handler), make_error_code(error::action_redefined),
                                   detail::empty_payload<R>(impl->memory()));
                    return;
                }
                // The decoding step is allocated as the user's handler is, which it carries.
                const auto allocator = webcpp::asio::get_associated_allocator(handler);
                impl->request(
                    name, std::move(payload), options,
                    impl->template erase<void(boost::system::error_code, detail::incoming)>(
                        webcpp::asio::bind_allocator(
                            allocator,
                            [impl, handler = std::move(handler)](boost::system::error_code ec,
                                                                 detail::incoming m) mutable {
                                const detail::decoding to = impl->decoding();
                                auto answer =
                                    ec ? std::nullopt : detail::payload_traits<R>::decode(m, to);
                                if (!ec && !answer) {
                                    ec = make_error_code(error::transfer_rejected);
                                }
                                impl->complete(std::move(handler), ec,
                                               answer ? std::move(*answer)
                                                      : detail::empty_payload<R>(to.memory));
                            })));
            },
            token, detail::payload_traits<T>::encode(std::move(value), impl_->memory()),
            std::move(options));
    }

private:
    friend class room;

    request_action(std::shared_ptr<detail::room_impl> impl, std::string_view name)
        : impl_(std::move(impl)),
          name_(name, impl_->memory()),
          redefined_(!impl_->add_action(name_, true)) {}

    std::shared_ptr<detail::room_impl> impl_;
    std::pmr::string name_;
    /** Whether the name was made a message action first: every operation fails. */
    bool redefined_;
};

template <class T>
action<T> room::make_action(std::string_view name) {
    return action<T>(impl_, name);
}

template <class T, class R>
request_action<T, R> room::make_request_action(std::string_view name) {
    return request_action<T, R>(impl_, name);
}

}  // namespace webcpp::trystero

#endif
