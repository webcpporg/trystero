// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_TEST_MEMORY_BACKEND_HPP
#define WEBCPP_TRYSTERO_TEST_MEMORY_BACKEND_HPP

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/json_resource.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/nostr.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/text.hpp>

#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/json/storage_ptr.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <memory_resource>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * A backend in memory, for running whole rooms in one process: Nostr relays that forward ephemeral
 * events to matching subscriptions, peer connections that pair through their descriptions, data
 * channels that deliver in order, and a sealer that only opens what was sealed under the same key.
 * Every event is posted to the io_context, never delivered from inside the call that caused it, as
 * in a browser.
 *
 * Tip: what a room makes, and what a room receives, is allocated in that room's resource, which
 * each factory method is given; the network's own state (its relays, their subscriptions, its
 * record of the connections) is in the resource the network is constructed with, and changes only
 * when a room joins, connects or leaves.
 */
namespace webcpp::trystero::test {

class memory_network;

/** Posts `f` to `io`, allocated in `memory`. */
template <class F>
void post_in(boost::asio::io_context& io, std::pmr::memory_resource* memory, F&& f) {
    boost::asio::post(io, boost::asio::bind_allocator(std::pmr::polymorphic_allocator<>(memory),
                                                      std::forward<F>(f)));
}

/** An Asio timer, as the native backend's: a wait cancelled, replaced or destroyed never fires,
 * even when Asio had already queued its expiry. */
class memory_timer : public backend::timer {
public:
    memory_timer(boost::asio::io_context& io, std::pmr::memory_resource* memory, int time_scale = 1)
        : timer_(io),
          memory_(memory),
          time_scale_(time_scale),
          waits_(
              std::allocate_shared<std::uint64_t>(std::pmr::polymorphic_allocator<>(memory), 0)) {}

    void start(std::chrono::milliseconds delay, handler<void()> fire) override {
        const std::uint64_t wait = ++*waits_;
        timer_.expires_after(delay / time_scale_);
        timer_.async_wait(boost::asio::bind_allocator(
            std::pmr::polymorphic_allocator<>(memory_),
            [waits = std::weak_ptr<std::uint64_t>(waits_), wait,
             fire = std::move(fire)](boost::system::error_code ec) mutable {
                if (const auto current = waits.lock(); !ec && current && *current == wait) {
                    fire();
                }
            }));
    }

    void cancel() override {
        ++*waits_;
        timer_.cancel();
    }

private:
    boost::asio::steady_timer timer_;
    std::pmr::memory_resource* memory_;
    int time_scale_;
    std::shared_ptr<std::uint64_t> waits_;
};

class memory_websocket;

/** A NIP-01 relay holding nothing: events reach only the subscriptions open when they arrive, as
 * for ephemeral kinds. */
class memory_relay {
public:
    // Its implicit move assignment may allocate: a std::pmr member copies when the two resources
    // differ, which bugprone-exception-escape, expecting every move assignment not to throw,
    // reports.
    struct subscription {  // NOLINT(bugprone-exception-escape)
        std::weak_ptr<memory_websocket> socket;
        std::pmr::set<std::int64_t> kinds;
        std::pmr::set<std::pmr::string, std::less<>> topics;
    };

    explicit memory_relay(std::pmr::memory_resource* memory)
        : memory_(memory), subscriptions_(memory) {}

    void receive(const std::shared_ptr<memory_websocket>& from, std::string_view text);

    void drop(const memory_websocket* socket) {
        for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
            const auto s = it->second.socket.lock();
            it = (!s || s.get() == socket) ? subscriptions_.erase(it) : std::next(it);
        }
    }

    /** Closes every connection that has a subscription, as a relay that goes away does. */
    void close_all();

    std::size_t events_seen = 0;
    /** Answers this many EVENTs with "rate-limited:", to exercise the strategy's backoff. */
    int rate_limit_next = 0;
    /** Closes each connection right after its subscription, as a relay does after a text longer
     * than a room reads. */
    bool close_after_subscribe = false;

private:
    using key = std::pair<const memory_websocket*, std::pmr::string>;

    std::pmr::memory_resource* memory_;
    std::pmr::map<key, subscription> subscriptions_;
};

class memory_websocket : public backend::websocket,
                         public std::enable_shared_from_this<memory_websocket> {
public:
    memory_websocket(boost::asio::io_context& io, memory_relay* relay, handlers h,
                     std::pmr::memory_resource* memory)
        : io_(io), relay_(relay), h_(std::move(h)), memory_(memory), json_(memory) {}

    std::pmr::memory_resource* memory() const { return memory_; }

    /** A storage over this socket's resource, for a value parsed and dropped during one call. */
    boost::json::storage_ptr json() { return {&json_}; }

    void open() {
        post_in(io_, memory_, [self = shared_from_this()] {
            if (self->closed_) {
                return;
            }
            if (!self->relay_) {
                self->finish();
                return;
            }
            self->open_ = true;
            if (self->h_.open) {
                self->h_.open();
            }
        });
    }

    void send(std::string_view text) override {
        if (!open_) {
            return;
        }
        post_in(io_, memory_, [self = shared_from_this(), text = std::pmr::string(text, memory_)] {
            if (self->open_ && self->relay_) {
                self->relay_->receive(self, text);
            }
        });
    }

    /** Delivers a text the relay made in this socket's resource. */
    void deliver(std::pmr::string text) {
        post_in(io_, memory_, [self = shared_from_this(), text = std::move(text)] {
            if (self->open_ && self->h_.message) {
                self->h_.message(text);
            }
        });
    }

    void close() override {
        if (closed_) {
            return;
        }
        closed_ = true;
        open_ = false;
        if (relay_ != nullptr) {
            relay_->drop(this);
        }
        post_in(io_, memory_, [self = shared_from_this()] { self->finish(); });
    }

private:
    void finish() {
        open_ = false;
        if (auto close = std::exchange(h_.close, {})) {
            close();
        }
    }

    boost::asio::io_context& io_;
    memory_relay* relay_;
    handlers h_;
    std::pmr::memory_resource* memory_;
    json_resource json_;
    bool open_ = false;
    bool closed_ = false;
};

inline void memory_relay::close_all() {
    std::vector<std::shared_ptr<memory_websocket>> sockets;
    for (const auto& [k, s] : subscriptions_) {
        if (auto socket = s.socket.lock()) {
            sockets.push_back(std::move(socket));
        }
    }
    for (const auto& socket : sockets) {
        socket->close();
    }
}

inline void memory_relay::receive(const std::shared_ptr<memory_websocket>& from,
                                  std::string_view text) {
    boost::system::error_code ec;
    const auto value = protocol::json::parse(text, from->json(), ec);
    const auto* message = ec ? nullptr : value.if_array();
    if (message == nullptr || message->empty() || !message->at(0).is_string()) {
        return;
    }
    const std::string_view type = message->at(0).get_string();
    if (type == "REQ" && message->size() >= 3) {
        subscription s{
            .socket = from,
            .kinds = std::pmr::set<std::int64_t>(memory_),
            .topics = std::pmr::set<std::pmr::string, std::less<>>(memory_),
        };
        const auto& filter = message->at(2).as_object();
        for (const auto& k : filter.at("kinds").as_array()) {
            s.kinds.insert(k.as_int64());
        }
        for (const auto& t : filter.at("#x").as_array()) {
            s.topics.emplace(std::string_view(t.as_string()));
        }
        subscriptions_.insert_or_assign(
            key{from.get(),
                std::pmr::string(std::string_view(message->at(1).as_string()), memory_)},
            std::move(s));
        std::pmr::string eose(R"(["EOSE",)", from->memory());
        protocol::json::append_value(eose, message->at(1));
        eose += ']';
        from->deliver(std::move(eose));
        if (close_after_subscribe) {
            from->close();
        }
    } else if (type == "CLOSE" && message->size() >= 2) {
        subscriptions_.erase(
            key{from.get(),
                std::pmr::string(std::string_view(message->at(1).as_string()), from->memory())});
    } else if (type == "EVENT" && message->size() >= 2) {
        const auto& event = message->at(1).as_object();
        std::pmr::string ok(R"(["OK",)", from->memory());
        protocol::json::append_value(ok, event.at("id"));
        if (rate_limit_next > 0) {
            --rate_limit_next;
            ok += R"(,false,"rate-limited: slow down"])";
            from->deliver(std::move(ok));
            return;
        }
        ++events_seen;
        ok += R"(,true,""])";
        from->deliver(std::move(ok));
        const auto kind = event.at("kind").as_int64();
        // The event's first tag, ["x", <topic>].
        const auto& tags = event.at("tags").as_array();
        const auto& tag = tags.at(0).as_array();
        const std::string_view topic = tag.at(1).as_string();
        for (auto& [subscribed, s] : subscriptions_) {
            const auto socket = s.socket.lock();
            if (!socket || !s.kinds.contains(kind) || !s.topics.contains(topic)) {
                continue;
            }
            // ["EVENT", <subscription>, <event>], as boost::json::serialize writes it, in the
            // receiver's resource.
            std::pmr::string forwarded(R"(["EVENT",)", socket->memory());
            protocol::json::append_string(forwarded, subscribed.second);
            forwarded += ',';
            protocol::json::append_value(forwarded, message->at(1));
            forwarded += ']';
            socket->deliver(std::move(forwarded));
        }
    }
}

class memory_peer_connection;

/**
 * The network's record of its peer connections, by id: a connection enters it when it is made
 * and leaves it when it is destroyed, through a weak reference, since a connection may outlive
 * the network while the io_context it posted to still holds it.
 */
using connection_registry = std::pmr::map<int, memory_peer_connection*>;

class memory_data_channel : public backend::data_channel,
                            public std::enable_shared_from_this<memory_data_channel> {
public:
    memory_data_channel(boost::asio::io_context& io, std::pmr::memory_resource* memory)
        : io_(io), memory_(memory) {}

    void set_handlers(handlers h) override { h_ = std::move(h); }

    bool is_open() const override { return open_; }

    std::size_t buffered_amount() const override { return 0; }

    /** The frame arrives at the other end as a copy in the other end's resource, as a received
     * frame is. */
    void send(std::span<const std::uint8_t> frame) override {
        const auto remote = remote_.lock();
        if (!open_ || !remote) {
            return;
        }
        bytes_sent += frame.size();
        post_in(
            io_, remote->memory_,
            [remote, data = backend::bytes(frame.begin(), frame.end(), remote->memory_)]() mutable {
                if (remote->open_ && remote->h_.message) {
                    remote->h_.message(std::move(data));
                }
            });
    }

    void close() override {
        if (!open_) {
            return;
        }
        open_ = false;
        post_in(io_, memory_, [self = shared_from_this(), remote = remote_.lock()] {
            if (auto h = std::exchange(self->h_.close, {})) {
                h();
            }
            if (remote && remote->open_) {
                remote->open_ = false;
                if (auto h = std::exchange(remote->h_.close, {})) {
                    h();
                }
            }
        });
    }

    void pair_with(const std::shared_ptr<memory_data_channel>& remote) { remote_ = remote; }

    void opened() {
        open_ = true;
        if (h_.open) {
            h_.open();
        }
    }

    std::size_t bytes_sent = 0;

private:
    boost::asio::io_context& io_;
    std::pmr::memory_resource* memory_;
    std::weak_ptr<memory_data_channel> remote_;
    handlers h_;
    bool open_ = false;
};

/** Peer connections pair through their descriptions: "mem:<id>" names the connection that wrote it.
 */
class memory_peer_connection : public backend::peer_connection,
                               public std::enable_shared_from_this<memory_peer_connection> {
public:
    memory_peer_connection(boost::asio::io_context& io, memory_network& net,
                           std::weak_ptr<connection_registry> registry, int id,
                           std::pmr::memory_resource* memory)
        : io_(io),
          net_(net),
          registry_(std::move(registry)),
          id_(id),
          memory_(memory),
          candidates_received(memory) {}

    memory_peer_connection(const memory_peer_connection&) = delete;
    memory_peer_connection& operator=(const memory_peer_connection&) = delete;

    /** Leaves the network's record, if the network is still there. */
    ~memory_peer_connection() override;

    void set_handlers(handlers h) override { h_ = std::move(h); }

    std::shared_ptr<backend::data_channel> create_data_channel(
        std::string_view /*label*/) override {
        channel_ = std::allocate_shared<memory_data_channel>(
            std::pmr::polymorphic_allocator<>(memory_), io_, memory_);
        return channel_;
    }

    void set_local_description(
        handler<void(backend::error_code, std::string_view, std::string_view)> done) override {
        post_in(io_, memory_, [self = shared_from_this(), done = std::move(done)]() mutable {
            if (self->closed_) {
                done(make_error_code(error::backend_failure), {}, {});
                return;
            }
            self->local_set_ = true;
            std::pmr::string sdp("mem:", self->memory_);
            protocol::json::append_integer(sdp, self->id_);
            done({}, self->remote_id_ < 0 ? "offer" : "answer", sdp);
            post_in(self->io_, self->memory_, [self] {
                if (self->closed_ || !self->h_.ice_candidate) {
                    return;
                }
                std::pmr::string candidate(R"({"candidate":"candidate:mem )", self->memory_);
                protocol::json::append_integer(candidate, self->id_);
                candidate += R"(","sdpMid":"0","sdpMLineIndex":0})";
                self->h_.ice_candidate(candidate);
            });
            self->try_connect();
        });
    }

    void set_remote_description(std::string_view /*type*/, std::string_view sdp_view,
                                handler<void(backend::error_code)> done) override {
        post_in(io_, memory_,
                [self = shared_from_this(), sdp = std::pmr::string(sdp_view, memory_),
                 done = std::move(done)]() mutable {
                    int remote = -1;
                    const bool ours =
                        sdp.starts_with("mem:") &&
                        std::from_chars(sdp.data() + 4, sdp.data() + sdp.size(), remote).ec ==
                            std::errc{};
                    if (self->closed_ || !ours) {
                        done(make_error_code(error::backend_failure));
                        return;
                    }
                    self->remote_id_ = remote;
                    done({});
                    self->try_connect();
                });
    }

    void add_ice_candidate(std::string_view candidate,
                           handler<void(backend::error_code)> done) override {
        candidates_received.emplace_back(candidate);
        post_in(io_, memory_, [done = std::move(done)]() mutable { done({}); });
    }

    bool has_remote_description() const override { return remote_id_ >= 0; }

    void close() override {
        if (closed_) {
            return;
        }
        closed_ = true;
        if (channel_) {
            channel_->close();
        }
    }

    int id() const { return id_; }

    int remote_id() const { return remote_id_; }

    bool ready() const { return local_set_ && remote_id_ >= 0 && !closed_; }

    void connect_to(memory_peer_connection& answerer);

private:
    void try_connect();

    boost::asio::io_context& io_;
    memory_network& net_;
    std::weak_ptr<connection_registry> registry_;
    int id_;
    std::pmr::memory_resource* memory_;
    int remote_id_ = -1;
    bool local_set_ = false;
    bool closed_ = false;
    bool connected_ = false;
    handlers h_;
    std::shared_ptr<memory_data_channel> channel_;

public:
    std::pmr::vector<std::pmr::string> candidates_received;
};

/**
 * The factory: one per test, shared by every room in it. Its relays and its record of the
 * connections are in `memory`; everything it makes for a room is in the resource the room gives.
 */
class memory_network : public backend::factory {
public:
    explicit memory_network(boost::asio::io_context& io, std::uint64_t seed = 1,
                            std::pmr::memory_resource* memory = std::pmr::get_default_resource())
        : io_(io),
          random_(seed),
          start_(std::chrono::steady_clock::now()),
          memory_(memory),
          relays_(memory),
          unreachable_(memory),
          connections_(std::allocate_shared<connection_registry>(
              std::pmr::polymorphic_allocator<>(memory))) {}

    memory_relay& relay(std::string_view url) {
        const auto it = relays_.find(url);
        return it != relays_.end()
                   ? it->second
                   : relays_.try_emplace(std::pmr::string(url, memory_), memory_).first->second;
    }

    std::shared_ptr<backend::websocket> connect(std::string_view url,
                                                backend::websocket::handlers h,
                                                std::pmr::memory_resource* memory) override {
        const auto it = relays_.find(url);
        auto* relay = it == relays_.end() || unreachable_.contains(url) ? nullptr : &it->second;
        auto socket = std::allocate_shared<memory_websocket>(
            std::pmr::polymorphic_allocator<>(memory), io_, relay, std::move(h), memory);
        socket->open();
        return socket;
    }

    void make_unreachable(std::string_view url) { unreachable_.emplace(url); }

    std::shared_ptr<backend::peer_connection> make_peer_connection(
        std::span<const backend::ice_server> servers, std::pmr::memory_resource* memory) override {
        if (record_servers) {
            for (const auto& server : servers) {
                for (const auto& url : server.urls) {
                    server_urls.emplace_back(url);
                }
            }
        }
        ++made_connections;
        if (refuse_peer_connections > 0) {
            --refuse_peer_connections;
            return nullptr;
        }
        auto pc = std::allocate_shared<memory_peer_connection>(
            std::pmr::polymorphic_allocator<>(memory), io_, *this, connections_, next_pc_++,
            memory);
        connections_->insert_or_assign(pc->id(), pc.get());
        peak_connections = std::max(peak_connections, connections_->size());
        return pc;
    }

    std::shared_ptr<backend::timer> make_timer(std::pmr::memory_resource* memory) override {
        return std::allocate_shared<memory_timer>(std::pmr::polymorphic_allocator<>(memory), io_,
                                                  memory, time_scale);
    }

    std::shared_ptr<backend::sealer> make_sealer(std::array<std::uint8_t, 32> key,
                                                 std::pmr::memory_resource* memory) override;

    void random(std::span<std::uint8_t> out) override {
        for (auto& b : out) {
            b = static_cast<std::uint8_t>(random_());
        }
    }

    std::int64_t unix_seconds() override {
        return 1'759'700'000 + static_cast<std::int64_t>(now_ms() / 1000);
    }

    double now_ms() override {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
                   .count() *
               time_scale;
    }

    std::shared_ptr<memory_peer_connection> find(int id) {
        const auto it = connections_->find(id);
        return it == connections_->end() ? nullptr : it->second->shared_from_this();
    }

    boost::asio::io_context& io() { return io_; }

    /** How many peer connections are open now. */
    [[nodiscard]] std::size_t live_connections() const { return connections_->size(); }

    /** How many peer connections were asked for, refused ones included: what a browser counts. */
    std::size_t made_connections = 0;
    /** The most peer connections that were open at once. */
    std::size_t peak_connections = 0;
    /** How much faster than real time the timers and the clock run, for tests of long timeouts. */
    int time_scale = 1;
    /** How many of the next peer connections to refuse, as a browser at its limit does. */
    int refuse_peer_connections = 0;
    /** Whether to record the URL of every ICE server a peer connection is made with. */
    bool record_servers = false;
    /** The URLs recorded, in the default resource: only a test that records them allocates. */
    std::pmr::vector<std::pmr::string> server_urls;

private:
    boost::asio::io_context& io_;
    std::mt19937_64 random_;
    std::chrono::steady_clock::time_point start_;
    std::pmr::memory_resource* memory_;
    std::pmr::map<std::pmr::string, memory_relay, std::less<>> relays_;
    std::pmr::set<std::pmr::string, std::less<>> unreachable_;
    std::shared_ptr<connection_registry> connections_;
    int next_pc_ = 0;
};

inline memory_peer_connection::~memory_peer_connection() {
    if (const auto registry = registry_.lock()) {
        registry->erase(id_);
    }
}

inline void memory_peer_connection::try_connect() {
    if (connected_ || !ready()) {
        return;
    }
    const auto remote = net_.find(remote_id_);
    if (!remote || !remote->ready() || remote->remote_id() != id_) {
        return;
    }
    // The offerer created the data channel; the answerer receives its end.
    memory_peer_connection& offerer = channel_ ? *this : *remote;
    memory_peer_connection& answerer = channel_ ? *remote : *this;
    if (!offerer.channel_) {
        return;
    }
    offerer.connect_to(answerer);
}

inline void memory_peer_connection::connect_to(memory_peer_connection& answerer) {
    connected_ = answerer.connected_ = true;
    const auto offer_end = channel_;
    const auto answer_end = std::allocate_shared<memory_data_channel>(
        std::pmr::polymorphic_allocator<>(answerer.memory_), io_, answerer.memory_);
    answerer.channel_ = answer_end;
    offer_end->pair_with(answer_end);
    answer_end->pair_with(offer_end);
    post_in(
        io_, memory_,
        [self = shared_from_this(), other = answerer.shared_from_this(), offer_end, answer_end] {
            for (auto* pc : {self.get(), other.get()}) {
                if (pc->h_.state) {
                    pc->h_.state(backend::connection_state::connected);
                }
            }
            if (other->h_.data_channel) {
                other->h_.data_channel(answer_end);
            }
            post_in(self->io_, self->memory_, [offer_end, answer_end] {
                offer_end->opened();
                answer_end->opened();
            });
        });
}

/** Seals as "<iv>$<base64(plaintext)>" whose IV is the key's first 16 bytes: only the same key
 * opens it. */
class memory_sealer : public backend::sealer {
public:
    memory_sealer(boost::asio::io_context& io, std::array<std::uint8_t, 32> key,
                  std::pmr::memory_resource* memory)
        : io_(io), key_(key), memory_(memory) {}

    void seal(std::string_view plaintext,
              handler<void(backend::error_code, std::string_view)> done) override {
        auto text = protocol::format_sealed(
            std::span(key_).first(16),
            std::span(reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size()),
            memory_);
        post_in(io_, memory_,
                [text = std::move(text), done = std::move(done)]() mutable { done({}, text); });
    }

    void open(std::string_view sealed,
              handler<void(backend::error_code, std::string_view)> done) override {
        const auto parts = protocol::parse_sealed(sealed, memory_);
        const auto iv = std::span(key_).first(16);
        const bool ours =
            parts && std::equal(parts->iv.begin(), parts->iv.end(), iv.begin(), iv.end());
        std::pmr::string plain(memory_);
        if (ours) {
            plain.assign(parts->ciphertext.begin(), parts->ciphertext.end());
        }
        post_in(io_, memory_, [ours, plain = std::move(plain), done = std::move(done)]() mutable {
            done(ours ? backend::error_code{} : make_error_code(error::wrong_password), plain);
        });
    }

private:
    boost::asio::io_context& io_;
    std::array<std::uint8_t, 32> key_;
    std::pmr::memory_resource* memory_;
};

inline std::shared_ptr<backend::sealer> memory_network::make_sealer(
    std::array<std::uint8_t, 32> key, std::pmr::memory_resource* memory) {
    return std::allocate_shared<memory_sealer>(std::pmr::polymorphic_allocator<>(memory), io_, key,
                                               memory);
}

}  // namespace webcpp::trystero::test

#endif
