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
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/platform/entropy.hpp>
#include <webcpp/trystero/platform/sealer.hpp>
#include <webcpp/trystero/platform/websocket.hpp>
#include <webcpp/trystero/webrtc/peer_connection.hpp>

#include <webcpp/webrtc/error.hpp>
#include <webcpp/webrtc/options.hpp>
#include <webcpp/webrtc/runtime.hpp>

#include <webcpp/asio/any_io_executor.hpp>  // lint-world: names the executor only
#include <webcpp/asio/ssl.hpp>              // lint-world: the relays' TLS

#include <boost/system/result.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * The platform: one implementation of the backend interface for both targets, over webcpp::webrtc
 * for WebRTC, webcpp::beast for the relays' WebSockets and webcpp::asio for timers; natively
 * libdatachannel's WebRTC, Boost.Beast's WebSockets and OpenSSL's TLS, AES-GCM and randomness, in a
 * page its own `RTCPeerConnection`, `WebSocket` and `crypto.subtle`. Everything it reports runs on
 * the executor it is given, whose io_context the program runs.
 */
namespace webcpp::trystero::platform {

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
 * WebSockets, sealers and room timers in the room's effective `room_options::memory`; its own state
 * is in `backend_config::memory`. Objects and their handlers use the resource supplied to their
 * factory method. What libdatachannel and OpenSSL allocate stays theirs, and what
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
 * secure context (https, or http on localhost), where `crypto.subtle` exists, for its sealer.
 *
 * @see "The platform backend", in the guide.
 */
class backend : public trystero::backend::factory {
public:
    /**
     * A backend with a runtime of its own, made with `backend_config::runtime`, whose TLS verifies
     * relays against the system's certificate authorities, as @ref make_tls_context makes it.
     *
     * It throws nothing: when OpenSSL cannot make the TLS context, `tls_context()` holds the error,
     * and every `wss://` relay reports a close, as an unreachable relay does.
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
     * @param ex The executor everything the backend reports runs on.
     * @param tls The TLS context of every `wss://` connection, whose authorities each relay is
     * verified against, whatever its verify mode; or the error that kept the program from making
     * it, which `tls_context()` then holds, every `wss://` relay reporting a close.
     * @param config What its peer connections, its relays and its own state are made with.
     */
    backend(webcpp::asio::any_io_executor ex, boost::system::result<webcpp::asio::ssl::context> tls,
            const backend_config& config = {})
        : backend(std::move(ex), std::move(tls), nullptr, config) {}

    /**
     * A backend whose peer connections the program's runtime counts, and whose TLS verifies relays
     * against the system's certificate authorities, as the first constructor's does.
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
     * The executor everything the backend reports runs on.
     *
     * @return The executor it was constructed with.
     */
    [[nodiscard]] const webcpp::asio::any_io_executor& get_executor() const noexcept { return ex_; }

    /**
     * The TLS context of the relays' connections, which an application may still configure before
     * the backend connects to a relay.
     *
     * @return The context every `wss://` connection of the backend is made with, or the error that
     * kept it from being made.
     */
    boost::system::result<webcpp::asio::ssl::context&> tls_context() {
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
     * @param memory The app's resource, which the connection, its channels, its handlers and its
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
        auto made = webrtc::peer_connection::make(runtime_, ex_, servers, config_.bind_address,
                                                  *this, memory);
#ifndef WEBCPP_TARGET_EMSCRIPTEN
        if (!made && made.error() == ::webcpp::webrtc::error::syntax_error) {
            made = webrtc::peer_connection::make(runtime_, ex_, parsed(servers, memory),
                                                 config_.bind_address, *this, memory);
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
     * Makes a sealer: natively over OpenSSL's AES-256-GCM, in a page over `crypto.subtle`.
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
     * Fills `out` from the platform's secure source of randomness, natively OpenSSL's generator,
     * in a page its `crypto.getRandomValues`; the program aborts when it has none.
     *
     * @param out The bytes to fill.
     */
    void random(std::span<std::uint8_t> out) override {
        if (!detail::fill_random(out)) {
            std::abort();  // no secure randomness: nothing is safe
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
    std::chrono::steady_clock::time_point steady_now() override {
        return std::chrono::steady_clock::now();  // lint-world: Trystero's timing
    }

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
};

}  // namespace webcpp::trystero::platform

#endif
