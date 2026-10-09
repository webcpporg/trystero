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
#include <webcpp/trystero/detail/signaling.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/append.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace webcpp::trystero {

using peer_id = std::string;
using bytes = std::vector<std::uint8_t>;

/** What an action carries when its type is not fixed: Trystero's text, JSON or binary payload. */
using payload = std::variant<std::string, boost::json::value, bytes>;

struct room_config {
    std::string app_id = {};
    /** Seals signals and adds a challenge to the handshake; peers with another password never join. */
    std::string password = {};
    /** Nostr relays to use; empty means Trystero's defaults, derived from the app id. */
    std::vector<std::string> relay_urls = {};
    std::size_t relay_redundancy = 5;
    /** STUN and TURN servers; empty means Trystero's defaults. */
    std::vector<backend::ice_server> ice_servers = {};
    /** This peer's id in the room; empty means 20 random characters. */
    std::string self_id = {};
    std::chrono::milliseconds handshake_timeout{10'000};
    std::size_t max_receive_bytes = 256 * 1024 * 1024;
    /** Diagnostics, by category; see detail::logger. */
    detail::logger::sink log = {};
};

/** Who sent a message, and the metadata it carried (JSON text, or empty). */
struct message_info {
    peer_id from;
    std::string metadata;
};

/**
 * Where an action goes: every active peer when `targets` is empty. `metadata` is JSON text, or empty.
 * Tip: it owns its strings because a completion token such as `asio::deferred` starts the operation later, after the
 * caller's views may be gone.
 */
struct send_options {
    std::vector<peer_id> targets = {};
    std::string metadata = {};
};

/** Trystero's default STUN servers. */
inline std::vector<backend::ice_server> default_ice_servers() {
    return {
        {{"stun:stun.l.google.com:19302"}, {}, {}},
        {{"stun:stun1.l.google.com:19302"}, {}, {}},
        {{"stun:stun2.l.google.com:19302"}, {}, {}},
        {{"stun:stun.cloudflare.com:3478"}, {}, {}},
    };
}

namespace detail {

/** A payload ready to send: its format, and its bytes kept alive by what owns them. */
struct encoded_payload {
    protocol::payload_format format = protocol::payload_format::text;
    payload_ref ref;
};

template <class Owner>
encoded_payload own(protocol::payload_format format, std::shared_ptr<const Owner> owner) {
    const auto data = std::span(reinterpret_cast<const std::uint8_t*>(owner->data()), owner->size());
    return {format, {data, std::move(owner)}};
}

inline std::string_view as_text(std::span<const std::uint8_t> data) { return {reinterpret_cast<const char*>(data.data()), data.size()}; }

/**
 * How an action's type travels. `encode` takes the value at the call and keeps it alive without copying it when the
 * type owns its storage; a view is copied once, there. `decode` reads a received message: a view type refers to the
 * message's buffer and is valid for the duration of the handler, so only owning types can be awaited.
 *
 * | Type                           | Trystero format | Receiving                            |
 * | std::string                    | text            | a copy of the bytes                  |
 * | std::string_view               | text            | a view of the frame, in a handler    |
 * | boost::json::value             | JSON            | parsed                               |
 * | trystero::bytes                | binary          | the transfer's buffer, moved         |
 * | std::span<const std::uint8_t>  | binary          | a view of the frame, in a handler    |
 * | trystero::payload              | any             | by format                            |
 */
template <class T>
struct payload_traits;

template <>
struct payload_traits<std::string> {
    static constexpr bool owning = true;
    static encoded_payload encode(std::string v) { return own(protocol::payload_format::text, std::make_shared<const std::string>(std::move(v))); }
    static std::optional<std::string> decode(incoming& m) {
        if (m.format != protocol::payload_format::text) return std::nullopt;
        return std::string(as_text(m.payload()));
    }
};

template <>
struct payload_traits<std::string_view> {
    static constexpr bool owning = false;
    static encoded_payload encode(std::string_view v) { return own(protocol::payload_format::text, std::make_shared<const std::string>(v)); }
    static std::optional<std::string_view> decode(incoming& m) {
        if (m.format != protocol::payload_format::text) return std::nullopt;
        return as_text(m.payload());
    }
};

template <>
struct payload_traits<boost::json::value> {
    static constexpr bool owning = true;
    static encoded_payload encode(const boost::json::value& v) {
        return own(protocol::payload_format::json, std::make_shared<const std::string>(boost::json::serialize(v)));
    }
    static std::optional<boost::json::value> decode(incoming& m) {
        if (m.format != protocol::payload_format::json) return std::nullopt;
        const auto text = as_text(m.payload());
        boost::system::error_code ec;
        auto v = boost::json::parse(boost::json::string_view(text.data(), text.size()), ec);
        if (ec) return std::nullopt;
        return v;
    }
};

template <>
struct payload_traits<bytes> {
    static constexpr bool owning = true;
    static encoded_payload encode(bytes v) { return own(protocol::payload_format::binary, std::make_shared<const bytes>(std::move(v))); }
    static std::optional<bytes> decode(incoming& m) {
        if (m.format != protocol::payload_format::binary) return std::nullopt;
        return std::move(m).take_payload();
    }
};

template <>
struct payload_traits<std::span<const std::uint8_t>> {
    static constexpr bool owning = false;
    static encoded_payload encode(std::span<const std::uint8_t> v) {
        return own(protocol::payload_format::binary, std::make_shared<const bytes>(v.begin(), v.end()));
    }
    static std::optional<std::span<const std::uint8_t>> decode(incoming& m) {
        if (m.format != protocol::payload_format::binary) return std::nullopt;
        return m.payload();
    }
};

template <>
struct payload_traits<payload> {
    static constexpr bool owning = true;
    static encoded_payload encode(payload v) {
        return std::visit([](auto&& x) { return payload_traits<std::decay_t<decltype(x)>>::encode(std::move(x)); }, std::move(v));
    }
    static std::optional<payload> decode(incoming& m) {
        switch (m.format) {
            case protocol::payload_format::text: return payload(*payload_traits<std::string>::decode(m));
            case protocol::payload_format::json:
                if (auto v = payload_traits<boost::json::value>::decode(m)) return payload(std::move(*v));
                return std::nullopt;
            case protocol::payload_format::binary: return payload(*payload_traits<bytes>::decode(m));
        }
        return std::nullopt;
    }
};

/**
 * Everything a joined room is: the strategy, the negotiations, the connected peers and their handshakes, the action
 * wire and the actions the application made. `room` and `action` are handles to it.
 *
 * | Internal action | Meaning                                                             |
 * | @_hsdata        | a handshake message: the password challenge and its response        |
 * | @_hsready       | the sender's side of the handshake passed                           |
 * | @_ping, @_pong  | round-trip measurement                                              |
 * | @_leave         | the sender leaves the room                                          |
 */
class room_impl : public std::enable_shared_from_this<room_impl> {
public:
    using executor_type = boost::asio::any_io_executor;
    using message_handler = std::function<void(incoming&)>;

    room_impl(executor_type ex, backend::factory& factory, room_config config, std::string_view room_id)
        : ex_(std::move(ex)), factory_(factory), config_(std::move(config)), room_id_(room_id), log_(config_.log) {
        if (config_.self_id.empty()) config_.self_id = random_id(protocol::peer_id_size);
        if (config_.ice_servers.empty()) config_.ice_servers = default_ice_servers();
        if (config_.relay_urls.empty()) {
            for (const auto url : protocol::derive_relays(config_.app_id, protocol::default_nostr_relays, config_.relay_redundancy)) {
                config_.relay_urls.emplace_back(url);
            }
        }
        token_ = protocol::room_token(config_.app_id, room_id_);
        root_topic_ = protocol::root_topic(config_.app_id, room_id_);
        self_topic_ = protocol::peer_topic(config_.app_id, room_id_, config_.self_id);
    }

    void start() {
        auto weak = weak_from_this();
        sealer_ = factory_.make_sealer(protocol::seal_key(config_.password, config_.app_id, room_id_));
        wire_ = std::make_shared<action_wire>(factory_, token_, config_.max_receive_bytes, action_wire::callbacks{
            .find_link = [weak](std::string_view peer, bool include_pending) -> std::optional<action_wire::link> {
                auto self = weak.lock();
                return self ? self->link_to(peer, include_pending) : std::nullopt;
            },
            .peers = [weak](bool include_pending) {
                auto self = weak.lock();
                return self ? self->peer_views(include_pending) : std::vector<std::string_view>{};
            },
            .may_receive = [weak](std::string_view peer, std::string_view name) {
                auto self = weak.lock();
                return self && self->may_receive(peer, name);
            },
            .has_receiver = [weak](std::string_view name) {
                auto self = weak.lock();
                return self && (name.starts_with("@_") || self->actions_.contains(name));
            },
            .deliver = [weak](incoming&& m) { if (auto self = weak.lock()) self->on_incoming(std::move(m)); },
            .fail_peer = [weak](std::string_view peer, std::string_view reason) { if (auto self = weak.lock()) self->kick(peer, reason); },
        });
        signaling_ = std::make_shared<signaling>(factory_, sealer_, config_.app_id, room_id_, config_.self_id, config_.ice_servers, signaling::callbacks{
            .publish = [weak](std::size_t relay, std::string_view topic, std::string_view content) {
                if (auto self = weak.lock()) self->strategy_->publish(relay, topic, content);
            },
            .connected = [weak](std::string_view peer, std::shared_ptr<backend::peer_connection> pc, std::shared_ptr<backend::data_channel> channel) {
                if (auto self = weak.lock()) self->on_connected(peer, std::move(pc), std::move(channel));
            },
            .failed = [weak](std::string_view peer, boost::system::error_code ec) { if (auto self = weak.lock()) self->report(peer, ec, ec.message()); },
        }, log_);
        strategy_ = std::make_shared<nostr_strategy>(factory_, config_.relay_urls, std::vector<std::string>{root_topic_, self_topic_}, root_topic_,
            [weak]() -> std::string {
                auto self = weak.lock();
                return self ? protocol::write_signal({.peer_id = self->config_.self_id}, self->random_id(protocol::nonce_size)) : std::string();
            },
            [weak](std::size_t relay, std::string_view, std::string_view content) {
                if (auto self = weak.lock()) self->signaling_->handle(relay, content);
            },
            log_);
        log_("room", [&] {
            return "joining room " + room_id_ + " of app " + config_.app_id + " as " + config_.self_id + " over " +
                   std::to_string(config_.relay_urls.size()) + " relays";
        });
        strategy_->start();
    }

    const executor_type& get_executor() const { return ex_; }
    std::string_view self_id() const { return config_.self_id; }
    std::span<const std::string> relay_urls() const { return config_.relay_urls; }

    std::vector<peer_id> active_peers() const {
        std::vector<peer_id> out;
        for (const auto& [id, p] : peers_) {
            if (p.active) out.push_back(id);
        }
        return out;
    }

    void on_peer_join(std::function<void(std::string_view)> f) { on_join_ = std::move(f); }
    void on_peer_leave(std::function<void(std::string_view)> f) { on_leave_ = std::move(f); }
    void on_error(std::function<void(std::string_view, boost::system::error_code, std::string_view)> f) { on_error_ = std::move(f); }

    /** Registers an action; frames that waited for it are delivered to it from now on. */
    void add_action(std::string_view name) {
        if (!actions_.contains(name)) actions_.try_emplace(std::string(name));
        if (wire_) wire_->receiver_added();
    }

    void set_action_handler(std::string_view name, message_handler handler) {
        auto& slot = slot_for(name);
        slot.handler = std::make_shared<message_handler>(std::move(handler));
        while (!slot.queue.empty()) {
            post_call(slot.handler, std::move(slot.queue.front()));
            slot.queue.pop_front();
        }
    }

    void receive(std::string_view name, boost::asio::any_completion_handler<void(boost::system::error_code, incoming)> handler) {
        if (left_) return complete(std::move(handler), make_error_code(error::room_left), incoming{});
        auto& slot = slot_for(name);
        if (!slot.queue.empty()) {
            auto m = std::move(slot.queue.front());
            slot.queue.pop_front();
            return complete(std::move(handler), boost::system::error_code{}, std::move(m));
        }
        slot.receivers.push_back(std::move(handler));
    }

    void send(std::string_view name, encoded_payload payload, send_options options, boost::asio::any_completion_handler<void(boost::system::error_code)> handler) {
        if (left_) return complete(std::move(handler), make_error_code(error::room_left));
        auto shared_handler = std::make_shared<decltype(handler)>(std::move(handler));
        auto weak = weak_from_this();
        const std::vector<std::string_view> targets(options.targets.begin(), options.targets.end());
        wire_->send(name, payload.format, options.metadata, std::move(payload.ref), targets, is_internal_to_pending(name),
                    [weak, shared_handler](boost::system::error_code ec) {
                        if (auto self = weak.lock()) self->complete(std::move(*shared_handler), ec);
                    });
    }

    void ping(std::string_view peer, boost::asio::any_completion_handler<void(boost::system::error_code, std::chrono::milliseconds)> handler) {
        auto it = peers_.find(peer);
        if (left_ || it == peers_.end() || !it->second.active) {
            return complete(std::move(handler), make_error_code(left_ ? error::room_left : error::no_such_peer), std::chrono::milliseconds{});
        }
        it->second.pings.push_back({factory_.now_ms(), std::move(handler)});
        send_internal("@_ping", it->first);
    }

    void leave(boost::asio::any_completion_handler<void(boost::system::error_code)> handler) {
        if (left_) return complete(std::move(handler), boost::system::error_code{});
        left_ = true;
        log_("room", [&] { return "leaving room " + room_id_; });
        for (const auto& [id, p] : peers_) send_internal("@_leave", id);
        // Trystero gives the farewell 99 ms to leave before closing the connections.
        leave_timer_ = factory_.make_timer();
        auto self = shared_from_this();
        auto shared_handler = std::make_shared<decltype(handler)>(std::move(handler));
        leave_timer_->start(std::chrono::milliseconds(99), [self, shared_handler] {
            self->close_everything();
            self->complete(std::move(*shared_handler), boost::system::error_code{});
        });
    }

    /** Leaves at once, without the farewell's grace period: the room handle was destroyed. */
    void abandon() {
        if (std::exchange(left_, true)) return;
        for (const auto& [id, p] : peers_) send_internal("@_leave", id);
        close_everything();
    }

    template <class Handler, class... Args>
    void complete(Handler&& handler, Args&&... args) {
        boost::asio::post(ex_, boost::asio::append(std::forward<Handler>(handler), std::forward<Args>(args)...));
    }

private:
    struct pending_ping {
        double started_ms = 0;
        boost::asio::any_completion_handler<void(boost::system::error_code, std::chrono::milliseconds)> handler;
    };

    struct peer_state {
        std::shared_ptr<peer_link> link;
        bool active = false;
        bool local_passed = false;
        bool remote_ready = false;
        std::string challenge;  // the initiator's, while it waits for the response
        std::unique_ptr<backend::timer> handshake_timer;
        std::deque<pending_ping> pings;
    };

    struct action_slot {
        std::shared_ptr<message_handler> handler;
        std::deque<incoming> queue;
        std::deque<boost::asio::any_completion_handler<void(boost::system::error_code, incoming)>> receivers;
    };

    static constexpr std::size_t max_queued_messages = 1024;

    std::string random_id(std::size_t n) {
        std::array<std::uint8_t, 64> random{};
        factory_.random(std::span(random).first(n));
        std::string id(n, '0');
        for (std::size_t i = 0; i < n; ++i) id[i] = protocol::id_alphabet[random[i] % protocol::id_alphabet.size()];
        return id;
    }

    action_slot& slot_for(std::string_view name) {
        auto it = actions_.find(name);
        if (it == actions_.end()) it = actions_.try_emplace(std::string(name)).first;
        return it->second;
    }

    static bool is_internal_to_pending(std::string_view name) { return name == "@_leave" || name == "@_hsdata" || name == "@_hsready"; }

    bool may_receive(std::string_view peer, std::string_view name) const {
        auto it = peers_.find(peer);
        return it != peers_.end() && (it->second.active || is_internal_to_pending(name));
    }

    std::vector<std::string_view> peer_views(bool include_pending) const {
        std::vector<std::string_view> out;
        out.reserve(peers_.size());
        for (const auto& [id, p] : peers_) {
            if (p.active || include_pending) out.emplace_back(id);
        }
        return out;
    }

    std::optional<action_wire::link> link_to(std::string_view peer, bool include_pending) {
        auto it = peers_.find(peer);
        if (it == peers_.end() || (!it->second.active && !include_pending)) return std::nullopt;
        std::weak_ptr<peer_link> weak = it->second.link;
        return action_wire::link{
            .send = [weak](std::span<const std::uint8_t> frame) { auto l = weak.lock(); return l && l->send(frame); },
            .buffered_amount = [weak] { auto l = weak.lock(); return l ? l->buffered_amount() : std::size_t{0}; },
            .wait_drained = [weak](std::function<void(bool)> done) {
                if (auto l = weak.lock()) return l->wait_drained(std::move(done));
                done(false);
            },
        };
    }

    /** An internal action, inline and to one peer: the text is written into the frame during the call. */
    void send_internal(std::string_view name, std::string_view peer, protocol::payload_format format = protocol::payload_format::text,
                       std::string_view text = {}) {
        if (!wire_) return;
        const payload_ref ref{std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), nullptr};
        wire_->send(name, format, {}, ref, std::span(&peer, 1), true, [](boost::system::error_code) {});
    }

    void post_call(const std::shared_ptr<message_handler>& handler, incoming m) {
        boost::asio::post(ex_, [handler, m = std::move(m)]() mutable { (*handler)(m); });
    }

    void report(std::string_view peer, boost::system::error_code ec, std::string_view what) {
        log_("room", [&] { return "peer " + std::string(peer) + ": " + std::string(what); });
        if (on_error_) boost::asio::post(ex_, [f = on_error_, peer = std::string(peer), ec, what = std::string(what)] { f(peer, ec, what); });
    }

    void on_connected(std::string_view peer_view, std::shared_ptr<backend::peer_connection> pc, std::shared_ptr<backend::data_channel> channel) {
        if (left_) {
            channel->close();
            pc->close();
            return;
        }
        auto weak = weak_from_this();
        auto it = peers_.try_emplace(std::string(peer_view)).first;
        const std::string& peer = it->first;
        auto& state = it->second;
        state.link = std::make_shared<peer_link>(factory_, peer, token_, std::move(pc), std::move(channel), peer_link::callbacks{
            .frame = [weak](std::string_view p, backend::bytes&& frame, std::span<const std::uint8_t> action) {
                if (auto self = weak.lock()) self->wire_->handle(p, std::move(frame), action);
            },
            .closed = [weak](std::string_view p, std::string_view reason) { if (auto self = weak.lock()) self->remove_peer(p, reason, false); },
        }, log_);
        state.handshake_timer = factory_.make_timer();
        state.handshake_timer->start(config_.handshake_timeout, [weak, peer] {
            auto self = weak.lock();
            if (!self) return;
            self->report(peer, make_error_code(error::handshake_timed_out), "handshake timed out");
            self->kick(peer, "handshake timed out");
        });
        state.link->start();
        start_handshake(peer, state);
    }

    void start_handshake(std::string_view peer, peer_state& state) {
        if (config_.password.empty()) return pass_local(peer);
        if (config_.self_id < peer) {
            state.challenge = random_id(36);
            std::string text = R"({"__trystero_pw":"challenge","c":)";
            protocol::json::append_string(text, state.challenge);
            text += '}';
            send_internal("@_hsdata", peer, protocol::payload_format::json, text);
        }
    }

    void on_handshake_data(std::string_view peer, std::string_view data) {
        auto it = peers_.find(peer);
        if (it == peers_.end() || it->second.active || it->second.local_passed) return;
        boost::system::error_code ec;
        const auto value = boost::json::parse(boost::json::string_view(data.data(), data.size()), ec);
        const auto* o = ec ? nullptr : value.if_object();
        auto field = [o](std::string_view key) -> std::string_view {
            const auto* v = o != nullptr ? o->if_contains(key) : nullptr;
            const auto* s = v != nullptr ? v->if_string() : nullptr;
            return s != nullptr ? std::string_view(s->data(), s->size()) : std::string_view();
        };
        if (config_.self_id < peer) {
            if (field("__trystero_pw") != "response" ||
                field("h") != protocol::handshake_proof(it->second.challenge, config_.password, config_.app_id, room_id_)) {
                return fail_handshake(peer);
            }
        } else {
            if (field("__trystero_pw") != "challenge" || o->if_contains("c") == nullptr) return fail_handshake(peer);
            std::string text = R"({"__trystero_pw":"response","h":)";
            protocol::json::append_string(text, protocol::handshake_proof(field("c"), config_.password, config_.app_id, room_id_));
            text += '}';
            send_internal("@_hsdata", peer, protocol::payload_format::json, text);
        }
        pass_local(peer);
    }

    void fail_handshake(std::string_view peer) {
        report(peer, make_error_code(error::wrong_password), "incorrect password for overlapping room");
        kick(peer, "handshake failed");
    }

    void pass_local(std::string_view peer) {
        auto it = peers_.find(peer);
        if (it == peers_.end()) return;
        it->second.local_passed = true;
        send_internal("@_hsready", it->first);
        maybe_activate(it->first);
    }

    void maybe_activate(std::string_view peer) {
        auto it = peers_.find(peer);
        if (it == peers_.end() || it->second.active || !it->second.local_passed || !it->second.remote_ready) return;
        it->second.active = true;
        it->second.handshake_timer.reset();
        log_("room", [&] { return "peer joined: " + it->first; });
        if (on_join_) boost::asio::post(ex_, [f = on_join_, peer = it->first] { f(peer); });
    }

    void on_incoming(incoming&& m) {
        const auto name = m.name();
        auto it_peer = peers_.find(m.peer);
        if (name == "@_hsready") {
            if (it_peer != peers_.end() && !it_peer->second.active) {
                it_peer->second.remote_ready = true;
                maybe_activate(it_peer->first);
            }
        } else if (name == "@_hsdata") {
            if (it_peer != peers_.end()) on_handshake_data(it_peer->first, as_text(m.payload()));
        } else if (name == "@_ping") {
            if (it_peer != peers_.end()) send_internal("@_pong", it_peer->first);
        } else if (name == "@_pong") {
            if (it_peer != peers_.end() && !it_peer->second.pings.empty()) {
                auto ping = std::move(it_peer->second.pings.front());
                it_peer->second.pings.pop_front();
                const auto elapsed = std::chrono::milliseconds(static_cast<long long>(factory_.now_ms() - ping.started_ms));
                complete(std::move(ping.handler), boost::system::error_code{}, elapsed);
            }
        } else if (name == "@_leave") {
            remove_peer(m.peer, "peer left the room", true);
        } else if (name.starts_with("@_")) {
            // Media and renegotiation actions: not implemented yet (the documentation's roadmap).
        } else if (auto it = actions_.find(name); it != actions_.end()) {
            auto& slot = it->second;
            if (slot.handler) {
                post_call(slot.handler, std::move(m));
            } else if (!slot.receivers.empty()) {
                auto receiver = std::move(slot.receivers.front());
                slot.receivers.pop_front();
                complete(std::move(receiver), boost::system::error_code{}, std::move(m));
            } else {
                if (slot.queue.size() >= max_queued_messages) {
                    log_("action", [&] { return "dropping the oldest unread message of " + it->first; });
                    slot.queue.pop_front();
                }
                slot.queue.push_back(std::move(m));
            }
        }
    }

    void kick(std::string_view peer, std::string_view reason) {
        auto it = peers_.find(peer);
        if (it == peers_.end()) return;
        send_internal("@_leave", it->first);
        remove_peer(it->first, reason, true);
    }

    void remove_peer(std::string_view peer_view, std::string_view reason, bool close_link) {
        auto it = peers_.find(peer_view);
        if (it == peers_.end()) return;
        // The id is moved out of the map before the entry goes: `peer_view` may refer to it.
        auto node = peers_.extract(it);
        const std::string peer = std::move(node.key());
        auto& state = node.mapped();
        log_("room", [&] { return "peer " + peer + " gone: " + std::string(reason); });
        if (close_link) state.link->close();
        for (auto& ping : state.pings) complete(std::move(ping.handler), make_error_code(error::peer_disconnected), std::chrono::milliseconds{});
        wire_->clear_peer(peer, make_error_code(error::peer_disconnected));
        if (left_) return;
        signaling_->forget(peer);
        strategy_->restart_announcements();
        if (state.active && on_leave_) boost::asio::post(ex_, [f = on_leave_, peer] { f(peer); });
    }

    void close_everything() {
        leave_timer_.reset();
        auto peers = std::move(peers_);
        peers_.clear();
        for (auto& [id, state] : peers) {
            state.link->close();
            for (auto& ping : state.pings) complete(std::move(ping.handler), make_error_code(error::room_left), std::chrono::milliseconds{});
            if (wire_) wire_->clear_peer(id, make_error_code(error::room_left));
        }
        for (auto& [name, slot] : actions_) {
            for (auto& receiver : slot.receivers) complete(std::move(receiver), make_error_code(error::room_left), incoming{});
            slot.receivers.clear();
        }
        if (signaling_) signaling_->close();
        if (strategy_) strategy_->stop();
    }

    executor_type ex_;
    backend::factory& factory_;
    room_config config_;
    std::string room_id_;
    logger log_;
    std::string token_;
    std::string root_topic_;
    std::string self_topic_;
    std::shared_ptr<backend::sealer> sealer_;
    std::shared_ptr<action_wire> wire_;
    std::shared_ptr<signaling> signaling_;
    std::shared_ptr<nostr_strategy> strategy_;
    std::map<std::string, peer_state, std::less<>> peers_;
    std::map<std::string, action_slot, std::less<>> actions_;
    std::function<void(std::string_view)> on_join_;
    std::function<void(std::string_view)> on_leave_;
    std::function<void(std::string_view, boost::system::error_code, std::string_view)> on_error_;
    std::unique_ptr<backend::timer> leave_timer_;
    bool left_ = false;
};

}  // namespace detail

template <class T>
class action;

/**
 * A room joined over Trystero's Nostr strategy. Constructing it joins; `async_leave` leaves with Trystero's farewell,
 * and destroying it leaves at once.
 *
 * Tip: every handler the room calls, and every completion, runs on the executor it was constructed with. The views a
 * handler receives are valid for the duration of the call.
 */
class room {
public:
    using executor_type = boost::asio::any_io_executor;

    room(executor_type ex, backend::factory& factory, room_config config, std::string_view room_id)
        : impl_(std::make_shared<detail::room_impl>(std::move(ex), factory, std::move(config), room_id)) {
        impl_->start();
    }

    room(const room&) = delete;
    room& operator=(const room&) = delete;
    room(room&&) noexcept = default;
    room& operator=(room&&) noexcept = default;

    ~room() {
        if (impl_) impl_->abandon();
    }

    const executor_type& get_executor() const { return impl_->get_executor(); }

    std::string_view self_id() const { return impl_->self_id(); }

    /** The Nostr relays this room uses, derived from the app id unless the configuration named them. */
    std::span<const std::string> relay_urls() const { return impl_->relay_urls(); }

    /** The peers whose handshake completed. */
    std::vector<peer_id> peers() const { return impl_->active_peers(); }

    void on_peer_join(std::function<void(std::string_view)> f) { impl_->on_peer_join(std::move(f)); }
    void on_peer_leave(std::function<void(std::string_view)> f) { impl_->on_peer_leave(std::move(f)); }
    /** Problems with one peer that the room handled itself: a wrong password, a timed-out handshake. */
    void on_error(std::function<void(std::string_view peer, boost::system::error_code, std::string_view what)> f) { impl_->on_error(std::move(f)); }

    /** An action of this room; `T` is one of the types detail::payload_traits lists. */
    template <class T>
    action<T> make_action(std::string_view name);

    /**
     * Measures the round trip to a peer. Completes with `(error_code, std::chrono::milliseconds)`. The id is copied at
     * the call, so a deferred operation does not depend on the caller's string.
     */
    template <class CompletionToken>
    auto async_ping(std::string_view peer, CompletionToken&& token) {
        return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code, std::chrono::milliseconds)>(
            [impl = impl_](auto handler, const peer_id& peer) { impl->ping(peer, std::move(handler)); }, token, peer_id(peer));
    }

    /** Says goodbye to every peer and closes everything. Completes with `(error_code)`. */
    template <class CompletionToken>
    auto async_leave(CompletionToken&& token) {
        return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
            [impl = impl_](auto handler) { impl->leave(std::move(handler)); }, token);
    }

private:
    std::shared_ptr<detail::room_impl> impl_;
};

/**
 * A named channel of messages of type `T` within a room, wire compatible with Trystero's `makeAction`. Messages
 * arrive at the handler `on_message` set, or, without one, queue for `async_receive`.
 */
template <class T>
class action {
public:
    std::string_view name() const { return name_; }

    /** The handler for every message; with a view type, the view is valid for the duration of the call. */
    void on_message(std::function<void(T, const message_info&)> handler) {
        impl_->set_action_handler(name_, [handler = std::move(handler)](detail::incoming& m) {
            // The metadata is read before decoding: a binary payload takes the buffer the metadata lives in.
            const message_info info{std::move(m.peer), std::string(m.metadata())};
            if (auto value = detail::payload_traits<T>::decode(m)) handler(std::move(*value), info);
        });
    }

    /**
     * Sends to `options.targets`, or every active peer. The value is taken at the call (moved when `T` owns its
     * storage). Completes with `(error_code)` once every target has it.
     */
    template <class CompletionToken>
    auto async_send(T value, send_options options, CompletionToken&& token) {
        return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
            [impl = impl_, name = name_](auto handler, detail::encoded_payload payload, send_options options) {
                impl->send(name, std::move(payload), std::move(options), std::move(handler));
            },
            token, detail::payload_traits<T>::encode(std::move(value)), std::move(options));
    }

    template <class CompletionToken>
    auto async_send(T value, CompletionToken&& token) {
        return async_send(std::move(value), send_options{}, std::forward<CompletionToken>(token));
    }

    /**
     * The next message, when no handler is set. Completes with `(error_code, T, message_info)`; a message of another
     * payload format than `T` completes with `error::transfer_rejected` and an empty value.
     */
    template <class CompletionToken>
    auto async_receive(CompletionToken&& token) {
        static_assert(detail::payload_traits<T>::owning, "a received view would outlive the message: await an owning type");
        return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code, T, message_info)>(
            [impl = impl_, name = name_](auto handler) {
                auto shared = std::make_shared<decltype(handler)>(std::move(handler));
                impl->receive(name, [impl, shared](boost::system::error_code ec, detail::incoming m) {
                    message_info info{std::move(m.peer), std::string(m.metadata())};
                    auto value = ec ? std::nullopt : detail::payload_traits<T>::decode(m);
                    if (!ec && !value) ec = make_error_code(error::transfer_rejected);
                    impl->complete(std::move(*shared), ec, value ? std::move(*value) : T{}, std::move(info));
                });
            },
            token);
    }

private:
    friend class room;
    action(std::shared_ptr<detail::room_impl> impl, std::string_view name) : impl_(std::move(impl)), name_(name) { impl_->add_action(name_); }

    std::shared_ptr<detail::room_impl> impl_;
    std::string name_;
};

template <class T>
action<T> room::make_action(std::string_view name) {
    return action<T>(impl_, name);
}

}  // namespace webcpp::trystero

#endif
