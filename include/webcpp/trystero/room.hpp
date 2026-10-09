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
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/detail/nostr_strategy.hpp>
#include <webcpp/trystero/detail/peer_link.hpp>
#include <webcpp/trystero/detail/post.hpp>
#include <webcpp/trystero/detail/signaling.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/json_resource.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <boost/asio/any_completion_handler.hpp>  // lint-world: posts handlers only
#include <boost/asio/any_io_executor.hpp>         // lint-world: posts handlers only
#include <boost/asio/append.hpp>                  // lint-world: posts handlers only
#include <boost/asio/associated_allocator.hpp>    // lint-world: posts handlers only
#include <boost/asio/associated_executor.hpp>     // lint-world: posts handlers only
#include <boost/asio/async_result.hpp>            // lint-world: posts handlers only
#include <boost/asio/bind_allocator.hpp>          // lint-world: posts handlers only
#include <boost/assert.hpp>
#include <boost/json/storage_ptr.hpp>
#include <boost/json/value.hpp>

#include <array>
#include <chrono>
#include <deque>
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

using peer_id = std::pmr::string;
using bytes = std::vector<std::uint8_t>;
/** Binary data in a memory resource: a room decodes a binary payload into its own resource without
 * a copy. */
using pmr_bytes = std::pmr::vector<std::uint8_t>;

/** What an action carries when its type is not fixed: Trystero's text, JSON or binary payload. */
using payload = std::variant<std::string, boost::json::value, bytes>;

/** What a room is joined with; the room copies every part of it into `memory`. */
struct room_config {
    std::pmr::string app_id = {};
    /** Seals signals and adds a challenge to the handshake; peers with another password never join.
     */
    std::pmr::string password = {};
    /** Nostr relays to use; empty means Trystero's defaults, derived from the app id. */
    std::pmr::vector<std::pmr::string> relay_urls = {};
    std::size_t relay_redundancy = 5;
    /** STUN and TURN servers; empty means Trystero's defaults. */
    std::pmr::vector<backend::ice_server> ice_servers = {};
    /** This peer's id in the room; empty means 20 random characters. */
    std::pmr::string self_id = {};
    std::chrono::milliseconds handshake_timeout{10'000};
    std::size_t max_receive_bytes = std::size_t{256} * 1024 * 1024;
    /** Diagnostics, by category; see detail::logger. The room moves the sink into `memory`. */
    detail::logger::sink log = {};
    /**
     * The resource every allocation of the room goes through, from its construction to its
     * destruction; not null.
     *
     * It must outlive the room, every value the room hands out (a `std::pmr::string`, a
     * `pmr_bytes`, a `boost::json::value`), and every operation the room leaves pending: on Asio,
     * until the io_context the room's executor belongs to has run out of the room's work or has
     * been destroyed, so a resource is declared before its io_context; in the browser, until the
     * room's pending promises have settled. A room allocates and frees per event, so a resource
     * that reuses what is freed (a pool) suits a long-lived room, and a monotonic one only a
     * short-lived one.
     */
    std::pmr::memory_resource* memory = std::pmr::get_default_resource();
};

/** Who sent a message, and the metadata it carried (JSON text, or empty), in the receiving room's
 * resource. */
struct message_info {
    peer_id from;
    std::pmr::string metadata;
};

/**
 * Where an action goes: every active peer when `targets` is empty. `metadata` is JSON text, or
 * empty. Tip: it owns its strings because a completion token such as `asio::deferred` starts the
 * operation later, after the caller's views may be gone.
 */
struct send_options {
    std::pmr::vector<peer_id> targets = {};
    std::pmr::string metadata = {};
};

/** Trystero's default STUN servers. */
inline std::pmr::vector<backend::ice_server> default_ice_servers(
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::vector<backend::ice_server> out(memory);
    for (const std::string_view url : {
             "stun:stun.l.google.com:19302",
             "stun:stun1.l.google.com:19302",
             "stun:stun2.l.google.com:19302",
             "stun:stun.cloudflare.com:3478",
         }) {
        backend::ice_server server{
            .urls = std::pmr::vector<std::pmr::string>(memory),
            .username = std::pmr::string(memory),
            .credential = std::pmr::string(memory),
        };
        server.urls.emplace_back(url);
        out.push_back(std::move(server));
    }
    return out;
}

namespace detail {

/** A payload ready to send: its format, and its bytes kept alive by what owns them. */
struct encoded_payload {
    protocol::payload_format format = protocol::payload_format::text;
    payload_ref ref;
};

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

inline std::string_view as_text(std::span<const std::uint8_t> data) {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

/**
 * How an action's type travels. `encode` takes the value at the call and keeps it alive without
 * copying it when the type owns its storage; a view is copied once, there, into the room's
 * resource. `decode` reads a received message: a view type refers to the message's buffer and is
 * valid for the duration of the handler, so only owning types can be awaited. The `std::pmr` types
 * are built in the receiving room's resource, and JSON in its JSON storage; the standard types
 * allocate with `std::allocator`, as the user chose.
 *
 * | Type                          | Trystero format | Receiving                                   |
 * | std::string                   | text            | a copy of the bytes                         |
 * | std::pmr::string              | text            | a copy of the bytes, in the room's resource |
 * | std::string_view              | text            | a view of the frame, in a handler           |
 * | boost::json::value            | JSON            | parsed, in the room's JSON storage          |
 * | trystero::bytes               | binary          | a copy of the bytes                         |
 * | trystero::pmr_bytes           | binary          | the transfer's buffer, moved, in the room's |
 * |                               |                 | resource                                    |
 * | std::span<const std::uint8_t> | binary          | a view of the frame, in a handler           |
 * | trystero::payload             | any             | by format                                   |
 */
template <class T>
struct payload_traits;

template <>
struct payload_traits<std::string> {
    static constexpr bool owning = true;

    static encoded_payload encode(std::string v, std::pmr::memory_resource* memory) {
        return own(protocol::payload_format::text, keep<std::string>(memory, std::move(v)));
    }

    static std::optional<std::string> decode(incoming& m, std::pmr::memory_resource* /*memory*/) {
        if (m.format != protocol::payload_format::text) {
            return std::nullopt;
        }
        return std::string(as_text(m.payload()));
    }
};

template <>
struct payload_traits<std::pmr::string> {
    static constexpr bool owning = true;

    static encoded_payload encode(std::pmr::string v, std::pmr::memory_resource* memory) {
        return own(protocol::payload_format::text, keep<std::pmr::string>(memory, std::move(v)));
    }

    static std::optional<std::pmr::string> decode(incoming& m, std::pmr::memory_resource* memory) {
        if (m.format != protocol::payload_format::text) {
            return std::nullopt;
        }
        return std::pmr::string(as_text(m.payload()), memory);
    }
};

template <>
struct payload_traits<std::string_view> {
    static constexpr bool owning = false;

    static encoded_payload encode(std::string_view v, std::pmr::memory_resource* memory) {
        return own(protocol::payload_format::text, keep<std::pmr::string>(memory, v));
    }

    static std::optional<std::string_view> decode(incoming& m,
                                                  std::pmr::memory_resource* /*memory*/) {
        if (m.format != protocol::payload_format::text) {
            return std::nullopt;
        }
        return as_text(m.payload());
    }
};

template <>
struct payload_traits<boost::json::value> {
    static constexpr bool owning = true;

    static encoded_payload encode(const boost::json::value& v, std::pmr::memory_resource* memory) {
        std::pmr::string text(memory);
        protocol::json::append_value(text, v);
        return own(protocol::payload_format::json, keep<std::pmr::string>(memory, std::move(text)));
    }

    static std::optional<boost::json::value> decode(incoming& m,
                                                    std::pmr::memory_resource* /*memory*/) {
        if (m.format != protocol::payload_format::json) {
            return std::nullopt;
        }
        boost::system::error_code ec;
        auto v = protocol::json::parse(as_text(m.payload()), m.json, ec);
        if (ec) {
            return std::nullopt;
        }
        return v;
    }
};

template <>
struct payload_traits<bytes> {
    static constexpr bool owning = true;

    static encoded_payload encode(bytes v, std::pmr::memory_resource* memory) {
        return own(protocol::payload_format::binary, keep<bytes>(memory, std::move(v)));
    }

    static std::optional<bytes> decode(incoming& m, std::pmr::memory_resource* /*memory*/) {
        if (m.format != protocol::payload_format::binary) {
            return std::nullopt;
        }
        const auto p = m.payload();
        return bytes(p.begin(), p.end());
    }
};

template <>
struct payload_traits<pmr_bytes> {
    static constexpr bool owning = true;

    static encoded_payload encode(pmr_bytes v, std::pmr::memory_resource* memory) {
        return own(protocol::payload_format::binary, keep<pmr_bytes>(memory, std::move(v)));
    }

    static std::optional<pmr_bytes> decode(incoming& m, std::pmr::memory_resource* memory) {
        if (m.format != protocol::payload_format::binary) {
            return std::nullopt;
        }
        return pmr_bytes(std::move(m).take_payload(), memory);
    }
};

template <>
struct payload_traits<std::span<const std::uint8_t>> {
    static constexpr bool owning = false;

    static encoded_payload encode(std::span<const std::uint8_t> v,
                                  std::pmr::memory_resource* memory) {
        return own(protocol::payload_format::binary, keep<pmr_bytes>(memory, v.begin(), v.end()));
    }

    static std::optional<std::span<const std::uint8_t>> decode(
        incoming& m, std::pmr::memory_resource* /*memory*/) {
        if (m.format != protocol::payload_format::binary) {
            return std::nullopt;
        }
        return m.payload();
    }
};

template <>
struct payload_traits<payload> {
    static constexpr bool owning = true;

    static encoded_payload encode(payload v, std::pmr::memory_resource* memory) {
        return std::visit(
            [memory](auto&& x) {
                return payload_traits<std::decay_t<decltype(x)>>::encode(
                    std::forward<decltype(x)>(x), memory);
            },
            std::move(v));
    }

    static std::optional<payload> decode(incoming& m, std::pmr::memory_resource* memory) {
        switch (m.format) {
            case protocol::payload_format::text:
                if (auto v = payload_traits<std::string>::decode(m, memory)) {
                    return payload(std::move(*v));
                }
                return std::nullopt;
            case protocol::payload_format::json:
                if (auto v = payload_traits<boost::json::value>::decode(m, memory)) {
                    return payload(std::move(*v));
                }
                return std::nullopt;
            case protocol::payload_format::binary:
                if (auto v = payload_traits<bytes>::decode(m, memory)) {
                    return payload(std::move(*v));
                }
                return std::nullopt;
        }
        return std::nullopt;
    }
};

/**
 * A user's completion handler, type-erased, and whether its associated executor is the room's,
 * which only its concrete type tells cheaply: a type-erased executor is compared through objects
 * Asio allocates.
 */
template <class Signature>
struct completion {
    boost::asio::any_completion_handler<Signature> handler;
    bool on_room_executor = false;

    using allocator_type =
        boost::asio::associated_allocator_t<boost::asio::any_completion_handler<Signature>>;

    [[nodiscard]] allocator_type get_allocator() const noexcept {
        return boost::asio::get_associated_allocator(handler);
    }

    template <class... Args>
    void operator()(Args&&... args) && {
        std::move(handler)(std::forward<Args>(args)...);
    }
};

/**
 * Everything a joined room is: the strategy, the negotiations, the connected peers and their
 * handshakes, the action wire and the actions the application made. `room` and `action` are handles
 * to it. Every allocation goes through the resource of its configuration.
 *
 * | Internal action   | Meaning                                                           |
 * | \@_hsdata         | a handshake message: the password challenge and its response      |
 * | \@_hsready        | the sender's side of the handshake passed                         |
 * | \@_ping, \@_pong  | round-trip measurement                                            |
 * | \@_leave          | the sender leaves the room                                        |
 */
class room_impl : public std::enable_shared_from_this<room_impl> {
public:
    using executor_type = boost::asio::any_io_executor;
    using message_handler = handler<void(incoming&)>;

    /**
     * Copies the configuration into its resource and makes the room's JSON storage there.
     *
     * Tip: the storage is counted, since a value a handler keeps may outlive the room; Boost.JSON
     * allocates its counter with `operator new`, once, here.
     */
    room_impl(executor_type ex, backend::factory& factory, room_config config,
              std::string_view room_id)
        : memory_(config.memory),
          json_(boost::json::make_shared_resource<json_resource>(memory_)),
          ex_(std::move(ex)),
          factory_(factory),
          log_(std::move(config.log), memory_),
          config_(adopt(config)),
          room_id_(room_id, memory_),
          token_(memory_),
          root_topic_(memory_),
          self_topic_(memory_),
          peers_(memory_),
          actions_(memory_) {
        if (config_.self_id.empty()) {
            config_.self_id = random_id(protocol::peer_id_size);
        }
        if (config_.ice_servers.empty()) {
            config_.ice_servers = default_ice_servers(memory_);
        }
        if (config_.relay_urls.empty()) {
            for (const auto url :
                 protocol::derive_relays(config_.app_id, protocol::default_nostr_relays,
                                         config_.relay_redundancy, memory_)) {
                config_.relay_urls.emplace_back(url);
            }
        }
        token_ = protocol::room_token(config_.app_id, room_id_, memory_);
        root_topic_ = protocol::root_topic(config_.app_id, room_id_, memory_);
        self_topic_ = protocol::peer_topic(config_.app_id, room_id_, config_.self_id, memory_);
    }

    void start() {
        const auto weak = weak_from_this();
        sealer_ = factory_.make_sealer(
            protocol::seal_key(config_.password, config_.app_id, room_id_), memory_);
        wire_ = std::allocate_shared<action_wire>(
            allocator(), factory_, token_, config_.max_receive_bytes,
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
                .has_receiver = bind<bool(std::string_view)>([weak](std::string_view name) {
                    const auto self = weak.lock();
                    return self && (name.starts_with("@_") || self->actions_.contains(name));
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
            },
            memory_, json_);
        signaling_ = std::allocate_shared<signaling>(
            allocator(), factory_, sealer_, config_.app_id, room_id_, config_.self_id,
            config_.ice_servers,
            signaling::callbacks{
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
                .failed = bind<void(std::string_view, boost::system::error_code)>(
                    [weak](std::string_view peer, boost::system::error_code ec) {
                        if (const auto self = weak.lock()) {
                            std::array<char, 128> buffer{};  // the message without a std::string
                            self->report(peer, ec, ec.message(buffer.data(), buffer.size()));
                        }
                    }),
            },
            log_, memory_, json_);
        std::pmr::vector<std::pmr::string> topics(memory_);
        topics.push_back(root_topic_);
        topics.push_back(self_topic_);
        strategy_ = std::allocate_shared<nostr_strategy>(
            allocator(), factory_, config_.relay_urls, std::move(topics), root_topic_,
            bind<std::pmr::string()>([weak, memory = memory_] {
                const auto self = weak.lock();
                return self ? protocol::write_signal({.peer_id = self->config_.self_id},
                                                     self->random_id(protocol::nonce_size), memory)
                            : std::pmr::string(memory);
            }),
            bind<void(std::size_t, std::string_view, std::string_view)>(
                [weak](std::size_t relay, std::string_view, std::string_view content) {
                    if (const auto self = weak.lock()) {
                        self->signaling_->handle(relay, content);
                    }
                }),
            log_, memory_, json_);
        log_("room", "joining room ", room_id_, " of app ", config_.app_id, " as ", config_.self_id,
             " over ", config_.relay_urls.size(), " relays");
        strategy_->start();
    }

    const executor_type& get_executor() const { return ex_; }

    std::pmr::memory_resource* memory() const { return memory_; }

    std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    std::string_view self_id() const { return config_.self_id; }

    std::span<const std::pmr::string> relay_urls() const { return config_.relay_urls; }

    std::pmr::vector<peer_id> active_peers() const {
        std::pmr::vector<peer_id> out(memory_);
        for (const auto& [id, p] : peers_) {
            if (p.active) {
                out.push_back(id);
            }
        }
        return out;
    }

    void on_peer_join(handler<void(std::string_view)> f) { on_join_ = keep_handler(std::move(f)); }

    void on_peer_leave(handler<void(std::string_view)> f) {
        on_leave_ = keep_handler(std::move(f));
    }

    void on_error(handler<void(std::string_view, boost::system::error_code, std::string_view)> f) {
        on_error_ = keep_handler(std::move(f));
    }

    /** Registers an action; frames that waited for it are delivered to it from now on. */
    void add_action(std::string_view name) {
        if (!actions_.contains(name)) {
            actions_.try_emplace(std::pmr::string(name, memory_), allocator());
        }
        if (wire_) {
            wire_->receiver_added();
        }
    }

    void set_action_handler(std::string_view name, message_handler handler) {
        auto& slot = slot_for(name);
        slot.handler = keep_handler(std::move(handler));
        while (!slot.queue.empty()) {
            post_call(slot.handler, std::move(slot.queue.front()));
            slot.queue.pop_front();
        }
    }

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
    }

    void send(std::string_view name, encoded_payload payload, const send_options& options,
              completion<void(boost::system::error_code)> handler) {
        if (left_) {
            complete(std::move(handler), make_error_code(error::room_left));
            return;
        }
        const auto weak = weak_from_this();
        const std::pmr::vector<std::string_view> targets(options.targets.begin(),
                                                         options.targets.end(), memory_);
        wire_->send(name, payload.format, options.metadata, std::move(payload.ref), targets,
                    is_internal_to_pending(name),
                    bind<void(boost::system::error_code)>(
                        [weak, handler = std::move(handler)](boost::system::error_code ec) mutable {
                            if (const auto self = weak.lock()) {
                                self->complete(std::move(handler), ec);
                            }
                        }));
    }

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
            {.started_ms = factory_.now_ms(), .handler = std::move(handler)});
        send_internal("@_ping", it->first);
    }

    void leave(completion<void(boost::system::error_code)> handler) {
        if (left_) {
            complete(std::move(handler), boost::system::error_code{});
            return;
        }
        left_ = true;
        log_("room", "leaving room ", room_id_);
        for (const auto& [id, p] : peers_) {
            send_internal("@_leave", id);
        }
        // Trystero gives the farewell 99 ms to leave before closing the connections.
        leave_timer_ = factory_.make_timer(memory_);
        leave_timer_->start(
            std::chrono::milliseconds(99),
            bind<void()>([self = shared_from_this(), handler = std::move(handler)]() mutable {
                self->close_everything();
                self->complete(std::move(handler), boost::system::error_code{});
            }));
    }

    /** Leaves at once, without the farewell's grace period: the room handle was destroyed. */
    void abandon() {
        if (std::exchange(left_, true)) {
            return;
        }
        for (const auto& [id, p] : peers_) {
            send_internal("@_leave", id);
        }
        close_everything();
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
            const auto allocator = boost::asio::get_associated_allocator(handler);
            post_handler(ex_, boost::asio::bind_allocator(
                                  allocator, [handler = std::forward<Handler>(handler),
                                              ... args = std::forward<Args>(args)]() mutable {
                                      std::move(handler)(std::move(args)...);
                                  }));
            return;
        }
        if constexpr (is_completion<std::decay_t<Handler>>) {
            boost::asio::post(
                ex_, boost::asio::append(std::move(handler.handler), std::forward<Args>(args)...));
        } else {
            boost::asio::post(ex_, boost::asio::append(std::forward<Handler>(handler),
                                                       std::forward<Args>(args)...));
        }
    }

    /** A user's completion handler, type-erased, with whether it runs on the room's executor. */
    template <class Signature, class Handler>
    completion<Signature> erase(Handler&& handler) const {
        const bool here = on_room_executor(handler);
        return {
            boost::asio::any_completion_handler<Signature>(std::forward<Handler>(handler)),
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
                                 boost::asio::associated_executor_t<Handler, executor_type>,
                                 executor_type>) {
            return boost::asio::get_associated_executor(handler, ex_) == ex_;
        } else {
            return false;
        }
    }

    struct pending_ping {
        double started_ms = 0;
        completion<void(boost::system::error_code, std::chrono::milliseconds)> handler;
    };

    struct peer_state {
        explicit peer_state(std::pmr::polymorphic_allocator<> a) : challenge(a), pings(a) {}

        std::shared_ptr<peer_link> link;
        bool active = false;
        bool local_passed = false;
        bool remote_ready = false;
        std::pmr::string challenge;  // the initiator's, while it waits for the response
        std::shared_ptr<backend::timer> handshake_timer;
        std::pmr::deque<pending_ping> pings;
    };

    struct action_slot {
        explicit action_slot(std::pmr::polymorphic_allocator<> a) : queue(a), receivers(a) {}

        std::shared_ptr<message_handler> handler;
        std::pmr::deque<incoming> queue;
        std::pmr::deque<completion<void(boost::system::error_code, incoming)>> receivers;
    };

    static constexpr std::size_t max_queued_messages = 1024;

    /** A copy of the configuration with every part in its resource, but the sink, which the logger
     * takes. */
    static room_config adopt(const room_config& c) {
        std::pmr::memory_resource* const m = c.memory;
        return room_config{
            .app_id = std::pmr::string(c.app_id, m),
            .password = std::pmr::string(c.password, m),
            .relay_urls = std::pmr::vector<std::pmr::string>(c.relay_urls, m),
            .relay_redundancy = c.relay_redundancy,
            .ice_servers = copy_ice_servers(c.ice_servers, m),
            .self_id = std::pmr::string(c.self_id, m),
            .handshake_timeout = c.handshake_timeout,
            .max_receive_bytes = c.max_receive_bytes,
            .log = {},
            .memory = m,
        };
    }

    /** A handler that several posts call, shared in the room's resource. */
    template <class Signature>
    std::shared_ptr<handler<Signature>> keep_handler(handler<Signature> f) const {
        if (!f) {
            return nullptr;
        }
        return std::allocate_shared<handler<Signature>>(allocator(), std::move(f));
    }

    template <class F>
    void post(F&& f) {
        detail::post(ex_, memory_, std::forward<F>(f));
    }

    std::pmr::string random_id(std::size_t n) {
        std::array<std::uint8_t, 64> random{};
        factory_.random(std::span(random).first(n));
        std::pmr::string id(n, '0', memory_);
        for (std::size_t i = 0; i < n; ++i) {
            id[i] = protocol::id_alphabet[random[i] % protocol::id_alphabet.size()];
        }
        return id;
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

    void post_call(const std::shared_ptr<message_handler>& handler, incoming m) {
        post([handler, m = std::move(m)]() mutable { (*handler)(m); });
    }

    void report(std::string_view peer, boost::system::error_code ec, std::string_view what) {
        log_("room", "peer ", peer, ": ", what);
        if (on_error_) {
            post([f = on_error_, peer = std::pmr::string(peer, memory_), ec,
                  what = std::pmr::string(what, memory_)] { (*f)(peer, ec, what); });
        }
    }

    void on_connected(std::string_view peer_view, std::shared_ptr<backend::peer_connection> pc,
                      std::shared_ptr<backend::data_channel> channel) {
        if (left_) {
            channel->close();
            pc->close();
            return;
        }
        const auto weak = weak_from_this();
        const auto it = peers_.try_emplace(std::pmr::string(peer_view, memory_), allocator()).first;
        const std::pmr::string& peer = it->first;
        auto& state = it->second;
        state.link = std::allocate_shared<peer_link>(
            allocator(), factory_, peer, token_, std::move(pc), std::move(channel),
            peer_link::callbacks{
                .frame =
                    bind<void(std::string_view, backend::bytes&&, std::span<const std::uint8_t>)>(
                        [weak](std::string_view p, backend::bytes&& frame,
                               std::span<const std::uint8_t> action) {
                            if (const auto self = weak.lock()) {
                                self->wire_->handle(p, std::move(frame), action);
                            }
                        }),
                .closed = bind<void(std::string_view, std::string_view)>(
                    [weak](std::string_view p, std::string_view reason) {
                        if (const auto self = weak.lock()) {
                            self->remove_peer(p, reason, false);
                        }
                    }),
            },
            log_, memory_);
        state.handshake_timer = factory_.make_timer(memory_);
        state.handshake_timer->start(
            config_.handshake_timeout, bind<void()>([weak, peer = std::pmr::string(peer, memory_)] {
                const auto self = weak.lock();
                if (!self) {
                    return;
                }
                self->report(peer, make_error_code(error::handshake_timed_out),
                             "handshake timed out");
                self->kick(peer, "handshake timed out");
            }));
        state.link->start();
        start_handshake(peer, state);
    }

    void start_handshake(std::string_view peer, peer_state& state) {
        if (config_.password.empty()) {
            pass_local(peer);
            return;
        }
        if (config_.self_id < peer) {
            state.challenge = random_id(36);
            std::pmr::string text(R"({"__trystero_pw":"challenge","c":)", memory_);
            protocol::json::append_string(text, state.challenge);
            text += '}';
            send_internal("@_hsdata", peer, protocol::payload_format::json, text);
        }
    }

    void on_handshake_data(std::string_view peer, std::string_view data) {
        const auto it = peers_.find(peer);
        if (it == peers_.end() || it->second.active || it->second.local_passed) {
            return;
        }
        boost::system::error_code ec;
        const auto value = protocol::json::parse(data, json_, ec);
        const auto* o = ec ? nullptr : value.if_object();
        const auto field = [o](std::string_view key) -> std::string_view {
            const auto* v = o != nullptr ? o->if_contains(key) : nullptr;
            const auto* s = v != nullptr ? v->if_string() : nullptr;
            return s != nullptr ? std::string_view(s->data(), s->size()) : std::string_view();
        };
        if (config_.self_id < peer) {
            if (field("__trystero_pw") != "response" ||
                field("h") != protocol::handshake_proof(it->second.challenge, config_.password,
                                                        config_.app_id, room_id_, memory_)) {
                fail_handshake(peer);
                return;
            }
        } else {
            if (field("__trystero_pw") != "challenge" || o->if_contains("c") == nullptr) {
                fail_handshake(peer);
                return;
            }
            std::pmr::string text(R"({"__trystero_pw":"response","h":)", memory_);
            protocol::json::append_string(
                text, protocol::handshake_proof(field("c"), config_.password, config_.app_id,
                                                room_id_, memory_));
            text += '}';
            send_internal("@_hsdata", peer, protocol::payload_format::json, text);
        }
        pass_local(peer);
    }

    void fail_handshake(std::string_view peer) {
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
        it->second.handshake_timer.reset();
        log_("room", "peer joined: ", it->first);
        if (on_join_) {
            post([f = on_join_, peer = std::pmr::string(it->first, memory_)] { (*f)(peer); });
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
                const auto elapsed = std::chrono::milliseconds(
                    static_cast<long long>(factory_.now_ms() - ping.started_ms));
                complete(std::move(ping.handler), boost::system::error_code{}, elapsed);
            }
        } else if (name == "@_leave") {
            remove_peer(m.peer, "peer left the room", true);
        } else if (name.starts_with("@_")) {
            // Media and renegotiation actions: not implemented yet (the documentation's roadmap).
        } else if (const auto it = actions_.find(name); it != actions_.end()) {
            auto& slot = it->second;
            if (slot.handler) {
                post_call(slot.handler, std::move(m));
            } else if (!slot.receivers.empty()) {
                auto receiver = std::move(slot.receivers.front());
                slot.receivers.pop_front();
                complete(std::move(receiver), boost::system::error_code{}, std::move(m));
            } else {
                if (slot.queue.size() >= max_queued_messages) {
                    log_("action", "dropping the oldest unread message of ", it->first);
                    slot.queue.pop_front();
                }
                slot.queue.push_back(std::move(m));
            }
        }
    }

    void kick(std::string_view peer, std::string_view reason) {
        const auto it = peers_.find(peer);
        if (it == peers_.end()) {
            return;
        }
        send_internal("@_leave", it->first);
        remove_peer(it->first, reason, true);
    }

    void remove_peer(std::string_view peer_view, std::string_view reason, bool close_link) {
        const auto it = peers_.find(peer_view);
        if (it == peers_.end()) {
            return;
        }
        // The id is moved out of the map before the entry goes: `peer_view` may refer to it.
        const auto node = peers_.extract(it);
        std::pmr::string peer = std::move(node.key());
        auto& state = node.mapped();
        log_("room", "peer ", peer, " gone: ", reason);
        if (close_link) {
            state.link->close();
        }
        for (auto& ping : state.pings) {
            complete(std::move(ping.handler), make_error_code(error::peer_disconnected),
                     std::chrono::milliseconds{});
        }
        wire_->clear_peer(peer, make_error_code(error::peer_disconnected));
        if (left_) {
            return;
        }
        signaling_->forget(peer);
        strategy_->restart_announcements();
        if (state.active && on_leave_) {
            post([f = on_leave_, peer = std::move(peer)] { (*f)(peer); });
        }
    }

    void close_everything() {
        leave_timer_.reset();
        auto peers = std::move(peers_);
        peers_.clear();
        for (auto& [id, state] : peers) {
            state.link->close();
            for (auto& ping : state.pings) {
                complete(std::move(ping.handler), make_error_code(error::room_left),
                         std::chrono::milliseconds{});
            }
            if (wire_) {
                wire_->clear_peer(id, make_error_code(error::room_left));
            }
        }
        for (auto& [name, slot] : actions_) {
            for (auto& receiver : slot.receivers) {
                complete(std::move(receiver), make_error_code(error::room_left), incoming{});
            }
            slot.receivers.clear();
        }
        if (signaling_) {
            signaling_->close();
        }
        if (strategy_) {
            strategy_->stop();
        }
    }

    std::pmr::memory_resource* memory_;
    boost::json::storage_ptr json_;
    executor_type ex_;
    backend::factory& factory_;
    logger log_;
    room_config config_;
    std::pmr::string room_id_;
    std::pmr::string token_;
    std::pmr::string root_topic_;
    std::pmr::string self_topic_;
    std::shared_ptr<backend::sealer> sealer_;
    std::shared_ptr<action_wire> wire_;
    std::shared_ptr<signaling> signaling_;
    std::shared_ptr<nostr_strategy> strategy_;
    std::pmr::map<std::pmr::string, peer_state, std::less<>> peers_;
    std::pmr::map<std::pmr::string, action_slot, std::less<>> actions_;
    std::shared_ptr<handler<void(std::string_view)>> on_join_;
    std::shared_ptr<handler<void(std::string_view)>> on_leave_;
    std::shared_ptr<handler<void(std::string_view, boost::system::error_code, std::string_view)>>
        on_error_;
    std::shared_ptr<backend::timer> leave_timer_;
    bool left_ = false;
};

}  // namespace detail

template <class T>
class action;

/**
 * A room joined over Trystero's Nostr strategy. Constructing it joins; `async_leave` leaves with
 * Trystero's farewell, and destroying it leaves at once.
 *
 * Tip: every handler the room calls, and every completion, runs on the executor it was constructed
 * with. The views a handler receives are valid for the duration of the call. Every allocation of
 * the room goes through `room_config::memory`; a completion handler is allocated as its own
 * associated allocator says. Destroying the room leaves operations pending on its executor, which
 * free into that resource: it must outlive them (see `room_config::memory`).
 */
class room {
public:
    using executor_type = boost::asio::any_io_executor;

    room(executor_type ex, backend::factory& factory, room_config config, std::string_view room_id)
        : impl_(make(std::move(ex), factory, std::move(config), room_id)) {
        impl_->start();
    }

    room(const room&) = delete;
    room& operator=(const room&) = delete;
    room(room&&) noexcept = default;
    room& operator=(room&&) noexcept = default;

    // Leaving sends each peer Trystero's farewell, whose frames are allocated in the room's
    // resource: a resource that fails then ends the program, as in any destructor.
    ~room() {  // NOLINT(bugprone-exception-escape)
        if (impl_) {
            impl_->abandon();
        }
    }

    [[nodiscard]] const executor_type& get_executor() const { return impl_->get_executor(); }

    [[nodiscard]] std::string_view self_id() const { return impl_->self_id(); }

    /** The Nostr relays this room uses, derived from the app id unless the configuration named
     * them. */
    [[nodiscard]] std::span<const std::pmr::string> relay_urls() const {
        return impl_->relay_urls();
    }

    /** The peers whose handshake completed, in the room's resource. */
    [[nodiscard]] std::pmr::vector<peer_id> peers() const { return impl_->active_peers(); }

    template <class F>
        requires std::is_invocable_v<F&, std::string_view>
    void on_peer_join(F&& f) {
        impl_->on_peer_join(impl_->bind<void(std::string_view)>(std::forward<F>(f)));
    }

    template <class F>
        requires std::is_invocable_v<F&, std::string_view>
    void on_peer_leave(F&& f) {
        impl_->on_peer_leave(impl_->bind<void(std::string_view)>(std::forward<F>(f)));
    }

    /** Problems with one peer that the room handled itself: a wrong password, a timed-out
     * handshake. */
    template <class F>
        requires std::is_invocable_v<F&, std::string_view, boost::system::error_code,
                                     std::string_view>
    void on_error(F&& f) {
        impl_->on_error(
            impl_->bind<void(std::string_view, boost::system::error_code, std::string_view)>(
                std::forward<F>(f)));
    }

    /** An action of this room; `T` is one of the types detail::payload_traits lists. */
    template <class T>
    action<T> make_action(std::string_view name);

    /**
     * Measures the round trip to a peer. Completes with `(error_code, std::chrono::milliseconds)`.
     * The id is copied at the call, so a deferred operation does not depend on the caller's string.
     */
    template <class CompletionToken>
    auto async_ping(std::string_view peer, CompletionToken&& token) {
        return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code,
                                                                 std::chrono::milliseconds)>(
            [impl = impl_](auto handler, const peer_id& peer) {
                impl->ping(peer, impl->template erase<void(boost::system::error_code,
                                                           std::chrono::milliseconds)>(
                                     std::move(handler)));
            },
            token, peer_id(peer, impl_->memory()));
    }

    /** Says goodbye to every peer and closes everything. Completes with `(error_code)`. */
    template <class CompletionToken>
    auto async_leave(CompletionToken&& token) {
        return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
            [impl = impl_](auto handler) {
                impl->leave(
                    impl->template erase<void(boost::system::error_code)>(std::move(handler)));
            },
            token);
    }

private:
    static std::shared_ptr<detail::room_impl> make(executor_type ex, backend::factory& factory,
                                                   room_config config, std::string_view room_id) {
        std::pmr::memory_resource* const memory = config.memory;
        BOOST_ASSERT_MSG(memory != nullptr, "room_config::memory names no resource");
        return std::allocate_shared<detail::room_impl>(std::pmr::polymorphic_allocator<>(memory),
                                                       std::move(ex), factory, std::move(config),
                                                       room_id);
    }

    std::shared_ptr<detail::room_impl> impl_;
};

/**
 * A named channel of messages of type `T` within a room, wire compatible with Trystero's
 * `makeAction`. Messages arrive at the handler `on_message` set, or, without one, queue for
 * `async_receive`.
 */
template <class T>
class action {
public:
    [[nodiscard]] std::string_view name() const { return name_; }

    /** The handler for every message; with a view type, the view is valid for the duration of the
     * call. */
    template <class F>
        requires std::is_invocable_v<F&, T, const message_info&>
    void on_message(F&& f) {
        impl_->set_action_handler(
            name_,
            impl_->bind<void(detail::incoming&)>(
                [f = std::forward<F>(f), memory = impl_->memory()](detail::incoming& m) mutable {
                    // The metadata is read before decoding: a binary payload takes the buffer the
                    // metadata lives in.
                    const message_info info{
                        .from = std::move(m.peer),
                        .metadata = std::pmr::string(m.metadata(), memory),
                    };
                    if (auto value = detail::payload_traits<T>::decode(m, memory)) {
                        f(std::move(*value), info);
                    }
                }));
    }

    // T is the action's type, taken by value and moved into its encoding, which keeps a value
    // that owns its storage; a boost::json::value is serialized there at once, from a reference.
    /**
     * Sends to `options.targets`, or every active peer. The value is taken at the call (moved when
     * `T` owns its storage). Completes with `(error_code)` once every target has it.
     */
    template <class CompletionToken>
    auto async_send(T value, send_options options,  // NOLINT(performance-unnecessary-value-param)
                    CompletionToken&& token) {
        return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
            [impl = impl_, name = std::pmr::string(name_, impl_->memory())](
                auto handler, detail::encoded_payload payload, const send_options& options) {
                impl->send(
                    name, std::move(payload), options,
                    impl->template erase<void(boost::system::error_code)>(std::move(handler)));
            },
            token, detail::payload_traits<T>::encode(std::move(value), impl_->memory()),
            std::move(options));
    }

    template <class CompletionToken>
    auto async_send(T value, CompletionToken&& token) {
        return async_send(std::move(value), send_options(), std::forward<CompletionToken>(token));
    }

    /**
     * The next message, when no handler is set. Completes with `(error_code, T, message_info)`; a
     * message of another payload format than `T` completes with `error::transfer_rejected` and an
     * empty value.
     */
    template <class CompletionToken>
    auto async_receive(CompletionToken&& token) {
        static_assert(detail::payload_traits<T>::owning,
                      "a received view would outlive the message: await an owning type");
        return boost::asio::async_initiate<CompletionToken,
                                           void(boost::system::error_code, T, message_info)>(
            [impl = impl_, name = std::pmr::string(name_, impl_->memory())](auto handler) {
                // The decoding step is allocated as the user's handler is, which it carries.
                const auto allocator = boost::asio::get_associated_allocator(handler);
                impl->receive(
                    name,
                    impl->template erase<void(boost::system::error_code, detail::incoming)>(
                        boost::asio::bind_allocator(allocator, [impl, handler = std::move(handler)](
                                                                   boost::system::error_code ec,
                                                                   detail::incoming m) mutable {
                            auto* const memory = impl->memory();
                            message_info info{
                                .from = std::move(m.peer),
                                .metadata = std::pmr::string(m.metadata(), memory),
                            };
                            auto value =
                                ec ? std::nullopt : detail::payload_traits<T>::decode(m, memory);
                            if (!ec && !value) {
                                ec = make_error_code(error::transfer_rejected);
                            }
                            impl->complete(std::move(handler), ec, value ? std::move(*value) : T{},
                                           std::move(info));
                        })));
            },
            token);
    }

private:
    friend class room;

    action(std::shared_ptr<detail::room_impl> impl, std::string_view name)
        : impl_(std::move(impl)), name_(name, impl_->memory()) {
        impl_->add_action(name_);
    }

    std::shared_ptr<detail::room_impl> impl_;
    std::pmr::string name_;
};

template <class T>
action<T> room::make_action(std::string_view name) {
    return action<T>(impl_, name);
}

}  // namespace webcpp::trystero

#endif
