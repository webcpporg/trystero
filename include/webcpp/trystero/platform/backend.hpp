// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PLATFORM_BACKEND_HPP
#define WEBCPP_TRYSTERO_PLATFORM_BACKEND_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/construction_window.hpp>
#include <webcpp/trystero/detail/timer.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/platform/websocket.hpp>
#include <webcpp/trystero/protocol/room_keys.hpp>
#include <webcpp/trystero/webrtc/peer_connection.hpp>

#include <webcpp/webrtc/error.hpp>
#include <webcpp/webrtc/options.hpp>
#include <webcpp/webrtc/runtime.hpp>

#include <webcpp/crypto/random_device.hpp>  // lint-world: the backend's randomness
#include <webcpp/hash/async.hpp>
#include <webcpp/hash/hmac.hpp>
#include <webcpp/trystero/detail/wipe.hpp>

#include <webcpp/asio/any_io_executor.hpp>  // lint-world: names the executor only
#include <webcpp/asio/bind_allocator.hpp>   // lint-world: binds an allocator only
#include <webcpp/asio/bind_executor.hpp>    // lint-world: binds an executor only
#include <webcpp/asio/ssl.hpp>              // lint-world: the relays' TLS
#ifndef WEBCPP_TARGET_EMSCRIPTEN
#include <webcpp/asio/thread_pool.hpp>  // lint-world: the thread a PBKDF2 runs on
#else
#include <webcpp/trystero/platform/page.hpp>

#include <emscripten/val.h>
#endif

#include <boost/system/result.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * The platform: one implementation of the backend interface for both targets, over webcpp::webrtc
 * for WebRTC, webcpp::beast for the relays' WebSockets, webcpp::asio for timers, webcpp::crypto
 * for randomness and webcpp::hash for a password's PBKDF2; natively libdatachannel's WebRTC,
 * Boost.Beast's WebSockets and OpenSSL's TLS, in a page its own `RTCPeerConnection`, `WebSocket`
 * and `crypto.subtle`. Everything it reports runs on the executor it is given, whose io_context
 * the program runs.
 */
namespace webcpp::trystero::platform {

#ifdef WEBCPP_TARGET_EMSCRIPTEN
namespace detail {

/**
 * The page's `online` and `offline` events, a listener of each on the page's global object, each
 * of which posts `changed` with `true` or `false` to the executor; the listeners are removed when
 * it is destroyed, and a call posted then reaches nothing.
 */
class connectivity_watch {
public:
    /** What each listener runs, on the executor. */
    struct event {
        /** The watcher's handler, which both listeners share. */
        std::shared_ptr<handler<void(bool)>> changed;
        /** Whether the event is `online`. */
        bool online = false;

        /** Calls the handler; the event itself is not read. */
        void operator()(const emscripten::val& /*event*/) const { (*changed)(online); }
    };

    /** Whether the page's global object takes listeners: a page's window or worker does, node's
     * global object does not. */
    static bool page_has_events() {
        return emscripten::val::global("addEventListener")
            .typeOf()
            .strictlyEquals(emscripten::val("function"));
    }

    /** Adds both listeners, whose calls go through the bridge of `ex`'s io_context. */
    connectivity_watch(const webcpp::asio::any_io_executor& ex, handler<void(bool)> changed,
                       std::pmr::memory_resource* memory)
        : changed_(std::allocate_shared<handler<void(bool)>>(
              std::pmr::polymorphic_allocator<>(memory), std::move(changed))),
          online_(ex, event{.changed = changed_, .online = true}, memory),
          offline_(ex, event{.changed = changed_, .online = false}, memory),
          online_function_(online_.function()),
          offline_function_(offline_.function()) {
        const auto page = emscripten::val::global();
        page.call<void>("addEventListener", emscripten::val("online"), online_function_);
        page.call<void>("addEventListener", emscripten::val("offline"), offline_function_);
    }

    /** Not copyable: the page holds its listeners. */
    connectivity_watch(const connectivity_watch&) = delete;
    /** Not copy-assignable. */
    connectivity_watch& operator=(const connectivity_watch&) = delete;

    /** Removes both listeners from the page. */
    ~connectivity_watch() {
        const auto page = emscripten::val::global();
        page.call<void>("removeEventListener", emscripten::val("online"), online_function_);
        page.call<void>("removeEventListener", emscripten::val("offline"), offline_function_);
    }

private:
    std::shared_ptr<handler<void(bool)>> changed_;
    page_call<event> online_;
    page_call<event> offline_;
    emscripten::val online_function_;
    emscripten::val offline_function_;
};

}  // namespace detail
#endif

/**
 * What a backend makes its peer connections, its relays' WebSockets and its own state with, beside
 * the app's `app_config::ice_servers`.
 *
 * @see "The platform backend", in the guide.
 */
struct backend_config {
    /**
     * The local address every peer connection binds to, as `"127.0.0.1"`; empty means every
     * interface but the loopback.
     *
     * Bound to the loopback, the peers of one machine connect with no other network, which a test
     * wants; a peer bound there reaches no other machine. Natively only: a page's
     * `RTCPeerConnection` cannot bind, and webcpp::webrtc refuses every connection a page asks it
     * to bind, so a page leaves it empty.
     */
    std::pmr::string bind_address = {};

    /**
     * The most peer connections the backend makes in any minute, for every app and room it serves
     * together: beyond, it makes none, and a room reports each negotiation it cannot start.
     *
     * Tip: a room makes at most 64 in any 57 s however its peers behave, so the default lets four
     * rooms do so at once; an application with more rooms raises it.
     */
    std::size_t max_peer_connections_per_minute = 256;

    /**
     * What the backend's own webcpp::webrtc runtime is made with: the most peer connections alive
     * at once, 256 by default, and, natively, libdatachannel's threads.
     *
     * A backend given the program's runtime does not use it. In a page the runtime's cap counts
     * every `RTCPeerConnection` of the page, every program of it together, from its construction
     * until the garbage collector frees it, as Chromium counts them against its 500: the default is
     * half of that, so that a closed connection still counts until a collection the page cannot ask
     * for, while the rest of Chromium's count stays free for what the page makes without a room.
     */
    ::webcpp::webrtc::runtime_options runtime = {};

    /** What the relays' WebSockets tolerate of a relay that stops reading or answering. */
    websocket_limits relay_limits = {};

    /**
     * The resource of the backend's own state: its copy of `bind_address`, its window of
     * constructions and its own runtime.
     *
     * It must not be null, and must outlive the backend and every connection of its runtime.
     *
     * What the backend makes for a room or an app uses the resource given with each call instead.
     */
    std::pmr::memory_resource* memory = std::pmr::get_default_resource();
};

/**
 * The platform's factory: one per application, beside the executor it runs on, natively or in a
 * page.
 *
 * It makes peer connections, data channels and received frames in `app_config::memory`, and
 * WebSockets and room timers in the room's effective `room_options::memory`; its own state is in
 * `backend_config::memory`. Objects and their handlers use the resource supplied to their factory
 * method, and a derivation its completion's allocator. What libdatachannel and OpenSSL allocate
 * stays theirs, as does the thread of `derive_password_master` natively, and what
 * `emscripten::val` holds is in the JavaScript heap.
 *
 * Its peer connections are webcpp::webrtc's, counted by a `webcpp::webrtc::runtime`: its own, or
 * the program's. Each of its connections and channels keeps its own runtime alive, so that one
 * which outlives the backend, in a handler the io_context still holds, does not outlive the
 * runtime. Natively a process holds one runtime at a time, since libdatachannel's state is the
 * process's: a program with several backends at once, or that uses webcpp::webrtc beside
 * trystero, makes one runtime and gives it to each; in a page the census counts the connections of
 * every runtime alike.
 *
 * Tip: in a page, every object it makes runs on its executor, whose io_context the program runs
 * from `main`, under a work guard, for as long as the page lives: the page's events, the timers and
 * the promises it awaits post to that io_context, and run only while it runs. A page needs a
 * secure context (https, or http on localhost), where `crypto.subtle` exists, for the PBKDF2 of a
 * room with a password.
 *
 * @see "The platform backend", in the guide.
 */
class backend : public trystero::backend::factory {
public:
    /**
     * A backend with a runtime of its own, made with `backend_config::runtime`, whose TLS context
     * is @ref make_tls_context's, which natively verifies relays against the system's certificate
     * authorities; in a page the browser verifies them against its own.
     *
     * It throws nothing: when OpenSSL cannot make the TLS context natively, `tls_context()` holds
     * the error, and every `wss://` relay reports a close, as an unreachable relay does.
     *
     * @param ex The executor everything the backend reports runs on, in a page an io_context's of
     * webcpp::asio, which must outlive the backend and every object it makes.
     * @param config What its peer connections, its relays and its own state are made with.
     */
    explicit backend(webcpp::asio::any_io_executor ex, const backend_config& config = {})
        : backend(std::move(ex), make_tls_context(), nullptr, config) {}

    /**
     * A backend with a runtime of its own whose relays' TLS is the program's: another set of
     * certificate authorities, as webcpp's embedded Mozilla roots, which
     * `webcpp::asio::make_tls_client_context()` trusts natively, or a private one.
     *
     * Each relay is verified whatever the context's verify mode, natively against the context's
     * authorities and in a page against the browser's; in a page a context that asks for what the
     * page cannot honour (authorities added, a client certificate or key, options, a method other
     * than `tls_client` or `sslv23_client`, or a verify callback of its own) has every `wss://`
     * relay report a close.
     *
     * @param ex The executor everything the backend reports runs on.
     * @param tls The TLS context of every `wss://` connection, or the error that kept the program
     * from making it, which `tls_context()` then holds, every `wss://` relay reporting a close.
     * @param config What its peer connections, its relays and its own state are made with.
     */
    backend(webcpp::asio::any_io_executor ex, boost::system::result<webcpp::asio::ssl::context> tls,
            const backend_config& config = {})
        : backend(std::move(ex), std::move(tls), nullptr, config) {}

    /**
     * A backend whose peer connections the program's runtime counts, and whose TLS context is
     * @ref make_tls_context's, as the first constructor's is.
     *
     * @param ex The executor everything the backend reports runs on.
     * @param runtime The program's runtime, which must outlive every connection of the backend,
     * and so the io_context that may hold one in a handler.
     * @param config What its peer connections, its relays and its own state are made with; its
     * `runtime` is not used.
     */
    backend(webcpp::asio::any_io_executor ex, ::webcpp::webrtc::runtime& runtime,
            const backend_config& config = {})
        : backend(std::move(ex), make_tls_context(), runtime, config) {}

    /**
     * A backend whose peer connections the program's runtime counts, and whose relays' TLS is the
     * program's.
     *
     * @param ex The executor everything the backend reports runs on.
     * @param tls The TLS context of every `wss://` connection, or the error that kept the program
     * from making it, as the second constructor takes it.
     * @param runtime The program's runtime, which must outlive every connection of the backend,
     * and so the io_context that may hold one in a handler.
     * @param config What its peer connections, its relays and its own state are made with; its
     * `runtime` is not used.
     */
    backend(webcpp::asio::any_io_executor ex, boost::system::result<webcpp::asio::ssl::context> tls,
            ::webcpp::webrtc::runtime& runtime, const backend_config& config = {})
        // An empty owner: the pointer does not own the program's runtime.
        : backend(std::move(ex), std::move(tls),
                  std::shared_ptr<::webcpp::webrtc::runtime>(
                      std::shared_ptr<::webcpp::webrtc::runtime>(), &runtime),
                  config) {}

    /**
     * Waits for the derivation its thread runs, natively, if any, then destroys the backend.
     *
     * The completion of that derivation is posted to the backend's executor, so its io_context
     * must still exist: a program destroys the backend before its io_context.
     */
    ~backend() override {
#ifndef WEBCPP_TARGET_EMSCRIPTEN
        if (pool_) {
            pool_->join();
        }
#endif
    }

    /** Not copyable: its rooms, connections and relays hold it by reference. */
    backend(const backend&) = delete;
    /** Not copy-assignable. */
    backend& operator=(const backend&) = delete;
    /** Not movable: its rooms, connections and relays hold it by reference. */
    backend(backend&&) = delete;
    /** Not move-assignable. */
    backend& operator=(backend&&) = delete;

    /**
     * The executor everything the backend reports runs on.
     *
     * @return The executor it was constructed with.
     */
    [[nodiscard]] const webcpp::asio::any_io_executor& get_executor() const noexcept { return ex_; }

    /**
     * The TLS context of the relays' connections, which an application may still configure before
     * the backend connects to a relay.
     *
     * Natively its authorities, its client certificate, its options and its protocol versions hold,
     * and its verify mode and verify callback do not: each connection verifies its relay, against
     * the URL's host. In a page the browser verifies each relay against its own authorities, and
     * what the context asks for that a page cannot honour (authorities added, a client certificate
     * or key, options, a verify callback of its own) has every `wss://` relay report a close.
     *
     * @return The context every `wss://` connection of the backend is made with, or the error that
     * kept it from being made.
     */
    [[nodiscard]] boost::system::result<webcpp::asio::ssl::context&> tls_context() {
        if (!tls_) {
            return tls_.error();
        }
        return *tls_;
    }

    /**
     * The runtime that counts the backend's peer connections.
     *
     * @return Its own, or the program's.
     */
    [[nodiscard]] ::webcpp::webrtc::runtime& runtime() const { return *runtime_; }

    /**
     * Opens a WebSocket to a relay over webcpp::beast, with @ref connect_websocket.
     *
     * @param url The relay's `ws://` or `wss://` URL, read during the call.
     * @param h What the WebSocket reports.
     * @param memory The room's resource, which the WebSocket, its handlers, its queued texts and
     * its operations are allocated in.
     * @return The WebSocket; one of a `wss://` URL reports a close when the backend has no TLS
     * context.
     */
    std::shared_ptr<trystero::backend::websocket> connect(
        std::string_view url, trystero::backend::websocket::handlers h,
        std::pmr::memory_resource* memory) override {
        return connect_websocket(ex_, tls_ ? &*tls_ : nullptr, url, std::move(h), memory,
                                 config_.relay_limits);
    }

    /**
     * Makes a peer connection of webcpp::webrtc, bound to `backend_config::bind_address`.
     *
     * A room has dropped every server WebRTC refuses when it joined. Natively a URL libdatachannel
     * still cannot parse, as a `stuns:` one, a scheme it does not know, is left out here; a page's
     * `RTCPeerConnection` refuses the connection for a server it cannot use.
     *
     * @param servers The STUN and TURN servers, read during the call.
     * @param memory The room's resource, which the connection, its channels, its handlers and its
     * texts are allocated in.
     * @return The connection, or null when the backend made
     * `backend_config::max_peer_connections_per_minute` in the last minute, when its runtime holds
     * as many connections as it allows, or when the platform refuses to make one.
     */
    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
        if (!constructions_.take(steady_now())) {
            return nullptr;
        }
        auto made =
            webrtc::peer_connection::make(runtime_, ex_, servers, config_.bind_address, memory);
#ifndef WEBCPP_TARGET_EMSCRIPTEN
        if (!made && made.error() == ::webcpp::webrtc::error::syntax_error) {
            made = webrtc::peer_connection::make(runtime_, ex_, parsed(servers, memory),
                                                 config_.bind_address, memory);
        }
#endif
        return made ? *made : nullptr;
    }

    /**
     * Makes a timer over webcpp::asio's steady timer, on the backend's executor.
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
     * Derives a room's PBKDF2 master off the room's thread: natively with
     * `webcpp::hash::async_pbkdf2` on a thread of the backend's own, made at the first call and
     * joined when the backend is destroyed; in a page through the page's WebCrypto
     * (`backends::webcrypto`), awaited on the backend's executor.
     *
     * @param password The password, in UTF-8; the caller's until `done` runs.
     * @param salt The salt; the caller's until `done` runs.
     * @param out Where the master is written; the caller's until `done` runs, zeroed on error.
     * @param done Called on the backend's executor, after the call returns, once the master is
     * written, or with `error::backend_failure`; the derivation's operation is allocated with its
     * allocator.
     */
    void derive_password_master(std::span<const std::uint8_t> password,
                                std::span<const std::uint8_t, 32> salt,
                                std::span<std::uint8_t, 32> out,
                                handler<void(boost::system::error_code)> done) override {
        // The pool's thread dispatches the completion through `ex_`, an `any_io_executor`, which
        // drops the allocator property: so nothing is allocated in `done`'s resource there, off
        // the room's thread. A concrete executor type would change that.
        const auto allocator = done.get_allocator();
        auto completion = webcpp::asio::bind_executor(
            ex_, webcpp::asio::bind_allocator(allocator, [out, done = std::move(done)](
                                                             boost::system::error_code ec) mutable {
                if (ec) {
                    webcpp::trystero::detail::wipe(out.data(), out.size());
                    done(make_error_code(error::backend_failure));
                    return;
                }
                done({});
            }));
#ifdef WEBCPP_TARGET_EMSCRIPTEN
        webcpp::hash::async_pbkdf2<webcpp::hash::hmac_sha2_256, webcpp::hash::backends::webcrypto>(
            ex_, out, password, salt, protocol::pbkdf2_iterations, std::move(completion));
#else
        if (!pool_) {
            pool_.emplace(1);
        }
        webcpp::hash::async_pbkdf2<webcpp::hash::hmac_sha2_256>(
            pool_->get_executor(), out, password, salt, protocol::pbkdf2_iterations,
            std::move(completion));
#endif
    }

    /**
     * Fills `out` from the system's generator, through webcpp::crypto, in a page its
     * `crypto.getRandomValues`; the program aborts when it has none.
     *
     * @param out The bytes to fill.
     */
    void random(std::span<std::uint8_t> out) override {
        if (!webcpp::crypto::random_device().fill(out)) {  // lint-world: Trystero's randomness
            std::abort();                                  // no secure randomness: nothing is safe
        }
    }

    /**
     * The standard library's wall clock, which a page reads from `Date.now()`, in whole seconds.
     *
     * @return The current time, in whole seconds since the Unix epoch.
     */
    std::chrono::sys_seconds unix_now() override {
        const auto now = std::chrono::system_clock::now();  // lint-world: Nostr's created_at
        return std::chrono::floor<std::chrono::seconds>(now);
    }

    /**
     * The standard library's steady clock, which a page reads from `performance.now()`.
     *
     * @return The current time, from the steady clock's epoch.
     */
    std::chrono::steady_clock::time_point steady_now() override {  // lint-world: a type only
        return std::chrono::steady_clock::now();                   // lint-world: Trystero's timing
    }

#ifdef WEBCPP_TARGET_EMSCRIPTEN
    /**
     * Watches the page's `online` and `offline` events, through listeners on its global object
     * whose calls post to the backend's executor; natively, where there are none, it watches
     * nothing, as the interface's default does.
     *
     * @param changed Called on the backend's executor after each event: `true` after `online`,
     * `false` after `offline`.
     * @param memory The app's resource, which the watch, its listeners' state and the calls they
     * post are allocated in.
     * @return The watch, whose destruction removes the listeners; null where the page's global
     * object takes no listener, as node's.
     */
    std::shared_ptr<void> watch_connectivity(handler<void(bool online)> changed,
                                             std::pmr::memory_resource* memory) override {
        if (!detail::connectivity_watch::page_has_events()) {
            return nullptr;
        }
        return std::allocate_shared<detail::connectivity_watch>(
            std::pmr::polymorphic_allocator<>(memory), ex_, std::move(changed), memory);
    }
#endif

private:
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    backend(webcpp::asio::any_io_executor ex, boost::system::result<webcpp::asio::ssl::context> tls,
            std::shared_ptr<::webcpp::webrtc::runtime> runtime, const backend_config& config)
        // NOLINTEND(performance-unnecessary-value-param)
        : ex_(std::move(ex)),
          config_(kept(config)),
          runtime_(runtime ? std::move(runtime) : own_runtime(config_)),
          constructions_(config_.max_peer_connections_per_minute, std::chrono::minutes(1),
                         config_.memory),
          tls_(std::move(tls)) {}

    /** The configuration the backend keeps, its bind address copied into its own memory. */
    static backend_config kept(const backend_config& config) {
        return {
            .bind_address = std::pmr::string(config.bind_address, config.memory),
            .max_peer_connections_per_minute = config.max_peer_connections_per_minute,
            .runtime = config.runtime,
            .relay_limits = config.relay_limits,
            .memory = config.memory,
        };
    }

    /** A runtime of the backend's own, in its memory, which its connections share with it. */
    static std::shared_ptr<::webcpp::webrtc::runtime> own_runtime(const backend_config& config) {
        return std::allocate_shared<::webcpp::webrtc::runtime>(
            std::pmr::polymorphic_allocator<>(config.memory), config.runtime);
    }

#ifndef WEBCPP_TARGET_EMSCRIPTEN
    /**
     * The servers of `servers` without the URLs libdatachannel cannot parse, in `memory`: each URL
     * is tried alone, so that one refused leaves the others.
     */
    std::pmr::vector<trystero::backend::ice_server> parsed(
        std::span<const trystero::backend::ice_server> servers, std::pmr::memory_resource* memory) {
        std::pmr::vector<trystero::backend::ice_server> out(memory);
        for (const auto& server : servers) {
            trystero::backend::ice_server kept{
                .urls = std::pmr::vector<std::pmr::string>(memory),
                .username = std::pmr::string(server.username, memory),
                .credential = std::pmr::string(server.credential, memory),
            };
            for (const auto& url : server.urls) {
                const trystero::backend::ice_server one{
                    .urls = std::pmr::vector<std::pmr::string>(1, url, memory),
                    .username = std::pmr::string(server.username, memory),
                    .credential = std::pmr::string(server.credential, memory),
                };
                // A URL it parses makes a connection, given up at once.
                const auto probe = webrtc::peer_connection::make_connection(
                    *runtime_, ex_, std::span(&one, 1), config_.bind_address, memory);
                if (probe || probe.error() != ::webcpp::webrtc::error::syntax_error) {
                    kept.urls.push_back(url);
                }
            }
            if (!kept.urls.empty()) {
                out.push_back(std::move(kept));
            }
        }
        return out;
    }
#endif

    webcpp::asio::any_io_executor ex_;
    backend_config config_;
    std::shared_ptr<::webcpp::webrtc::runtime> runtime_;
    trystero::detail::construction_window constructions_;
    boost::system::result<webcpp::asio::ssl::context> tls_;
#ifndef WEBCPP_TARGET_EMSCRIPTEN
    // The one thread of the derivations, made at the first.
    std::optional<webcpp::asio::thread_pool> pool_;
#endif
};

}  // namespace webcpp::trystero::platform

#endif
