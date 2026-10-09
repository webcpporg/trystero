// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_BROWSER_BACKEND_HPP
#define WEBCPP_TRYSTERO_BROWSER_BACKEND_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/browser/event_loop.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/protocol/signal.hpp>

#include <emscripten/val.h>
#include <sys/random.h>

#include <cmath>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/**
 * The browser target: the backend interface over the page's own `WebSocket`, `RTCPeerConnection`, `RTCDataChannel`,
 * `setTimeout` and `crypto.subtle`, through `emscripten::val`. It needs a secure context (https, or http on
 * localhost), where `crypto.subtle` exists.
 */
namespace webcpp::trystero::browser {

using emscripten::val;

namespace detail {

/** A copy of bytes as a new `Uint8Array`, independent of the wasm memory. */
inline val to_js(std::span<const std::uint8_t> data) {
    return val::global("Uint8Array").new_(val(emscripten::typed_memory_view(data.size(), data.data())));
}

/** A copy of an `ArrayBuffer` or typed array's bytes. */
inline backend::bytes from_js(const val& buffer) {
    const val view = val::global("Uint8Array").new_(buffer);
    backend::bytes out(view["length"].as<std::size_t>());
    val(emscripten::typed_memory_view(out.size(), out.data())).call<void>("set", view);
    return out;
}

/** Calls `done(true, value)` or `done(false, reason)` once a promise settles. */
inline void then(event_loop& loop, val promise, std::function<void(bool, val)> done) {
    auto id = std::make_shared<int>(0);
    *id = loop.add([&loop, id, done = std::move(done)](int slot, val value) {
        loop.remove(*id);
        done(slot == 0, std::move(value));
    });
    promise.call<void>("then", loop.handler(*id, 0), loop.handler(*id, 1));
}

}  // namespace detail

class timer : public backend::timer {
public:
    explicit timer(event_loop& loop) : loop_(loop), id_(loop.add([this](int, val) { on_fire(); })) {}

    ~timer() override {
        cancel();
        loop_.remove(id_);
    }

    void start(std::chrono::milliseconds delay, std::function<void()> fire) override {
        cancel();
        fire_ = std::move(fire);
        handle_ = val::global("setTimeout")(loop_.handler(id_, 0), val(static_cast<double>(delay.count())), val::undefined());
    }

    void cancel() override {
        if (handle_.isUndefined()) return;
        val::global("clearTimeout")(handle_);
        handle_ = val::undefined();
        fire_ = nullptr;
    }

private:
    void on_fire() {
        handle_ = val::undefined();
        auto fire = std::exchange(fire_, nullptr);
        if (fire) fire();  // may destroy this timer
    }

    event_loop& loop_;
    int id_;
    val handle_ = val::undefined();
    std::function<void()> fire_;
};

class websocket : public backend::websocket {
public:
    websocket(event_loop& loop, std::string_view url, handlers h)
        : loop_(loop), h_(std::move(h)), id_(loop.add([this](int slot, val event) { on_event(slot, std::move(event)); })) {
        socket_ = val::global("WebSocket").new_(loop_.text(url));
        socket_.set("onopen", loop_.handler(id_, 0));
        socket_.set("onmessage", loop_.handler(id_, 1));
        socket_.set("onclose", loop_.handler(id_, 2));
    }

    ~websocket() override {
        loop_.remove(id_);
        for (const char* name : {"onopen", "onmessage", "onclose"}) socket_.set(name, val::null());
        socket_.call<void>("close");
    }

    void send(std::string_view text) override {
        if (socket_["readyState"].as<int>() == 1) socket_.call<void>("send", loop_.text(text));
    }

    void close() override { socket_.call<void>("close"); }

private:
    void on_event(int slot, val event) {
        if (slot == 0) {
            if (auto f = h_.open) f();
        } else if (slot == 1) {
            const val data = event["data"];
            if (data.isString() && h_.message) {
                auto f = h_.message;
                const auto text = data.as<std::string>();  // the one copy: out of the JavaScript heap
                f(text);
            }
        } else if (auto f = std::exchange(h_.close, {})) {
            f();
        }
    }

    event_loop& loop_;
    handlers h_;
    int id_;
    val socket_ = val::undefined();
};

class data_channel : public backend::data_channel {
public:
    data_channel(event_loop& loop, val channel)
        : loop_(loop), channel_(std::move(channel)), id_(loop.add([this](int slot, val event) { on_event(slot, std::move(event)); })) {
        channel_.set("binaryType", val("arraybuffer"));
        channel_.set("bufferedAmountLowThreshold", val(static_cast<double>(low_threshold)));
        channel_.set("onopen", loop_.handler(id_, 0));
        channel_.set("onmessage", loop_.handler(id_, 1));
        channel_.set("onclose", loop_.handler(id_, 2));
        channel_.set("onbufferedamountlow", loop_.handler(id_, 3));
    }

    ~data_channel() override {
        loop_.remove(id_);
        for (const char* name : {"onopen", "onmessage", "onclose", "onbufferedamountlow"}) channel_.set(name, val::null());
        channel_.call<void>("close");
    }

    void set_handlers(handlers h) override { h_ = std::move(h); }

    bool is_open() const override { return channel_["readyState"].strictlyEquals(val("open")); }

    void send(std::span<const std::uint8_t> frame) override {
        if (is_open()) channel_.call<void>("send", detail::to_js(frame));
    }

    std::size_t buffered_amount() const override { return static_cast<std::size_t>(channel_["bufferedAmount"].as<double>()); }

    void close() override { channel_.call<void>("close"); }

private:
    void on_event(int slot, val event) {
        switch (slot) {
            case 0:
                if (auto f = h_.open) f();
                break;
            case 1:
                if (auto f = h_.message) f(detail::from_js(event["data"]));
                break;
            case 2:
                if (auto f = std::exchange(h_.close, {})) f();
                break;
            default:
                if (auto f = h_.buffered_amount_low) f();
        }
    }

    event_loop& loop_;
    val channel_;
    int id_;
    handlers h_;
};

class peer_connection : public backend::peer_connection, public std::enable_shared_from_this<peer_connection> {
public:
    peer_connection(event_loop& loop, const std::vector<backend::ice_server>& servers)
        : loop_(loop), id_(loop.add([this](int slot, val event) { on_event(slot, std::move(event)); })) {
        val list = val::array();
        for (const auto& s : servers) {
            val server = val::object();
            val urls = val::array();
            for (const auto& url : s.urls) urls.call<void>("push", val(url));
            server.set("urls", urls);
            if (!s.username.empty()) server.set("username", val(s.username));
            if (!s.credential.empty()) server.set("credential", val(s.credential));
            list.call<void>("push", server);
        }
        val config = val::object();
        config.set("iceServers", list);
        pc_ = val::global("RTCPeerConnection").new_(config);
        pc_.set("onicecandidate", loop_.handler(id_, 0));
        pc_.set("ondatachannel", loop_.handler(id_, 1));
        pc_.set("onconnectionstatechange", loop_.handler(id_, 2));
        pc_.set("oniceconnectionstatechange", loop_.handler(id_, 2));
    }

    ~peer_connection() override {
        loop_.remove(id_);
        for (const char* name : {"onicecandidate", "ondatachannel", "onconnectionstatechange", "oniceconnectionstatechange"}) pc_.set(name, val::null());
        pc_.call<void>("close");
    }

    void set_handlers(handlers h) override { h_ = std::move(h); }

    std::shared_ptr<backend::data_channel> create_data_channel(std::string_view label) override {
        return std::make_shared<data_channel>(loop_, pc_.call<val>("createDataChannel", loop_.text(label)));
    }

    void set_local_description(std::function<void(backend::error_code, std::string_view, std::string_view)> done) override {
        std::weak_ptr<peer_connection> weak = shared_from_this();
        detail::then(loop_, pc_.call<val>("setLocalDescription"), [weak, done = std::move(done)](bool ok, val) {
            auto self = weak.lock();
            if (!self || !ok) return done(make_error_code(error::backend_failure), {}, {});
            const val local = self->pc_["localDescription"];
            const auto type = local["type"].as<std::string>();
            const auto sdp = local["sdp"].as<std::string>();
            done({}, type, sdp);
        });
    }

    void set_remote_description(std::string_view type, std::string_view sdp, std::function<void(backend::error_code)> done) override {
        val description = val::object();
        description.set("type", loop_.text(type));
        description.set("sdp", loop_.text(sdp));
        detail::then(loop_, pc_.call<val>("setRemoteDescription", description), [done = std::move(done)](bool ok, val) {
            done(ok ? backend::error_code{} : make_error_code(error::backend_failure));
        });
    }

    void add_ice_candidate(std::string_view candidate_json, std::function<void(backend::error_code)> done) override {
        const val candidate = val::global("JSON").call<val>("parse", loop_.text(candidate_json));
        detail::then(loop_, pc_.call<val>("addIceCandidate", candidate), [done = std::move(done)](bool ok, val) {
            done(ok ? backend::error_code{} : make_error_code(error::backend_failure));
        });
    }

    bool has_remote_description() const override { return !pc_["remoteDescription"].isNull(); }

    void close() override { pc_.call<void>("close"); }

    /** The browser's own `RTCPeerConnection`: what media will be added to (the documentation's roadmap). */
    const val& native_handle() const { return pc_; }

private:
    backend::connection_state state() const {
        using backend::connection_state;
        // Compared in JavaScript, so reading a state allocates nothing on the C++ side.
        const val connection = pc_["connectionState"];
        const val ice = pc_["iceConnectionState"];
        auto is = [](const val& state, const char* name) { return state.strictlyEquals(val(name)); };
        if (is(connection, "failed") || is(ice, "failed")) return connection_state::failed;
        if (is(connection, "closed") || is(ice, "closed")) return connection_state::closed;
        if (is(connection, "disconnected") || is(ice, "disconnected")) return connection_state::disconnected;
        if (is(connection, "connected") || is(ice, "connected") || is(ice, "completed")) return connection_state::connected;
        if (is(connection, "connecting") || is(ice, "checking")) return connection_state::connecting;
        return connection_state::fresh;
    }

    void on_event(int slot, val event) {
        if (slot == 0) {
            const val candidate = event["candidate"];
            if (candidate.isNull() || candidate.isUndefined() || !h_.ice_candidate) return;
            auto f = h_.ice_candidate;
            const auto json = val::global("JSON").call<std::string>("stringify", candidate);
            f(json);
        } else if (slot == 1) {
            if (auto f = h_.data_channel) f(std::make_shared<data_channel>(loop_, event["channel"]));
        } else if (auto f = h_.state) {
            f(state());
        }
    }

    event_loop& loop_;
    int id_;
    val pc_ = val::undefined();
    handlers h_;
};

/**
 * AES-GCM through `crypto.subtle`, under a key imported once. The text is copied into JavaScript during the call; an
 * operation that arrives while the key is still being imported waits with a copy of its text.
 */
class sealer : public backend::sealer, public std::enable_shared_from_this<sealer> {
public:
    using done_handler = std::function<void(backend::error_code, std::string_view)>;

    sealer(event_loop& loop, std::array<std::uint8_t, 32> key) : loop_(loop), raw_key_(key) {}

    void seal(std::string_view plaintext, done_handler done) override {
        if (state_ == key_state::ready) return encrypt(plaintext, std::move(done));
        wait_for_key([plaintext = std::string(plaintext), done = std::move(done)](sealer& self, bool ok) mutable {
            if (!ok) return done(make_error_code(error::backend_failure), {});
            self.encrypt(plaintext, std::move(done));
        });
    }

    void open(std::string_view sealed, done_handler done) override {
        if (state_ == key_state::ready) return decrypt(sealed, std::move(done));
        wait_for_key([sealed = std::string(sealed), done = std::move(done)](sealer& self, bool ok) mutable {
            if (!ok) return done(make_error_code(error::backend_failure), {});
            self.decrypt(sealed, std::move(done));
        });
    }

private:
    static val subtle() { return val::global("crypto")["subtle"]; }

    static val algorithm(std::span<const std::uint8_t> iv) {
        val a = val::object();
        a.set("name", val("AES-GCM"));
        a.set("iv", detail::to_js(iv));
        return a;
    }

    void encrypt(std::string_view plaintext, done_handler done) {
        std::array<std::uint8_t, protocol::seal_iv_size> iv{};
        getentropy(iv.data(), iv.size());
        const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size());
        const val promise = subtle().call<val>("encrypt", algorithm(iv), key_, detail::to_js(bytes));
        detail::then(loop_, promise, [iv, done = std::move(done)](bool ok, val cipher) {
            if (!ok) return done(make_error_code(error::backend_failure), {});
            const auto sealed = protocol::format_sealed(iv, detail::from_js(cipher));
            done({}, sealed);
        });
    }

    void decrypt(std::string_view sealed, done_handler done) {
        const auto parts = protocol::parse_sealed(sealed);
        if (!parts || parts->iv.empty()) return done(make_error_code(error::wrong_password), {});
        const val promise = subtle().call<val>("decrypt", algorithm(parts->iv), key_, detail::to_js(parts->ciphertext));
        detail::then(loop_, promise, [done = std::move(done)](bool ok, val plain) {
            if (!ok) return done(make_error_code(error::wrong_password), {});
            const auto b = detail::from_js(plain);
            done({}, std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
        });
    }

    void wait_for_key(std::function<void(sealer&, bool)> op) {
        if (state_ == key_state::failed) return op(*this, false);
        waiting_.push_back(std::move(op));
        if (state_ == key_state::importing) return;
        state_ = key_state::importing;
        if (subtle().isUndefined()) return key_imported(false, val::undefined());  // not a secure context
        val usages = val::array();
        usages.call<void>("push", val("encrypt"));
        usages.call<void>("push", val("decrypt"));
        std::weak_ptr<sealer> weak = shared_from_this();
        detail::then(loop_, subtle().call<val>("importKey", val("raw"), detail::to_js(raw_key_), val("AES-GCM"), val(false), usages),
                     [weak](bool ok, val key) { if (auto self = weak.lock()) self->key_imported(ok, std::move(key)); });
    }

    void key_imported(bool ok, val key) {
        state_ = ok ? key_state::ready : key_state::failed;
        key_ = std::move(key);
        for (auto& op : std::exchange(waiting_, {})) op(*this, ok);
    }

    enum class key_state { none, importing, ready, failed };

    event_loop& loop_;
    std::array<std::uint8_t, 32> raw_key_;
    key_state state_ = key_state::none;
    val key_ = val::undefined();
    std::deque<std::function<void(sealer&, bool)>> waiting_;
};

/** The browser's factory: one per page, beside the page's `event_loop`. */
class backend : public trystero::backend::factory {
public:
    explicit backend(event_loop& loop) : loop_(loop) {}

    std::shared_ptr<trystero::backend::websocket> connect(std::string_view url, trystero::backend::websocket::handlers h) override {
        return std::make_shared<websocket>(loop_, url, std::move(h));
    }

    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(const std::vector<trystero::backend::ice_server>& servers) override {
        return std::make_shared<peer_connection>(loop_, servers);
    }

    std::unique_ptr<trystero::backend::timer> make_timer() override { return std::make_unique<timer>(loop_); }

    std::shared_ptr<trystero::backend::sealer> make_sealer(std::array<std::uint8_t, 32> key) override { return std::make_shared<sealer>(loop_, key); }

    void random(std::span<std::uint8_t> out) override {
        for (std::size_t at = 0; at < out.size(); at += 256) getentropy(out.data() + at, std::min<std::size_t>(256, out.size() - at));
    }

    std::int64_t unix_seconds() override { return static_cast<std::int64_t>(std::floor(val::global("Date").call<double>("now") / 1000)); }

    double now_ms() override { return val::global("performance").call<double>("now"); }

private:
    event_loop& loop_;
};

}  // namespace webcpp::trystero::browser

#endif
