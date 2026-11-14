// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_BROWSER_BACKEND_HPP
#define WEBCPP_TRYSTERO_BROWSER_BACKEND_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/post.hpp>
#include <webcpp/trystero/detail/timer.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/webrtc/peer_connection.hpp>

#include <webcpp/webrtc/options.hpp>
#include <webcpp/webrtc/runtime.hpp>

#include <webcpp/asio/any_io_executor.hpp>      // lint-world: names the executor only
#include <webcpp/asio/bind_allocator.hpp>       // lint-world: binds an allocator only
#include <webcpp/asio/browser/async_await.hpp>  // lint-world: awaits the page's promises
#include <webcpp/asio/browser/bridge.hpp>       // lint-world: the page's events
#include <webcpp/asio/execution/context.hpp>    // lint-world: the bridge of the executor's context
#include <webcpp/asio/query.hpp>                // lint-world: the bridge of the executor's context

#include <emscripten/em_asm.h>
#include <emscripten/val.h>
#include <sys/random.h>  // lint-world: getentropy, the page's entropy

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

/**
 * The browser target: the backend interface over the page's own `WebSocket` and `crypto.subtle`,
 * over webcpp::asio's timers, and over its `RTCPeerConnection` and `RTCDataChannel` through
 * webcpp::webrtc. It needs a secure context (https, or http on localhost), where `crypto.subtle`
 * exists.
 *
 * Tip: the page's events reach C++ through webcpp::asio's bridge, a service of the program's
 * io_context, which only posts to it: they run while the program runs that io_context, from
 * `main`. Its objects, their handlers and the frames and texts they hand to the room are in the
 * room's resource; what `emscripten::val` holds is in the JavaScript heap.
 */
namespace webcpp::trystero::browser {

/** A JavaScript value, embind's `emscripten::val`. */
using emscripten::val;

namespace detail {

// The page's WebSocket and Web Crypto, called from C++. Each function is an EM_ASM block in an
// inline function, which Emscripten allows in several translation units, as webcpp::webrtc's and
// webcpp::beast's browser imports are. JavaScript stays in JavaScript: what can throw is caught
// there, before it can reach a WebAssembly frame, and a Web Crypto call is made inside an async
// function, so that what it throws becomes the rejection of the promise it returns. The functions
// use Emval, HEAPU8 and UTF8ToString, which webcpp::asio's browser runtime has Emscripten link, in
// every program on /webcpp/asio//asio.

// clang-format off
/** The JavaScript string of `size` bytes of UTF-8 at `data`, its NUL characters included. */
inline emscripten::EM_VAL js_text(const char* data, std::size_t size) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const data = $0 >>> 0;
    const size = $1 >>> 0;

    return Emval.toHandle(UTF8ToString(data, size, true));
    }), data, size));
}

/**
 * The UTF-8 bytes of `text` as a `Uint8Array`, or null for a value that is no string or for a
 * text longer than `max` bytes, whose length is checked first in UTF-16 units.
 */
inline emscripten::EM_VAL js_encode(emscripten::EM_VAL text, double max) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const text = Emval.toValue($0);
    const max = $1;

    if (typeof text !== 'string' || text.length > max) return Emval.toHandle(null);
    const bytes = new TextEncoder().encode(text);
    return Emval.toHandle(bytes.length > max ? null : bytes);
    }), text, max));
}

/** Copies the first `size` bytes of `bytes`, a `Uint8Array`, to `data`. */
// NOLINTNEXTLINE(misc-const-correctness): JavaScript writes to the pointed WebAssembly storage
inline void js_copy(emscripten::EM_VAL bytes, void* data, std::size_t size) {
    EM_ASM(({
    const bytes = Emval.toValue($0);
    const data = $1 >>> 0;
    const size = $2 >>> 0;

    HEAPU8.set(bytes.subarray(0, size), data);
    }), bytes, data, size);
}

/**
 * A `WebSocket` to `url` whose events call `deliver` with `{kind: 0}`, `{kind: 1, data}` and
 * `{kind: 2}`, its open, a message and its close, or null when the constructor throws.
 */
inline emscripten::EM_VAL js_websocket(emscripten::EM_VAL url, emscripten::EM_VAL deliver) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const url = Emval.toValue($0);
    const deliver = Emval.toValue($1);

    try {
        const socket = new WebSocket(url);
        socket.onopen = () => deliver({kind: 0});
        socket.onmessage = event => deliver({kind: 1, data: event.data});
        socket.onclose = () => deliver({kind: 2});
        return Emval.toHandle(socket);
    } catch (_) {
        return Emval.toHandle(null);
    }
    }), url, deliver));
}

/**
 * Sends `size` bytes of UTF-8 at `data` as a text when the socket is open, returning 1, with
 * nothing sent, when its `bufferedAmount` would pass `bound` with them, else 0.
 */
inline int js_send(emscripten::EM_VAL socket, const char* data, std::size_t size, double bound) {
    return EM_ASM_INT(({
    const socket = Emval.toValue($0);
    const data = $1 >>> 0;
    const size = $2 >>> 0;
    const bound = $3;

    try {
        if (socket.readyState !== 1) return 0;
        if (Number(socket.bufferedAmount) + size > bound) return 1;
        socket.send(UTF8ToString(data, size, true));
    } catch (_) {
    }
    return 0;
    }), socket, data, size, bound);
}

/** Closes a socket, its handlers set to null first with `detach`, so that it reports nothing. */
inline void js_close(emscripten::EM_VAL socket, bool detach) {
    EM_ASM(({
    const socket = Emval.toValue($0);
    const detach = $1;

    try {
        if (detach) {
            socket.onopen = null;
            socket.onmessage = null;
            socket.onclose = null;
        }
        socket.close();
    } catch (_) {
    }
    }), socket, detach);
}

/**
 * A promise of the AES-GCM key of the `size` raw bytes at `key`, from `crypto.subtle.importKey`,
 * rejected where there is no `crypto.subtle`.
 */
inline emscripten::EM_VAL js_import_key(const std::uint8_t* key, std::size_t size) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const key = $0 >>> 0;
    const size = $1 >>> 0;

    const raw = HEAPU8.slice(key, key + size);
    return Emval.toHandle((async () => crypto.subtle.importKey('raw', raw, 'AES-GCM', false,
        ['encrypt', 'decrypt']))());
    }), key, size));
}

/**
 * A promise of `crypto.subtle.encrypt`, or `decrypt`, of the `size` bytes at `data` under `key`
 * and the IV at `iv`, rejected for what the call throws as for what it rejects.
 */
inline emscripten::EM_VAL js_crypt(bool decrypt, emscripten::EM_VAL key, const std::uint8_t* iv,
                                   std::size_t iv_size, const void* data, std::size_t size) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const decrypt = $0;
    const key = Emval.toValue($1);
    const iv = HEAPU8.slice($2 >>> 0, ($2 >>> 0) + ($3 >>> 0));
    const data = HEAPU8.slice($4 >>> 0, ($4 >>> 0) + ($5 >>> 0));

    const algorithm = {name: 'AES-GCM', iv};
    return Emval.toHandle((async () => decrypt ? crypto.subtle.decrypt(algorithm, key, data)
        : crypto.subtle.encrypt(algorithm, key, data))());
    }), decrypt, key, iv, iv_size, data, size));
}
// clang-format on

/** Takes ownership of a handle an import returned. */
inline val take(emscripten::EM_VAL handle) {
    return val::take_ownership(handle);
}

/**
 * The UTF-8 bytes of a JavaScript string, copied straight into a string of `memory` when they are
 * at most `max_bytes`, and nothing otherwise, nor for a value that is no string.
 */
inline std::optional<std::pmr::string> utf8(const val& text, std::pmr::memory_resource* memory,
                                            std::size_t max_bytes) {
    const val encoded = take(js_encode(text.as_handle(), static_cast<double>(max_bytes)));
    if (encoded.isNull()) {
        return std::nullopt;
    }
    std::pmr::string out(encoded["length"].as<std::size_t>(), '\0', memory);
    js_copy(encoded.as_handle(), out.data(), out.size());
    return out;
}

/** A copy of an `ArrayBuffer`'s bytes, in `memory`. */
inline backend::bytes from_js(const val& buffer, std::pmr::memory_resource* memory) {
    const val view = val::global("Uint8Array").new_(buffer);
    backend::bytes out(view["length"].as<std::size_t>(), memory);
    js_copy(view.as_handle(), out.data(), out.size());
    return out;
}

/** The bridge of the io_context an executor runs on, which delivers the page's events to it. */
inline webcpp::asio::browser::bridge& bridge_of(const webcpp::asio::any_io_executor& ex) {
    return webcpp::asio::use_service<webcpp::asio::browser::bridge>(
        webcpp::asio::query(ex, webcpp::asio::execution::context));
}

}  // namespace detail

/**
 * A JavaScript string of UTF-8 text, decoded straight from the WebAssembly memory the view points
 * into.
 *
 * No `std::string` is made to carry it across, as `emscripten::val(std::string)` would; a NUL
 * character is kept, and a byte that is not UTF-8 becomes U+FFFD, as `TextDecoder` makes it.
 *
 * @param utf8 The text, read during the call.
 * @return The JavaScript string.
 */
inline val js_string(std::string_view utf8) {
    return detail::take(detail::js_text(utf8.data(), utf8.size()));
}

/**
 * A WebSocket over the page's `WebSocket`, whose events reach the executor through webcpp::asio's
 * bridge.
 *
 * Each event of the page's socket, its open, a message or its close, is posted to the executor in
 * the order the page delivers them, and reaches the handlers when it runs there. A text longer
 * than `protocol::max_relay_message_bytes` is not copied into WebAssembly: the socket closes, and
 * reports the close. A text that would take the socket's `bufferedAmount` beyond
 * `protocol::max_queued_relay_bytes`, as when a relay stops reading, drops the socket, which
 * reports one close after the call. A page has no idle timeout: its `WebSocket` cannot ping.
 *
 * @see "The browser backend", in the guide.
 */
class websocket : public backend::websocket, public std::enable_shared_from_this<websocket> {
public:
    /**
     * Opens a WebSocket to `url`.
     *
     * A URL the page's `WebSocket` refuses with an exception (one that does not parse, another
     * scheme, a fragment, mixed content, a policy of the page's) makes no socket: the handler
     * `close` reports it after the call, as it reports a relay that cannot be reached.
     *
     * @param ex The executor its events are posted to, an io_context's of webcpp::asio, which must
     * outlive the WebSocket.
     * @param url The relay's URL, read during the call.
     * @param h What the WebSocket reports.
     * @param memory The room's resource, which the WebSocket's handlers, its events and the texts
     * it receives are allocated in.
     */
    websocket(const webcpp::asio::any_io_executor& ex, std::string_view url, handlers h,
              std::pmr::memory_resource* memory)
        : ex_(ex),
          memory_(memory),
          h_(std::allocate_shared<handlers>(std::pmr::polymorphic_allocator<>(memory),
                                            std::move(h))),
          events_(*this),
          bridge_(&detail::bridge_of(ex_)),
          key_(bridge_->add(events_)) {
        socket_ = detail::take(
            detail::js_websocket(js_string(url).as_handle(), bridge_->function(key_).as_handle()));
        if (socket_.isNull()) {
            socket_ = val::undefined();
            leave_bridge();
            report(close_event, std::pmr::string(memory_));
        }
    }

    /** Not copyable: the bridge delivers to it by its address. */
    websocket(const websocket&) = delete;

    /** Not copy-assignable: the bridge delivers to it by its address. */
    websocket& operator=(const websocket&) = delete;

    /** Destroys the WebSocket, which closes it without reporting the close. */
    ~websocket() override {
        leave_bridge();
        if (!socket_.isUndefined()) {
            detail::js_close(socket_.as_handle(), true);
        }
    }

    /**
     * Sends a text message when the socket is open, its `readyState` 1; otherwise, or when the
     * socket refuses it, drops it.
     *
     * A text that would take the bytes the socket holds unsent beyond
     * `protocol::max_queued_relay_bytes` drops the socket instead, which reports its close after
     * the call.
     *
     * @param text The message, read during the call.
     */
    void send(std::string_view text) override {
        if (socket_.isUndefined()) {
            return;
        }
        if (detail::js_send(socket_.as_handle(), text.data(), text.size(),
                            static_cast<double>(protocol::max_queued_relay_bytes)) != 0) {
            drop();
        }
    }

    /** Closes the socket, which the handler `close` reports; one never made has reported it. */
    void close() override {
        if (!socket_.isUndefined()) {
            detail::js_close(socket_.as_handle(), false);
        }
    }

private:
    /** What the bridge delivers the page's events of the socket to. */
    class events final : public webcpp::asio::browser::intrusive_registration {
    public:
        explicit events(websocket& owner) noexcept : owner_(owner) {}

        bool deliver(const val& event) override { return owner_.deliver(event); }

        void shutdown() noexcept override { owner_.shutdown(); }

    private:
        websocket& owner_;
    };

    static constexpr int open_event = 0;
    static constexpr int message_event = 1;
    static constexpr int close_event = 2;

    /**
     * Posts an event of the page to the executor, a message copied into the room's resource first,
     * or closes the socket for a text too long for a relay; it runs no handler.
     */
    bool deliver(const val& event) {
        const int kind = event["kind"].as<int>();
        if (kind != message_event) {
            report(kind, std::pmr::string(memory_));
            return false;
        }
        const val data = event["data"];
        if (!data.isString()) {
            return false;
        }
        // The one copy, out of the JavaScript heap, of at most a relay's longest text: a longer
        // one closes the socket, which reports the close, as the native one does.
        auto text = detail::utf8(data, memory_, protocol::max_relay_message_bytes);
        if (!text) {
            close();
            return false;
        }
        report(message_event, std::move(*text));
        return false;
    }

    /** The io_context's shutdown: the socket is closed, and reports nothing more. */
    void shutdown() noexcept {
        bridge_ = nullptr;
        key_ = 0;
        if (!socket_.isUndefined()) {
            detail::js_close(socket_.as_handle(), true);
            socket_ = val::undefined();
        }
    }

    /**
     * Posts the report of an event to the executor, through the shared handlers: a socket destroyed
     * before it runs reports nothing, and a handler may destroy the socket during its own call.
     */
    void report(int kind, std::pmr::string text) {
        trystero::detail::post(
            ex_, memory_,
            [weak = std::weak_ptr<handlers>(h_), kind, text = std::move(text)]() mutable {
                const auto h = weak.lock();
                if (!h) {
                    return;
                }
                if (kind == open_event) {
                    if (h->open) {
                        h->open();
                    }
                } else if (kind == message_event) {
                    if (h->message) {
                        h->message(text);
                    }
                } else if (auto f = std::exchange(h->close, {})) {
                    f();
                }
            });
    }

    /**
     * Drops a socket whose relay stopped reading: its close handshake could never complete, so the
     * socket is detached and closed, and its close reported after the call, once.
     */
    void drop() {
        leave_bridge();
        detail::js_close(socket_.as_handle(), true);
        socket_ = val::undefined();
        report(close_event, std::pmr::string(memory_));
    }

    /** Removes the registration: an event of the page from now on finds nothing. */
    void leave_bridge() noexcept {
        if (bridge_ != nullptr && key_ != 0) {
            bridge_->remove(std::exchange(key_, 0));
        }
    }

    webcpp::asio::any_io_executor ex_;
    std::pmr::memory_resource* memory_;
    std::shared_ptr<handlers> h_;
    events events_;
    webcpp::asio::browser::bridge* bridge_;
    std::uint64_t key_;
    val socket_ = val::undefined();
};

/**
 * AES-GCM through `crypto.subtle`, under a key imported once, as Trystero's `encrypt` and `decrypt`
 * do.
 *
 * Each Web Crypto promise is awaited with webcpp::asio's `browser::async_await`, which posts its
 * settlement to the executor. The text is copied into JavaScript during the call; an operation that
 * arrives while the key is still being imported waits with a copy of its text, in the room's
 * resource. Without `crypto.subtle`, outside a secure context, every operation fails.
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
     * @param ex The executor its completions run on, an io_context's of webcpp::asio, which must
     * outlive the sealer's pending promises.
     * @param key The raw AES-256 key, `protocol::seal_key`'s.
     * @param memory The room's resource, which its texts, its waiting operations and its
     * completions are allocated in.
     */
    sealer(const webcpp::asio::any_io_executor& ex, std::array<std::uint8_t, 32> key,
           std::pmr::memory_resource* memory)
        : ex_(ex), raw_key_(key), memory_(memory), waiting_(memory) {}

    /**
     * Seals a text under a fresh random IV, with `crypto.subtle.encrypt`.
     *
     * @param plaintext The text, read during the call.
     * @param done Called with the sealed text, or with `error::backend_failure` when the key
     * cannot be imported, the page has no entropy for the IV, or the encryption fails.
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
    using waiting_operation = handler<void(sealer&, bool)>;

    [[nodiscard]] std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    /** Calls `done` with `failure` after the call, on the executor. */
    void fail(done_handler done, trystero::error failure) {
        trystero::detail::post(ex_, memory_, [done = std::move(done), failure]() mutable {
            done(make_error_code(failure), {});
        });
    }

    void encrypt(std::string_view plaintext, done_handler done) {
        std::array<std::uint8_t, protocol::seal_iv_size> iv{};
        if (getentropy(iv.data(), iv.size()) != 0) {  // lint-world: a seal's IV
            fail(std::move(done), error::backend_failure);
            return;
        }
        webcpp::asio::browser::async_await(
            ex_,
            detail::take(detail::js_crypt(false, key_.as_handle(), iv.data(), iv.size(),
                                          plaintext.data(), plaintext.size())),
            webcpp::asio::bind_allocator(
                allocator(), [iv, done = std::move(done), memory = memory_](
                                 boost::system::error_code ec, const val& cipher) mutable {
                    if (ec) {
                        done(make_error_code(error::backend_failure), {});
                        return;
                    }
                    const auto sealed =
                        protocol::format_sealed(iv, detail::from_js(cipher, memory), memory);
                    done({}, sealed);
                }));
    }

    void decrypt(std::string_view sealed, done_handler done) {
        const auto parts = protocol::parse_sealed(sealed, memory_);
        if (!parts || parts->iv.empty()) {
            fail(std::move(done), error::wrong_password);
            return;
        }
        webcpp::asio::browser::async_await(
            ex_,
            detail::take(detail::js_crypt(true, key_.as_handle(), parts->iv.data(),
                                          parts->iv.size(), parts->ciphertext.data(),
                                          parts->ciphertext.size())),
            webcpp::asio::bind_allocator(
                allocator(), [done = std::move(done), memory = memory_](
                                 boost::system::error_code ec, const val& plain) mutable {
                    if (ec) {
                        done(make_error_code(error::wrong_password), {});
                        return;
                    }
                    const auto b = detail::from_js(plain, memory);
                    done({}, std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
                }));
    }

    /**
     * Runs `op` once the key is imported, or has failed to be: after the call in every case, so
     * that no completion runs inside the operation that started it.
     */
    void wait_for_key(waiting_operation op) {
        const std::weak_ptr<sealer> weak = weak_from_this();
        if (state_ == key_state::failed) {
            trystero::detail::post(ex_, memory_, [weak, op = std::move(op)]() mutable {
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
        webcpp::asio::browser::async_await(
            ex_, detail::take(detail::js_import_key(raw_key_.data(), raw_key_.size())),
            webcpp::asio::bind_allocator(allocator(),
                                         [weak](boost::system::error_code ec, const val& key) {
                                             if (const auto self = weak.lock()) {
                                                 self->key_imported(!ec, key);
                                             }
                                         }));
    }

    void key_imported(bool ok, const val& key) {
        state_ = ok ? key_state::ready : key_state::failed;
        key_ = key;
        std::pmr::deque<waiting_operation> ready(allocator());
        ready.swap(waiting_);
        for (auto& op : ready) {
            op(*this, ok);
        }
    }

    enum class key_state : std::uint8_t { none, importing, ready, failed };

    webcpp::asio::any_io_executor ex_;
    std::array<std::uint8_t, 32> raw_key_;
    std::pmr::memory_resource* memory_;
    key_state state_ = key_state::none;
    val key_ = val::undefined();
    std::pmr::deque<waiting_operation> waiting_;
};

/**
 * The browser's factory: one per page, over the executor of the io_context the program runs.
 *
 * It makes peer connections, data channels and received frames in `app_config::memory`, and
 * WebSockets, sealers and room timers in the room's effective `room_options::memory`. Objects,
 * handlers and their events use the resource supplied to their factory method. What
 * `emscripten::val` holds is in the JavaScript heap. It needs a secure context, where
 * `crypto.subtle` exists.
 *
 * Tip: every object it makes runs on its executor, whose io_context the program runs from `main`
 * (`io.run()`, under a work guard) for as long as the page lives: the page's events, the timers
 * and the promises it awaits post to that io_context, and run only while it runs. The app is
 * given an executor of the same io_context.
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
     * Constructs the page's backend over `ex`, with a webcpp::webrtc runtime of its own: in the
     * browser the page's census counts the connections of every runtime alike.
     *
     * @param ex The executor every object of the backend runs on, an io_context's of
     * webcpp::asio, which must outlive the backend and every object it makes.
     */
    explicit backend(webcpp::asio::any_io_executor ex)
        : ex_(std::move(ex)),
          runtime_(std::allocate_shared<::webcpp::webrtc::runtime>(
              std::pmr::polymorphic_allocator<>(std::pmr::get_default_resource()),
              ::webcpp::webrtc::runtime_options{
                  .max_peer_connections = max_peer_connections,
                  .thread_pool_size = 0,
              })) {}

    /**
     * The executor every object of the backend runs on.
     *
     * @return The executor it was constructed with.
     */
    [[nodiscard]] const webcpp::asio::any_io_executor& get_executor() const noexcept { return ex_; }

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
        return std::allocate_shared<websocket>(std::pmr::polymorphic_allocator<>(memory), ex_, url,
                                               std::move(h), memory);
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
        auto made = webrtc::peer_connection::make(runtime_, ex_, servers, {}, *this, memory);
        return made ? *made : nullptr;
    }

    /**
     * Makes a timer, over webcpp::asio's `steady_timer`.
     *
     * @param memory The room's resource, which the timer and its waits are allocated in.
     * @return The timer, not started.
     */
    std::shared_ptr<trystero::backend::timer> make_timer(
        std::pmr::memory_resource* memory) override {
        return std::allocate_shared<trystero::detail::timer>(
            std::pmr::polymorphic_allocator<>(memory), ex_, memory);
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
        return std::allocate_shared<sealer>(std::pmr::polymorphic_allocator<>(memory), ex_, key,
                                            memory);
    }

    /**
     * Fills `out` with the page's entropy, which Emscripten draws from `crypto.getRandomValues`,
     * and ends the program when the page has none, as the native backend does.
     *
     * @param out The bytes to fill.
     */
    void random(std::span<std::uint8_t> out) override {
        for (std::size_t at = 0; at < out.size(); at += 256) {
            // Each call fills at most 256 bytes.
            const auto piece = std::min<std::size_t>(256, out.size() - at);
            if (getentropy(out.data() + at, piece) != 0) {  // lint-world: Trystero's randomness
                std::abort();                               // no secure randomness: nothing is safe
            }
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
    webcpp::asio::any_io_executor ex_;
    std::shared_ptr<::webcpp::webrtc::runtime> runtime_;
};

}  // namespace webcpp::trystero::browser

#endif
