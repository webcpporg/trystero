// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_TEST_MEMORY_BACKEND_HPP
#define WEBCPP_TRYSTERO_TEST_MEMORY_BACKEND_HPP

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/protocol/nostr.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/text.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

/**
 * A backend in memory, for running whole rooms in one process: Nostr relays that forward ephemeral events to matching
 * subscriptions, peer connections that pair through their descriptions, data channels that deliver in order, and a
 * sealer that only opens what was sealed under the same key. Every event is posted to the io_context, never delivered
 * from inside the call that caused it, as in a browser.
 */
namespace webcpp::trystero::test {

class memory_network;

class memory_timer : public backend::timer {
public:
    explicit memory_timer(boost::asio::io_context& io) : timer_(io) {}

    void start(std::chrono::milliseconds delay, std::function<void()> fire) override {
        timer_.expires_after(delay);
        timer_.async_wait([fire = std::move(fire)](boost::system::error_code ec) {
            if (!ec) fire();
        });
    }

    void cancel() override { timer_.cancel(); }

private:
    boost::asio::steady_timer timer_;
};

class memory_websocket;

/** A NIP-01 relay holding nothing: events reach only the subscriptions open when they arrive, as for ephemeral kinds. */
class memory_relay {
public:
    struct subscription {
        std::weak_ptr<memory_websocket> socket;
        std::set<std::int64_t> kinds;
        std::set<std::string> topics;
    };

    void receive(const std::shared_ptr<memory_websocket>& from, std::string_view text);
    void drop(const memory_websocket* socket) {
        for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
            auto s = it->second.socket.lock();
            it = (!s || s.get() == socket) ? subscriptions_.erase(it) : std::next(it);
        }
    }

    std::size_t events_seen = 0;
    /** Answers this many EVENTs with "rate-limited:", to exercise the strategy's backoff. */
    int rate_limit_next = 0;

private:
    std::map<std::pair<const memory_websocket*, std::string>, subscription> subscriptions_;
};

class memory_websocket : public backend::websocket, public std::enable_shared_from_this<memory_websocket> {
public:
    memory_websocket(boost::asio::io_context& io, memory_relay* relay, handlers h) : io_(io), relay_(relay), h_(std::move(h)) {}

    void open() {
        auto self = shared_from_this();
        boost::asio::post(io_, [self] {
            if (self->closed_) return;
            if (!self->relay_) return self->finish();
            self->open_ = true;
            if (self->h_.open) self->h_.open();
        });
    }

    void send(std::string_view text) override {
        if (!open_) return;
        auto self = shared_from_this();
        boost::asio::post(io_, [self, text = std::string(text)] {
            if (self->open_ && self->relay_) self->relay_->receive(self, text);
        });
    }

    void deliver(std::string text) {
        auto self = shared_from_this();
        boost::asio::post(io_, [self, text = std::move(text)] {
            if (self->open_ && self->h_.message) self->h_.message(text);
        });
    }

    void close() override {
        if (closed_) return;
        closed_ = true;
        open_ = false;
        if (relay_) relay_->drop(this);
        auto self = shared_from_this();
        boost::asio::post(io_, [self] { self->finish(); });
    }

private:
    void finish() {
        open_ = false;
        if (auto close = std::exchange(h_.close, {})) close();
    }

    boost::asio::io_context& io_;
    memory_relay* relay_;
    handlers h_;
    bool open_ = false;
    bool closed_ = false;
};

inline void memory_relay::receive(const std::shared_ptr<memory_websocket>& from, std::string_view text) {
    boost::system::error_code ec;
    const auto value = boost::json::parse(text, ec);
    const auto* message = ec ? nullptr : value.if_array();
    if (message == nullptr || message->empty() || !message->at(0).is_string()) return;
    const std::string_view type = message->at(0).get_string();
    if (type == "REQ" && message->size() >= 3) {
        subscription s{from, {}, {}};
        const auto& filter = message->at(2).as_object();
        for (const auto& k : filter.at("kinds").as_array()) s.kinds.insert(k.as_int64());
        for (const auto& t : filter.at("#x").as_array()) s.topics.insert(std::string(t.as_string()));
        subscriptions_[{from.get(), std::string(message->at(1).as_string())}] = std::move(s);
        from->deliver(boost::json::serialize(boost::json::array{"EOSE", message->at(1)}));
    } else if (type == "CLOSE" && message->size() >= 2) {
        subscriptions_.erase({from.get(), std::string(message->at(1).as_string())});
    } else if (type == "EVENT" && message->size() >= 2) {
        const auto& event = message->at(1).as_object();
        if (rate_limit_next > 0) {
            --rate_limit_next;
            from->deliver(boost::json::serialize(boost::json::array{"OK", event.at("id"), false, "rate-limited: slow down"}));
            return;
        }
        ++events_seen;
        from->deliver(boost::json::serialize(boost::json::array{"OK", event.at("id"), true, ""}));
        const auto kind = event.at("kind").as_int64();
        const auto topic = std::string(event.at("tags").as_array().at(0).as_array().at(1).as_string());
        for (auto& [key, s] : subscriptions_) {
            auto socket = s.socket.lock();
            if (!socket || !s.kinds.contains(kind) || !s.topics.contains(topic)) continue;
            socket->deliver(boost::json::serialize(boost::json::array{"EVENT", key.second, event}));
        }
    }
}

class memory_peer_connection;

class memory_data_channel : public backend::data_channel, public std::enable_shared_from_this<memory_data_channel> {
public:
    explicit memory_data_channel(boost::asio::io_context& io) : io_(io) {}

    void set_handlers(handlers h) override { h_ = std::move(h); }
    bool is_open() const override { return open_; }
    std::size_t buffered_amount() const override { return 0; }

    void send(std::span<const std::uint8_t> frame) override {
        auto remote = remote_.lock();
        if (!open_ || !remote) return;
        bytes_sent += frame.size();
        boost::asio::post(io_, [remote, data = backend::bytes(frame.begin(), frame.end())]() mutable {
            if (remote->open_ && remote->h_.message) remote->h_.message(std::move(data));
        });
    }

    void close() override {
        if (!open_) return;
        open_ = false;
        auto self = shared_from_this();
        auto remote = remote_.lock();
        boost::asio::post(io_, [self, remote] {
            if (auto h = std::exchange(self->h_.close, {})) h();
            if (remote && remote->open_) {
                remote->open_ = false;
                if (auto h = std::exchange(remote->h_.close, {})) h();
            }
        });
    }

    void pair_with(const std::shared_ptr<memory_data_channel>& remote) { remote_ = remote; }

    void opened() {
        open_ = true;
        if (h_.open) h_.open();
    }

    std::size_t bytes_sent = 0;

private:
    boost::asio::io_context& io_;
    std::weak_ptr<memory_data_channel> remote_;
    handlers h_;
    bool open_ = false;
};

/** Peer connections pair through their descriptions: "mem:<id>" names the connection that wrote it. */
class memory_peer_connection : public backend::peer_connection, public std::enable_shared_from_this<memory_peer_connection> {
public:
    memory_peer_connection(boost::asio::io_context& io, memory_network& net, int id) : io_(io), net_(net), id_(id) {}

    void set_handlers(handlers h) override { h_ = std::move(h); }

    std::shared_ptr<backend::data_channel> create_data_channel(std::string_view) override {
        channel_ = std::make_shared<memory_data_channel>(io_);
        return channel_;
    }

    void set_local_description(std::function<void(backend::error_code, std::string_view, std::string_view)> done) override {
        auto self = shared_from_this();
        boost::asio::post(io_, [self, done = std::move(done)] {
            if (self->closed_) return done(make_error_code(error::backend_failure), {}, {});
            self->local_set_ = true;
            done({}, self->remote_id_ < 0 ? "offer" : "answer", "mem:" + std::to_string(self->id_));
            boost::asio::post(self->io_, [self] {
                if (!self->closed_ && self->h_.ice_candidate) self->h_.ice_candidate(R"({"candidate":"candidate:mem )" + std::to_string(self->id_) + R"(","sdpMid":"0","sdpMLineIndex":0})");
            });
            self->try_connect();
        });
    }

    void set_remote_description(std::string_view, std::string_view sdp_view, std::function<void(backend::error_code)> done) override {
        auto self = shared_from_this();
        boost::asio::post(io_, [self, sdp = std::string(sdp_view), done = std::move(done)] {
            if (self->closed_ || !sdp.starts_with("mem:")) return done(make_error_code(error::backend_failure));
            self->remote_id_ = std::stoi(sdp.substr(4));
            done({});
            self->try_connect();
        });
    }

    void add_ice_candidate(std::string_view candidate, std::function<void(backend::error_code)> done) override {
        candidates_received.emplace_back(candidate);
        boost::asio::post(io_, [done = std::move(done)] { done({}); });
    }

    bool has_remote_description() const override { return remote_id_ >= 0; }

    void close() override {
        if (closed_) return;
        closed_ = true;
        if (channel_) channel_->close();
    }

    int id() const { return id_; }
    int remote_id() const { return remote_id_; }
    bool ready() const { return local_set_ && remote_id_ >= 0 && !closed_; }
    std::vector<std::string> candidates_received;

    void connect_to(memory_peer_connection& answerer);

private:
    void try_connect();

    boost::asio::io_context& io_;
    memory_network& net_;
    int id_;
    int remote_id_ = -1;
    bool local_set_ = false;
    bool closed_ = false;
    bool connected_ = false;
    handlers h_;
    std::shared_ptr<memory_data_channel> channel_;
};

/** The factory: one per test, shared by every room in it. */
class memory_network : public backend::factory {
public:
    explicit memory_network(boost::asio::io_context& io, std::uint64_t seed = 1) : io_(io), random_(seed), start_(std::chrono::steady_clock::now()) {}

    memory_relay& relay(std::string_view url) {
        auto it = relays_.find(url);
        return it != relays_.end() ? it->second : relays_.try_emplace(std::string(url)).first->second;
    }

    std::shared_ptr<backend::websocket> connect(std::string_view url, backend::websocket::handlers h) override {
        auto it = relays_.find(url);
        auto socket = std::make_shared<memory_websocket>(io_, it == relays_.end() || unreachable_.contains(url) ? nullptr : &it->second, std::move(h));
        socket->open();
        return socket;
    }

    void make_unreachable(std::string_view url) { unreachable_.emplace(url); }

    std::shared_ptr<backend::peer_connection> make_peer_connection(const std::vector<backend::ice_server>&) override {
        auto pc = std::make_shared<memory_peer_connection>(io_, *this, next_pc_++);
        connections_[pc->id()] = pc;
        return pc;
    }

    std::unique_ptr<backend::timer> make_timer() override { return std::make_unique<memory_timer>(io_); }

    std::shared_ptr<backend::sealer> make_sealer(std::array<std::uint8_t, 32> key) override;

    void random(std::span<std::uint8_t> out) override {
        for (auto& b : out) b = static_cast<std::uint8_t>(random_());
    }

    std::int64_t unix_seconds() override { return 1'759'700'000 + static_cast<std::int64_t>(now_ms() / 1000); }

    double now_ms() override {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    }

    std::shared_ptr<memory_peer_connection> find(int id) {
        auto it = connections_.find(id);
        return it == connections_.end() ? nullptr : it->second.lock();
    }

    boost::asio::io_context& io() { return io_; }

private:
    boost::asio::io_context& io_;
    std::mt19937_64 random_;
    std::chrono::steady_clock::time_point start_;
    std::map<std::string, memory_relay, std::less<>> relays_;
    std::set<std::string, std::less<>> unreachable_;
    std::map<int, std::weak_ptr<memory_peer_connection>> connections_;
    int next_pc_ = 0;
};

inline void memory_peer_connection::try_connect() {
    if (connected_ || !ready()) return;
    auto remote = net_.find(remote_id_);
    if (!remote || !remote->ready() || remote->remote_id() != id_) return;
    // The offerer created the data channel; the answerer receives its end.
    memory_peer_connection& offerer = channel_ ? *this : *remote;
    memory_peer_connection& answerer = channel_ ? *remote : *this;
    if (!offerer.channel_) return;
    offerer.connect_to(answerer);
}

inline void memory_peer_connection::connect_to(memory_peer_connection& answerer) {
    connected_ = answerer.connected_ = true;
    auto offer_end = channel_;
    auto answer_end = std::make_shared<memory_data_channel>(io_);
    answerer.channel_ = answer_end;
    offer_end->pair_with(answer_end);
    answer_end->pair_with(offer_end);
    auto self = shared_from_this();
    auto other = answerer.shared_from_this();
    boost::asio::post(io_, [self, other, offer_end, answer_end] {
        for (auto* pc : {self.get(), other.get()}) {
            if (pc->h_.state) pc->h_.state(backend::connection_state::connected);
        }
        if (other->h_.data_channel) other->h_.data_channel(answer_end);
        boost::asio::post(self->io_, [offer_end, answer_end] {
            offer_end->opened();
            answer_end->opened();
        });
    });
}

/** Seals as "<iv>$<base64(plaintext)>" whose IV is the key's first 16 bytes: only the same key opens it. */
class memory_sealer : public backend::sealer {
public:
    memory_sealer(boost::asio::io_context& io, std::array<std::uint8_t, 32> key) : io_(io), key_(key) {}

    void seal(std::string_view plaintext, std::function<void(backend::error_code, std::string_view)> done) override {
        const auto text = protocol::format_sealed(std::span(key_).first(16), std::span(reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size()));
        boost::asio::post(io_, [text, done = std::move(done)] { done({}, text); });
    }

    void open(std::string_view sealed, std::function<void(backend::error_code, std::string_view)> done) override {
        const auto parts = protocol::parse_sealed(sealed);
        const bool ours = parts && parts->iv == backend::bytes(key_.begin(), key_.begin() + 16);
        std::string plain = ours ? std::string(parts->ciphertext.begin(), parts->ciphertext.end()) : std::string();
        boost::asio::post(io_, [ours, plain = std::move(plain), done = std::move(done)] {
            done(ours ? backend::error_code{} : make_error_code(error::wrong_password), plain);
        });
    }

private:
    boost::asio::io_context& io_;
    std::array<std::uint8_t, 32> key_;
};

inline std::shared_ptr<backend::sealer> memory_network::make_sealer(std::array<std::uint8_t, 32> key) {
    return std::make_shared<memory_sealer>(io_, key);
}

}  // namespace webcpp::trystero::test

#endif
