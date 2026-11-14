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
#include <webcpp/trystero/browser/js.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/webrtc/peer_connection.hpp>

#include <webcpp/webrtc/options.hpp>
#include <webcpp/webrtc/runtime.hpp>

#include <webcpp/asio/bind_allocator.hpp>  // lint-world: binds an allocator only
#include <webcpp/asio/post.hpp>            // lint-world: posts handlers only

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
 * The browser target: the backend interface over the page's own `WebSocket`, `setTimeout` and
 * `crypto.subtle`, through `emscripten::val`, and over its `RTCPeerConnection` and
 * `RTCDataChannel` through webcpp::webrtc. It needs a secure context (https, or http on
 * localhost), where `crypto.subtle` exists.
 *
 * Tip: its objects, their handlers and the frames and texts they hand to the room are in the room's
 * resource; what `emscripten::val` holds is in the JavaScript heap, and the page's `event_loop`
 * keeps its registrations in its own resource.
 */
namespace webcpp::trystero::browser {

/** A JavaScript value, embind's `emscripten::val`. */
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

/**
 * Runs `f` on the loop's io_context after the current call, allocated in `memory`: how a
 * completion that is known at once still reaches its handler later, never inside the call that
 * started the operation.
 */
template <class F>
void defer(event_loop& loop, std::pmr::memory_resource* memory, F&& f) {
    webcpp::asio::post(loop.io(),
                       webcpp::asio::bind_allocator(std::pmr::polymorphic_allocator<>(memory),
                                                    std::forward<F>(f)));
    loop.schedule_poll();
}

/**
 * Calls `done(true, value)` or `done(false, reason)` once a promise an operation returned
 * settles, or `done(false, exception)` after the call when the operation threw instead.
 */
inline void then(event_loop& loop, const js_outcome& operation, handler<void(bool, val)> done,
                 std::pmr::memory_resource* memory);

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

inline void then(event_loop& loop, const js_outcome& operation, handler<void(bool, val)> done,
                 std::pmr::memory_resource* memory) {
    if (operation.ok) {
        then(loop, operation.value, std::move(done), memory);
        return;
    }
    defer(loop, memory, [done = std::move(done), reason = operation.value]() mutable {
        done(false, std::move(reason));
    });
}

}  // namespace detail

/**
 * A timer over the page's `setTimeout`.
 *
 * @see "The browser backend", in the guide.
 */
class timer : public backend::timer {
public:
    /**
     * Constructs a timer, not started, registered with the loop.
     *
     * @param loop The page's loop, which must outlive the timer.
     * @param memory The room's resource, which the timer's registration and handler are allocated
     * in.
     */
    timer(event_loop& loop, std::pmr::memory_resource* memory)
        : loop_(loop),
          id_(loop.add({std::allocator_arg, std::pmr::polymorphic_allocator<>(memory),
                        [this](int /*slot*/, const val& /*value*/) { on_fire(); }})) {}

    /** Destroys the timer, which clears a pending `setTimeout`. */
    ~timer() override {
        cancel();
        loop_.remove(id_);
    }

    /**
     * Calls `fire` once `delay` has passed, through `setTimeout`, in place of a pending expiry.
     *
     * @param delay How long to wait.
     * @param fire What to call when the timer expires; never called once it is cancelled.
     */
    void start(std::chrono::milliseconds delay, handler<void()> fire) override {
        cancel();
        fire_ = std::move(fire);
        handle_ = val::global("setTimeout")(
            loop_.handler(id_, 0), val(static_cast<double>(delay.count())), val::undefined());
    }

    /** Clears the pending `setTimeout`, if any, whose handler is then never called. */
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

/**
 * A WebSocket over the page's `WebSocket`.
 *
 * A text longer than `protocol::max_relay_message_bytes` is not copied into WebAssembly: the
 * socket closes, and reports the close. A text that would take the socket's `bufferedAmount`
 * beyond `protocol::max_queued_relay_bytes`, as when a relay stops reading, drops the socket,
 * which reports one close at once. A page has no idle timeout: its `WebSocket` cannot ping.
 *
 * @see "The browser backend", in the guide.
 */
class websocket : public backend::websocket {
public:
    /**
     * Opens a WebSocket to `url`.
     *
     * A URL the page's `WebSocket` refuses with an exception (one that does not parse, another
     * scheme, a fragment, mixed content, a policy of the page's) makes no socket: the handler
     * `close` reports it after the call, as it reports a relay that cannot be reached.
     *
     * @param loop The page's loop, which must outlive the WebSocket.
     * @param url The relay's URL, read during the call.
     * @param h What the WebSocket reports.
     * @param memory The room's resource, which the WebSocket's handlers, registration and received
     * texts are allocated in.
     */
    websocket(event_loop& loop, std::string_view url, handlers h, std::pmr::memory_resource* memory)
        : loop_(loop),
          memory_(memory),
          h_(std::allocate_shared<handlers>(std::pmr::polymorphic_allocator<>(memory),
                                            std::move(h))),
          id_(loop.add({std::allocator_arg, std::pmr::polymorphic_allocator<>(memory),
                        [this](int slot, const val& event) { on_event(slot, event); }})) {
        const auto made = detail::js_new(val::global("WebSocket"), loop_.text(url));
        if (!made.ok) {
            detail::defer(loop_, memory_, [weak = std::weak_ptr<handlers>(h_)] {
                if (const auto h = weak.lock()) {
                    if (auto f = std::exchange(h->close, {})) {
                        f();
                    }
                }
            });
            return;
        }
        socket_ = made.value;
        socket_.set("onopen", loop_.handler(id_, 0));
        socket_.set("onmessage", loop_.handler(id_, 1));
        socket_.set("onclose", loop_.handler(id_, 2));
    }

    /** Destroys the WebSocket, which closes it without reporting the close. */
    ~websocket() override {
        loop_.remove(id_);
        if (socket_.isUndefined()) {
            return;
        }
        for (const char* name : {"onopen", "onmessage", "onclose"}) {
            socket_.set(name, val::null());
        }
        socket_.call<void>("close");
    }

    /**
     * Sends a text message when the socket is open, its `readyState` 1; otherwise, or when the
     * socket refuses it, drops it.
     *
     * A text that would take the bytes the socket holds unsent beyond
     * `protocol::max_queued_relay_bytes` drops the socket instead, which reports its close.
     *
     * @param text The message, read during the call.
     */
    void send(std::string_view text) override {
        if (socket_.isUndefined() || socket_["readyState"].as<int>() != 1) {
            return;
        }
        const auto held = static_cast<std::size_t>(socket_["bufferedAmount"].as<double>());
        if (held + text.size() > protocol::max_queued_relay_bytes) {
            drop();
            return;
        }
        detail::js_call(socket_, "send", loop_.text(text));
    }

    /** Closes the socket, which the handler `close` reports; one never made has reported it. */
    void close() override {
        if (!socket_.isUndefined()) {
            socket_.call<void>("close");
        }
    }

private:
    /**
     * Drops a socket whose relay stopped reading: its close handshake could never complete, so the
     * socket is detached and closed, and its close reported after the call, once.
     */
    void drop() {
        for (const char* name : {"onopen", "onmessage", "onclose"}) {
            socket_.set(name, val::null());
        }
        socket_.call<void>("close");
        socket_ = val::undefined();
        detail::defer(loop_, memory_, [weak = std::weak_ptr<handlers>(h_)] {
            if (const auto h = weak.lock()) {
                if (auto f = std::exchange(h->close, {})) {
                    f();
                }
            }
        });
    }

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
                // The one copy, out of the JavaScript heap, of at most a relay's longest text: a
                // longer one closes the socket, which reports the close, as the native one does.
                const auto text = loop_.utf8(data, memory_, protocol::max_relay_message_bytes);
                if (!text) {
                    socket_.call<void>("close");
                    return;
                }
                h->message(*text);
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

/**
 * AES-GCM through `crypto.subtle`, under a key imported once, as Trystero's `encrypt` and `decrypt`
 * do.
 *
 * The text is copied into JavaScript during the call; an operation that arrives while the key is
 * still being imported waits with a copy of its text, in the room's resource. Without
 * `crypto.subtle`, outside a secure context, every operation fails.
 *
 * @see "The browser backend", in the guide.
 */
class sealer : public backend::sealer, public std::enable_shared_from_this<sealer> {
public:
    /** What a seal or an open completes with: an error, or the text, a view valid for the call. */
    using done_handler = handler<void(backend::error_code, std::string_view)>;

    /**
     * Constructs a sealer under a room's key, which it imports on its first operation.
     *
     * @param loop The page's loop, which must outlive the sealer's pending promises.
     * @param key The raw AES-256 key, `protocol::seal_key`'s.
     * @param memory The room's resource, which its texts, its waiting operations and its
     * completions are allocated in.
     */
    sealer(event_loop& loop, std::array<std::uint8_t, 32> key, std::pmr::memory_resource* memory)
        : loop_(loop), raw_key_(key), memory_(memory), waiting_(memory) {}

    /**
     * Seals a text under a fresh random IV, with `crypto.subtle.encrypt`.
     *
     * @param plaintext The text, read during the call.
     * @param done Called with the sealed text, or with `error::backend_failure` when the key
     * cannot be imported or the encryption fails.
     */
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

    /**
     * Opens a sealed text, with `crypto.subtle.decrypt`.
     *
     * @param sealed The sealed text, read during the call.
     * @param done Called with the plaintext, or with `error::wrong_password` when the text was
     * sealed under another key or is not a sealed text, and `error::backend_failure` when the key
     * cannot be imported.
     */
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

    /** The page's `crypto.subtle`; undefined outside a secure context, or without `crypto`. */
    static val subtle() {
        const val crypto = val::global("crypto");
        return crypto.isUndefined() || crypto.isNull() ? val::undefined() : crypto["subtle"];
    }

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
        detail::then(
            loop_, detail::js_call(subtle(), "encrypt", algorithm(iv), key_, detail::to_js(bytes)),
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
            detail::defer(loop_, memory_, [done = std::move(done)]() mutable {
                done(make_error_code(error::wrong_password), {});
            });
            return;
        }
        detail::then(
            loop_,
            detail::js_call(subtle(), "decrypt", algorithm(parts->iv), key_,
                            detail::to_js(parts->ciphertext)),
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

    /**
     * Runs `op` once the key is imported, or has failed to be: after the call in every case, so
     * that no completion runs inside the operation that started it.
     */
    void wait_for_key(handler<void(sealer&, bool)> op) {
        const std::weak_ptr<sealer> weak = shared_from_this();
        if (state_ == key_state::failed) {
            detail::defer(loop_, memory_, [weak, op = std::move(op)]() mutable {
                if (const auto self = weak.lock()) {
                    op(*self, false);
                }
            });
            return;
        }
        waiting_.push_back(std::move(op));
        if (state_ == key_state::importing) {
            return;
        }
        state_ = key_state::importing;
        if (subtle().isUndefined()) {  // not a secure context
            detail::defer(loop_, memory_, [weak] {
                if (const auto self = weak.lock()) {
                    self->key_imported(false, val::undefined());
                }
            });
            return;
        }
        val usages = val::array();
        usages.call<void>("push", val("encrypt"));
        usages.call<void>("push", val("decrypt"));
        detail::then(loop_,
                     detail::js_call(subtle(), "importKey", "raw", detail::to_js(raw_key_),
                                     "AES-GCM", false, usages),
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

/**
 * The browser's factory: one per page, beside the page's `event_loop`.
 *
 * It makes peer connections, data channels and received frames in `app_config::memory`, and
 * WebSockets, sealers and room timers in the room's effective `room_options::memory`. Objects,
 * handlers and registrations use the resource supplied to their factory method. What
 * `emscripten::val` holds is in the JavaScript heap. It needs a secure context, where
 * `crypto.subtle` exists.
 *
 * @see "The browser backend", in the guide.
 */
class backend : public trystero::backend::factory {
public:
    /**
     * The most `RTCPeerConnection` objects the page holds, every program of the page together,
     * counted from their construction until the garbage collector frees them: half of the 500
     * Chromium allows a page, which it counts the same way, so that a closed connection still
     * counts until a collection the page cannot ask for.
     *
     * It is the cap of the backend's webcpp::webrtc runtime,
     * `runtime_options::max_peer_connections`, whose census of the page counts the connections
     * webcpp::webrtc makes.
     *
     * Tip: a room makes at most 64 connections in any 57 s, so floods of four rooms reach this
     * within a minute when no collection runs; the page then makes none until one does, and its
     * rooms report each negotiation they cannot start, while the rest of Chromium's count stays
     * free for what the page makes without a room.
     */
    static constexpr std::size_t max_peer_connections = 256;

    /**
     * Constructs the page's backend, with a webcpp::webrtc runtime of its own: in the browser the
     * page's census counts the connections of every runtime alike.
     *
     * @param loop The page's loop, which must outlive the backend and every object it makes.
     */
    explicit backend(event_loop& loop)
        : loop_(loop),
          runtime_(std::allocate_shared<::webcpp::webrtc::runtime>(
              std::pmr::polymorphic_allocator<>(std::pmr::get_default_resource()),
              ::webcpp::webrtc::runtime_options{
                  .max_peer_connections = max_peer_connections,
                  .thread_pool_size = 0,
              })) {}

    /**
     * Opens a WebSocket to a relay, with the page's `WebSocket`.
     *
     * @param url The relay's URL, read during the call.
     * @param h What the WebSocket reports.
     * @param memory The room's resource, which the WebSocket, its handlers and its texts are
     * allocated in.
     * @return The WebSocket.
     */
    std::shared_ptr<trystero::backend::websocket> connect(
        std::string_view url, trystero::backend::websocket::handlers h,
        std::pmr::memory_resource* memory) override {
        return std::allocate_shared<websocket>(std::pmr::polymorphic_allocator<>(memory), loop_,
                                               url, std::move(h), memory);
    }

    /**
     * Makes a peer connection of webcpp::webrtc, over the page's `RTCPeerConnection`.
     *
     * @param servers The STUN and TURN servers, read during the call.
     * @param memory The app's resource, which the connection, its channels, its handlers and its
     * texts are allocated in.
     * @return The connection, or null when the page holds `max_peer_connections`, or when the
     * browser refuses to make one: it throws for a server it cannot use, and when the page holds as
     * many connections as it allows.
     */
    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
        auto made = webrtc::peer_connection::make(runtime_, loop_.get_executor(), servers, {},
                                                  *this, memory);
        return made ? *made : nullptr;
    }

    /**
     * Makes a timer, over `setTimeout`.
     *
     * @param memory The room's resource, which the timer and its handler are allocated in.
     * @return The timer, not started.
     */
    std::shared_ptr<trystero::backend::timer> make_timer(
        std::pmr::memory_resource* memory) override {
        return std::allocate_shared<timer>(std::pmr::polymorphic_allocator<>(memory), loop_,
                                           memory);
    }

    /**
     * Makes a sealer, over `crypto.subtle`.
     *
     * @param key The raw AES-256 key.
     * @param memory The room's resource, which the sealer, its texts and its completions are
     * allocated in.
     * @return The sealer.
     */
    std::shared_ptr<trystero::backend::sealer> make_sealer(
        std::array<std::uint8_t, 32> key, std::pmr::memory_resource* memory) override {
        return std::allocate_shared<sealer>(std::pmr::polymorphic_allocator<>(memory), loop_, key,
                                            memory);
    }

    /**
     * Fills `out` with the page's entropy, which Emscripten draws from `crypto.getRandomValues`.
     *
     * @param out The bytes to fill.
     */
    void random(std::span<std::uint8_t> out) override {
        for (std::size_t at = 0; at < out.size(); at += 256) {
            // Each call fills at most 256 bytes.
            const auto piece = std::min<std::size_t>(256, out.size() - at);
            getentropy(out.data() + at, piece);  // lint-world: Trystero's randomness
        }
    }

    /**
     * Seconds since the Unix epoch, from `Date.now()`.
     *
     * @return The current time, in whole seconds.
     */
    std::int64_t unix_seconds() override {
        return static_cast<std::int64_t>(
            std::floor(val::global("Date").call<double>("now") / 1000));
    }

    /**
     * Milliseconds from `performance.now()`.
     *
     * @return The current time, in milliseconds since the page's time origin.
     */
    double now_ms() override { return val::global("performance").call<double>("now"); }

private:
    event_loop& loop_;
    std::shared_ptr<::webcpp::webrtc::runtime> runtime_;
};

}  // namespace webcpp::trystero::browser

#endif
