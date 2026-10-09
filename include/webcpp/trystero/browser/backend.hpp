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
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/signal.hpp>

#include <emscripten/val.h>
#include <sys/random.h>  // lint-world: getentropy, the page's entropy

#include <cmath>
#include <deque>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <utility>
#include <vector>

/**
 * The browser target: the backend interface over the page's own `WebSocket`, `RTCPeerConnection`,
 * `RTCDataChannel`, `setTimeout` and `crypto.subtle`, through `emscripten::val`. It needs a secure
 * context (https, or http on localhost), where `crypto.subtle` exists.
 *
 * Tip: its objects, their handlers and the frames and texts they hand to the room are in the room's
 * resource; what `emscripten::val` holds is in the JavaScript heap, and the page's `event_loop`
 * keeps its registrations in its own resource.
 */
namespace webcpp::trystero::browser {

using emscripten::val;

namespace detail {

/** A copy of bytes as a new `Uint8Array`, independent of the wasm memory. */
inline val to_js(std::span<const std::uint8_t> data) {
    return val::global("Uint8Array")
        .new_(val(emscripten::typed_memory_view(data.size(), data.data())));
}

/** A copy of an `ArrayBuffer` or typed array's bytes, in `memory`. */
inline backend::bytes from_js(const val& buffer, std::pmr::memory_resource* memory) {
    const val view = val::global("Uint8Array").new_(buffer);
    backend::bytes out(view["length"].as<std::size_t>(), memory);
    val(emscripten::typed_memory_view(out.size(), out.data())).call<void>("set", view);
    return out;
}

/** Calls `done(true, value)` or `done(false, reason)` once a promise settles. */
inline void then(event_loop& loop, const val& promise, handler<void(bool, val)> done,
                 std::pmr::memory_resource* memory) {
    const std::pmr::polymorphic_allocator<> allocator(memory);
    const auto id = std::allocate_shared<int>(allocator, 0);
    *id = loop.add({std::allocator_arg, allocator,
                    [&loop, id, done = std::move(done)](int slot, val value) mutable {
                        loop.remove(*id);
                        done(slot == 0, std::move(value));
                    }});
    promise.call<void>("then", loop.handler(*id, 0), loop.handler(*id, 1));
}

}  // namespace detail

class timer : public backend::timer {
public:
    timer(event_loop& loop, std::pmr::memory_resource* memory)
        : loop_(loop),
          id_(loop.add({std::allocator_arg, std::pmr::polymorphic_allocator<>(memory),
                        [this](int /*slot*/, const val& /*value*/) { on_fire(); }})) {}

    ~timer() override {
        cancel();
        loop_.remove(id_);
    }

    void start(std::chrono::milliseconds delay, handler<void()> fire) override {
        cancel();
        fire_ = std::move(fire);
        handle_ = val::global("setTimeout")(
            loop_.handler(id_, 0), val(static_cast<double>(delay.count())), val::undefined());
    }

    void cancel() override {
        if (handle_.isUndefined()) {
            return;
        }
        val::global("clearTimeout")(handle_);
        handle_ = val::undefined();
        fire_ = nullptr;
    }

private:
    void on_fire() {
        handle_ = val::undefined();
        auto fire = std::exchange(fire_, nullptr);
        if (fire) {
            fire();  // may destroy this timer
        }
    }

    event_loop& loop_;
    int id_;
    val handle_ = val::undefined();
    handler<void()> fire_;
};

class websocket : public backend::websocket {
public:
    websocket(event_loop& loop, std::string_view url, handlers h, std::pmr::memory_resource* memory)
        : loop_(loop),
          memory_(memory),
          h_(std::allocate_shared<handlers>(std::pmr::polymorphic_allocator<>(memory),
                                            std::move(h))),
          id_(loop.add({std::allocator_arg, std::pmr::polymorphic_allocator<>(memory),
                        [this](int slot, const val& event) { on_event(slot, event); }})) {
        socket_ = val::global("WebSocket").new_(loop_.text(url));
        socket_.set("onopen", loop_.handler(id_, 0));
        socket_.set("onmessage", loop_.handler(id_, 1));
        socket_.set("onclose", loop_.handler(id_, 2));
    }

    ~websocket() override {
        loop_.remove(id_);
        for (const char* name : {"onopen", "onmessage", "onclose"}) {
            socket_.set(name, val::null());
        }
        socket_.call<void>("close");
    }

    void send(std::string_view text) override {
        if (socket_["readyState"].as<int>() == 1) {
            socket_.call<void>("send", loop_.text(text));
        }
    }

    void close() override { socket_.call<void>("close"); }

private:
    /** Calls through a copy of the shared handlers: a handler may destroy this socket during its
     * own call. */
    void on_event(int slot, const val& event) {
        const auto h = h_;
        if (slot == 0) {
            if (h->open) {
                h->open();
            }
        } else if (slot == 1) {
            const val data = event["data"];
            if (data.isString() && h->message) {
                const auto text =
                    loop_.utf8(data, memory_);  // the one copy: out of the JavaScript heap
                h->message(text);
            }
        } else if (auto f = std::exchange(h->close, {})) {
            f();
        }
    }

    event_loop& loop_;
    std::pmr::memory_resource* memory_;
    std::shared_ptr<handlers> h_;
    int id_;
    val socket_ = val::undefined();
};

class data_channel : public backend::data_channel {
public:
    data_channel(event_loop& loop, val channel, std::pmr::memory_resource* memory)
        : loop_(loop),
          memory_(memory),
          channel_(std::move(channel)),
          id_(loop.add({std::allocator_arg, std::pmr::polymorphic_allocator<>(memory),
                        [this](int slot, const val& event) { on_event(slot, event); }})) {
        channel_.set("binaryType", val("arraybuffer"));
        channel_.set("bufferedAmountLowThreshold", val(static_cast<double>(low_threshold)));
        channel_.set("onopen", loop_.handler(id_, 0));
        channel_.set("onmessage", loop_.handler(id_, 1));
        channel_.set("onclose", loop_.handler(id_, 2));
        channel_.set("onbufferedamountlow", loop_.handler(id_, 3));
    }

    ~data_channel() override {
        loop_.remove(id_);
        for (const char* name : {"onopen", "onmessage", "onclose", "onbufferedamountlow"}) {
            channel_.set(name, val::null());
        }
        channel_.call<void>("close");
    }

    /** The handlers, kept in a shared object so that one replacing them during its own call
     * outlives the call. */
    void set_handlers(handlers h) override {
        h_ = std::allocate_shared<handlers>(std::pmr::polymorphic_allocator<>(memory_),
                                            std::move(h));
    }

    [[nodiscard]] bool is_open() const override {
        return channel_["readyState"].strictlyEquals(val("open"));
    }

    void send(std::span<const std::uint8_t> frame) override {
        if (is_open()) {
            channel_.call<void>("send", detail::to_js(frame));
        }
    }

    [[nodiscard]] std::size_t buffered_amount() const override {
        return static_cast<std::size_t>(channel_["bufferedAmount"].as<double>());
    }

    void close() override { channel_.call<void>("close"); }

private:
    void on_event(int slot, const val& event) {
        const auto h = h_;
        if (!h) {
            return;
        }
        switch (slot) {
            case 0:
                if (h->open) {
                    h->open();
                }
                break;
            case 1:
                if (h->message) {
                    h->message(detail::from_js(event["data"], memory_));
                }
                break;
            case 2:
                if (auto f = std::exchange(h->close, {})) {
                    f();
                }
                break;
            default:
                if (h->buffered_amount_low) {
                    h->buffered_amount_low();
                }
        }
    }

    event_loop& loop_;
    std::pmr::memory_resource* memory_;
    val channel_;
    int id_;
    std::shared_ptr<handlers> h_;
};

class peer_connection : public backend::peer_connection,
                        public std::enable_shared_from_this<peer_connection> {
public:
    peer_connection(event_loop& loop, std::span<const backend::ice_server> servers,
                    std::pmr::memory_resource* memory)
        : loop_(loop),
          memory_(memory),
          id_(loop.add({std::allocator_arg, std::pmr::polymorphic_allocator<>(memory),
                        [this](int slot, const val& event) { on_event(slot, event); }})) {
        val list = val::array();
        for (const auto& s : servers) {
            val server = val::object();
            val urls = val::array();
            for (const auto& url : s.urls) {
                urls.call<void>("push", loop_.text(url));
            }
            server.set("urls", urls);
            if (!s.username.empty()) {
                server.set("username", loop_.text(s.username));
            }
            if (!s.credential.empty()) {
                server.set("credential", loop_.text(s.credential));
            }
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
        for (const char* name : {
                 "onicecandidate",
                 "ondatachannel",
                 "onconnectionstatechange",
                 "oniceconnectionstatechange",
             }) {
            pc_.set(name, val::null());
        }
        pc_.call<void>("close");
    }

    /** The handlers, kept in a shared object so that one replacing them during its own call
     * outlives the call. */
    void set_handlers(handlers h) override {
        h_ = std::allocate_shared<handlers>(allocator(), std::move(h));
    }

    std::shared_ptr<backend::data_channel> create_data_channel(std::string_view label) override {
        return std::allocate_shared<data_channel>(
            allocator(), loop_, pc_.call<val>("createDataChannel", loop_.text(label)), memory_);
    }

    void set_local_description(
        handler<void(backend::error_code, std::string_view, std::string_view)> done) override {
        const std::weak_ptr<peer_connection> weak = shared_from_this();
        detail::then(loop_, pc_.call<val>("setLocalDescription"),
                     {std::allocator_arg, allocator(),
                      [weak, done = std::move(done)](bool ok, const val& /*value*/) mutable {
                          const auto self = weak.lock();
                          if (!self || !ok) {
                              done(make_error_code(error::backend_failure), {}, {});
                              return;
                          }
                          const val local = self->pc_["localDescription"];
                          const auto type = self->loop_.utf8(local["type"], self->memory_);
                          const auto sdp = self->loop_.utf8(local["sdp"], self->memory_);
                          done({}, type, sdp);
                      }},
                     memory_);
    }

    void set_remote_description(std::string_view type, std::string_view sdp,
                                handler<void(backend::error_code)> done) override {
        val description = val::object();
        description.set("type", loop_.text(type));
        description.set("sdp", loop_.text(sdp));
        detail::then(
            loop_, pc_.call<val>("setRemoteDescription", description),
            {std::allocator_arg, allocator(),
             [done = std::move(done)](bool ok, const val& /*value*/) mutable {
                 done(ok ? backend::error_code{} : make_error_code(error::backend_failure));
             }},
            memory_);
    }

    void add_ice_candidate(std::string_view candidate_json,
                           handler<void(backend::error_code)> done) override {
        const val candidate = val::global("JSON").call<val>("parse", loop_.text(candidate_json));
        detail::then(
            loop_, pc_.call<val>("addIceCandidate", candidate),
            {std::allocator_arg, allocator(),
             [done = std::move(done)](bool ok, const val& /*value*/) mutable {
                 done(ok ? backend::error_code{} : make_error_code(error::backend_failure));
             }},
            memory_);
    }

    bool has_remote_description() const override { return !pc_["remoteDescription"].isNull(); }

    void close() override { pc_.call<void>("close"); }

    /** The browser's own `RTCPeerConnection`: what media will be added to (the documentation's
     * roadmap). */
    const val& native_handle() const { return pc_; }

private:
    std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    backend::connection_state state() const {
        using backend::connection_state;
        // Compared in JavaScript, so reading a state allocates nothing on the C++ side.
        const val connection = pc_["connectionState"];
        const val ice = pc_["iceConnectionState"];
        const auto is = [](const val& state, const char* name) {
            return state.strictlyEquals(val(name));
        };
        if (is(connection, "failed") || is(ice, "failed")) {
            return connection_state::failed;
        }
        if (is(connection, "closed") || is(ice, "closed")) {
            return connection_state::closed;
        }
        if (is(connection, "disconnected") || is(ice, "disconnected")) {
            return connection_state::disconnected;
        }
        if (is(connection, "connected") || is(ice, "connected") || is(ice, "completed")) {
            return connection_state::connected;
        }
        if (is(connection, "connecting") || is(ice, "checking")) {
            return connection_state::connecting;
        }
        return connection_state::fresh;
    }

    void on_event(int slot, const val& event) {
        const auto h = h_;
        if (!h) {
            return;
        }
        if (slot == 0) {
            const val candidate = event["candidate"];
            if (candidate.isNull() || candidate.isUndefined() || !h->ice_candidate) {
                return;
            }
            const auto json =
                loop_.utf8(val::global("JSON").call<val>("stringify", candidate), memory_);
            h->ice_candidate(json);
        } else if (slot == 1) {
            if (h->data_channel) {
                h->data_channel(std::allocate_shared<data_channel>(allocator(), loop_,
                                                                   event["channel"], memory_));
            }
        } else if (h->state) {
            h->state(state());
        }
    }

    event_loop& loop_;
    std::pmr::memory_resource* memory_;
    int id_;
    val pc_ = val::undefined();
    std::shared_ptr<handlers> h_;
};

/**
 * AES-GCM through `crypto.subtle`, under a key imported once. The text is copied into JavaScript
 * during the call; an operation that arrives while the key is still being imported waits with a
 * copy of its text, in the room's resource.
 */
class sealer : public backend::sealer, public std::enable_shared_from_this<sealer> {
public:
    using done_handler = handler<void(backend::error_code, std::string_view)>;

    sealer(event_loop& loop, std::array<std::uint8_t, 32> key, std::pmr::memory_resource* memory)
        : loop_(loop), raw_key_(key), memory_(memory), waiting_(memory) {}

    void seal(std::string_view plaintext, done_handler done) override {
        if (state_ == key_state::ready) {
            encrypt(plaintext, std::move(done));
            return;
        }
        wait_for_key({std::allocator_arg, allocator(),
                      [plaintext = std::pmr::string(plaintext, memory_), done = std::move(done)](
                          sealer& self, bool ok) mutable {
                          if (!ok) {
                              done(make_error_code(error::backend_failure), {});
                              return;
                          }
                          self.encrypt(plaintext, std::move(done));
                      }});
    }

    void open(std::string_view sealed, done_handler done) override {
        if (state_ == key_state::ready) {
            decrypt(sealed, std::move(done));
            return;
        }
        wait_for_key({std::allocator_arg, allocator(),
                      [sealed = std::pmr::string(sealed, memory_), done = std::move(done)](
                          sealer& self, bool ok) mutable {
                          if (!ok) {
                              done(make_error_code(error::backend_failure), {});
                              return;
                          }
                          self.decrypt(sealed, std::move(done));
                      }});
    }

private:
    std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    static val subtle() { return val::global("crypto")["subtle"]; }

    static val algorithm(std::span<const std::uint8_t> iv) {
        val a = val::object();
        a.set("name", val("AES-GCM"));
        a.set("iv", detail::to_js(iv));
        return a;
    }

    void encrypt(std::string_view plaintext, done_handler done) {
        std::array<std::uint8_t, protocol::seal_iv_size> iv{};
        getentropy(iv.data(), iv.size());  // lint-world: a seal's IV
        const auto bytes =
            std::span(reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size());
        const val promise =
            subtle().call<val>("encrypt", algorithm(iv), key_, detail::to_js(bytes));
        detail::then(
            loop_, promise,
            {std::allocator_arg, allocator(),
             [iv, done = std::move(done), memory = memory_](bool ok, const val& cipher) mutable {
                 if (!ok) {
                     done(make_error_code(error::backend_failure), {});
                     return;
                 }
                 const auto sealed =
                     protocol::format_sealed(iv, detail::from_js(cipher, memory), memory);
                 done({}, sealed);
             }},
            memory_);
    }

    void decrypt(std::string_view sealed, done_handler done) {
        const auto parts = protocol::parse_sealed(sealed, memory_);
        if (!parts || parts->iv.empty()) {
            done(make_error_code(error::wrong_password), {});
            return;
        }
        const val promise = subtle().call<val>("decrypt", algorithm(parts->iv), key_,
                                               detail::to_js(parts->ciphertext));
        detail::then(
            loop_, promise,
            {std::allocator_arg, allocator(),
             [done = std::move(done), memory = memory_](bool ok, const val& plain) mutable {
                 if (!ok) {
                     done(make_error_code(error::wrong_password), {});
                     return;
                 }
                 const auto b = detail::from_js(plain, memory);
                 done({}, std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
             }},
            memory_);
    }

    void wait_for_key(handler<void(sealer&, bool)> op) {
        if (state_ == key_state::failed) {
            op(*this, false);
            return;
        }
        waiting_.push_back(std::move(op));
        if (state_ == key_state::importing) {
            return;
        }
        state_ = key_state::importing;
        if (subtle().isUndefined()) {
            key_imported(false, val::undefined());
            return;  // not a secure context
        }
        val usages = val::array();
        usages.call<void>("push", val("encrypt"));
        usages.call<void>("push", val("decrypt"));
        const std::weak_ptr<sealer> weak = shared_from_this();
        detail::then(loop_,
                     subtle().call<val>("importKey", val("raw"), detail::to_js(raw_key_),
                                        val("AES-GCM"), val(false), usages),
                     {std::allocator_arg, allocator(),
                      [weak](bool ok, val key) {
                          if (const auto self = weak.lock()) {
                              self->key_imported(ok, std::move(key));
                          }
                      }},
                     memory_);
    }

    void key_imported(bool ok, val key) {
        state_ = ok ? key_state::ready : key_state::failed;
        key_ = std::move(key);
        for (auto& op : std::exchange(waiting_, {})) {
            op(*this, ok);
        }
    }

    enum class key_state { none, importing, ready, failed };

    event_loop& loop_;
    std::array<std::uint8_t, 32> raw_key_;
    std::pmr::memory_resource* memory_;
    key_state state_ = key_state::none;
    val key_ = val::undefined();
    std::pmr::deque<handler<void(sealer&, bool)>> waiting_;
};

/** The browser's factory: one per page, beside the page's `event_loop`. */
class backend : public trystero::backend::factory {
public:
    explicit backend(event_loop& loop) : loop_(loop) {}

    std::shared_ptr<trystero::backend::websocket> connect(
        std::string_view url, trystero::backend::websocket::handlers h,
        std::pmr::memory_resource* memory) override {
        return std::allocate_shared<websocket>(std::pmr::polymorphic_allocator<>(memory), loop_,
                                               url, std::move(h), memory);
    }

    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
        return std::allocate_shared<peer_connection>(std::pmr::polymorphic_allocator<>(memory),
                                                     loop_, servers, memory);
    }

    std::shared_ptr<trystero::backend::timer> make_timer(
        std::pmr::memory_resource* memory) override {
        return std::allocate_shared<timer>(std::pmr::polymorphic_allocator<>(memory), loop_,
                                           memory);
    }

    std::shared_ptr<trystero::backend::sealer> make_sealer(
        std::array<std::uint8_t, 32> key, std::pmr::memory_resource* memory) override {
        return std::allocate_shared<sealer>(std::pmr::polymorphic_allocator<>(memory), loop_, key,
                                            memory);
    }

    void random(std::span<std::uint8_t> out) override {
        for (std::size_t at = 0; at < out.size(); at += 256) {
            // Each call fills at most 256 bytes.
            const auto piece = std::min<std::size_t>(256, out.size() - at);
            getentropy(out.data() + at, piece);  // lint-world: Trystero's randomness
        }
    }

    std::int64_t unix_seconds() override {
        return static_cast<std::int64_t>(
            std::floor(val::global("Date").call<double>("now") / 1000));
    }

    double now_ms() override { return val::global("performance").call<double>("now"); }

private:
    event_loop& loop_;
};

}  // namespace webcpp::trystero::browser

#endif
