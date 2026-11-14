// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_NATIVE_BACKEND_HPP
#define WEBCPP_TRYSTERO_NATIVE_BACKEND_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/construction_window.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/native/sealer.hpp>
#include <webcpp/trystero/native/timer.hpp>
#include <webcpp/trystero/native/websocket.hpp>
#include <webcpp/trystero/webrtc/peer_connection.hpp>

#include <webcpp/webrtc/error.hpp>
#include <webcpp/webrtc/options.hpp>
#include <webcpp/webrtc/runtime.hpp>

#include <webcpp/asio/any_io_executor.hpp>  // lint-world: posts handlers only

#include <openssl/rand.h>
#include <boost/asio/any_io_executor.hpp>  // lint-world: the relays' Boost.Beast WebSockets
#include <boost/asio/ssl/context.hpp>      // lint-world: the relays' TLS

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
#include <vector>

/**
 * The native target: the backend interface over webcpp::webrtc for WebRTC, natively
 * libdatachannel's, Boost.Beast for the relays' WebSockets, OpenSSL for TLS and AES-GCM, and Asio
 * for timers. Everything it reports runs on the executor it is given; the application runs that
 * executor's context, blocking on it or polling it.
 */
namespace webcpp::trystero::native {

/**
 * What a native backend makes its peer connections with, beside the app's
 * `app_config::ice_servers`.
 *
 * @see "The native backend", in the guide.
 */
struct backend_config {
    /**
     * The local address every peer connection binds to, as `"127.0.0.1"`; empty means every
     * interface but the loopback.
     *
     * Bound to the loopback, the peers of one machine connect with no other network, which a test
     * wants; a peer bound there reaches no other machine.
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
     * at once, 256 by default, and libdatachannel's threads.
     *
     * A backend given the program's runtime does not use it.
     */
    ::webcpp::webrtc::runtime_options runtime = {};

    /** What the relays' WebSockets tolerate of a relay that stops reading or answering. */
    websocket_limits relay_limits = {};
};

/**
 * The native factory: one per application, beside the executor it runs on.
 *
 * It makes peer connections, data channels and received frames in `app_config::memory`, and
 * WebSockets, sealers and room timers in the room's effective `room_options::memory`. Objects
 * and their handlers use the resource supplied to their factory method. What libdatachannel and
 * OpenSSL allocate stays theirs.
 *
 * Its peer connections are webcpp::webrtc's, counted by a `webcpp::webrtc::runtime`: its own, or
 * the program's. Each of its connections and channels keeps its own runtime alive, so that one
 * which outlives the backend, in a handler the io_context still holds, does not outlive the
 * runtime. A process holds one runtime at a time, since libdatachannel's state is the process's:
 * a program with several native backends at once, or that uses webcpp::webrtc beside trystero,
 * makes one runtime and gives it to each.
 *
 * @see "The native backend", in the guide.
 */
class backend : public trystero::backend::factory {
public:
    /**
     * A backend with a runtime of its own, made with `backend_config::runtime`, whose TLS verifies
     * relays against the system's certificate authorities.
     *
     * @param ex The executor everything the backend reports runs on.
     * @param config What its peer connections are made with; the default binds them to no address.
     * @throws std::exception A `boost::system::system_error` when OpenSSL cannot make the TLS
     * context or load the system's certificate authorities; without exceptions, Boost's
     * `throw_exception`, which the program defines, is called instead.
     */
    explicit backend(webcpp::asio::any_io_executor ex, backend_config config = {})
        : backend(std::move(ex), nullptr, std::move(config)) {}

    /**
     * A backend whose peer connections the program's runtime counts.
     *
     * @param ex The executor everything the backend reports runs on.
     * @param runtime The program's runtime, which must outlive every connection of the backend,
     * and so the io_context that may hold one in a handler.
     * @param config What its peer connections are made with; its `runtime` is not used.
     * @throws std::exception As the other constructor.
     */
    backend(webcpp::asio::any_io_executor ex, ::webcpp::webrtc::runtime& runtime,
            backend_config config = {})
        // An empty owner: the pointer does not own the program's runtime.
        : backend(std::move(ex),
                  std::shared_ptr<::webcpp::webrtc::runtime>(
                      std::shared_ptr<::webcpp::webrtc::runtime>(), &runtime),
                  std::move(config)) {}

    /**
     * The TLS configuration of the relays' connections, for an application with authorities of its
     * own.
     *
     * @return The context every `wss://` connection of the backend is made with.
     */
    boost::asio::ssl::context& tls_context() { return tls_; }

    /**
     * The runtime that counts the backend's peer connections.
     *
     * @return Its own, or the program's.
     */
    [[nodiscard]] ::webcpp::webrtc::runtime& runtime() const { return *runtime_; }

    /**
     * Opens a WebSocket to a relay over Boost.Beast, with @ref connect_websocket.
     *
     * @param url The relay's `ws://` or `wss://` URL, read during the call.
     * @param h What the WebSocket reports.
     * @param memory The room's resource, which the WebSocket, its handlers and its queued texts are
     * allocated in.
     * @return The WebSocket.
     */
    std::shared_ptr<trystero::backend::websocket> connect(
        std::string_view url, trystero::backend::websocket::handlers h,
        std::pmr::memory_resource* memory) override {
        return connect_websocket(relay_executor(), tls_, url, std::move(h), memory,
                                 config_.relay_limits);
    }

    /**
     * Makes a peer connection of webcpp::webrtc, bound to `backend_config::bind_address`.
     *
     * A room has dropped every server WebRTC refuses when it joined; a URL libdatachannel still
     * cannot parse, as a `stuns:` one, a scheme it does not know, is left out here, where a
     * browser refuses the connection.
     *
     * @param servers The STUN and TURN servers, read during the call.
     * @param memory The app's resource, which the connection, its channels, its handlers and its
     * texts are allocated in.
     * @return The connection, or null when the backend made
     * `backend_config::max_peer_connections_per_minute` in the last minute, when its runtime holds
     * as many connections as it allows, or when libdatachannel refuses to make one.
     */
    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
        if (!constructions_.take(now_ms())) {
            return nullptr;
        }
        auto made = webrtc::peer_connection::make(runtime_, ex_, servers, config_.bind_address,
                                                  *this, memory);
        if (!made && made.error() == ::webcpp::webrtc::error::syntax_error) {
            made = webrtc::peer_connection::make(runtime_, ex_, parsed(servers, memory),
                                                 config_.bind_address, *this, memory);
        }
        return made ? *made : nullptr;
    }

    /**
     * Makes an Asio timer on the backend's executor.
     *
     * @param memory The room's resource, which the timer and its waits are allocated in.
     * @return The timer, not started.
     */
    std::shared_ptr<trystero::backend::timer> make_timer(
        std::pmr::memory_resource* memory) override {
        return std::allocate_shared<timer>(std::pmr::polymorphic_allocator<>(memory), ex_, memory);
    }

    /**
     * Makes a sealer over OpenSSL's AES-256-GCM.
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
     * Fills `out` from OpenSSL's random generator; the program aborts when it has no secure
     * randomness.
     *
     * @param out The bytes to fill.
     */
    void random(std::span<std::uint8_t> out) override {
        const auto size = static_cast<int>(out.size());
        if (RAND_bytes(out.data(), size) != 1) {  // lint-world: Trystero's randomness
            std::abort();                         // no secure randomness: nothing is safe
        }
    }

    /**
     * Seconds since the Unix epoch, from the system's wall clock.
     *
     * @return The current time, in whole seconds.
     */
    std::int64_t unix_seconds() override {
        const auto now = std::chrono::system_clock::now();  // lint-world: Nostr's created_at
        return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    }

    /**
     * Milliseconds from the standard library's steady clock.
     *
     * @return The current time, in milliseconds since the clock's epoch.
     */
    double now_ms() override {
        const auto now = std::chrono::steady_clock::now();  // lint-world: Trystero's timing
        return std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
    }

private:
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    backend(webcpp::asio::any_io_executor ex, std::shared_ptr<::webcpp::webrtc::runtime> runtime,
            backend_config config)
        // NOLINTEND(performance-unnecessary-value-param)
        : ex_(std::move(ex)),
          config_(std::move(config)),
          runtime_(runtime ? std::move(runtime) : own_runtime(config_.runtime)),
          constructions_(config_.max_peer_connections_per_minute, std::chrono::minutes(1),
                         std::pmr::get_default_resource()),
          tls_(boost::asio::ssl::context::tls_client) {
        tls_.set_default_verify_paths();
        tls_.set_verify_mode(boost::asio::ssl::verify_peer);
    }

    /**
     * The executor of the relays' WebSockets, which are Boost.Beast's and take Boost.Asio's.
     *
     * Natively webcpp::asio is Boost.Asio, and the two executors are one: the backend builds
     * natively alone. The reference of the guide reads every header for Emscripten's target,
     * where webcpp::asio is its own and the backend is never built: it names no executor there.
     */
    [[nodiscard]] boost::asio::any_io_executor relay_executor() const {
#ifdef WEBCPP_TARGET_NATIVE
        return ex_;
#else
        return {};
#endif
    }

    /** A runtime of the backend's own, which its connections share with it. */
    static std::shared_ptr<::webcpp::webrtc::runtime> own_runtime(
        const ::webcpp::webrtc::runtime_options& options) {
        return std::allocate_shared<::webcpp::webrtc::runtime>(
            std::pmr::polymorphic_allocator<>(std::pmr::get_default_resource()), options);
    }

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

    webcpp::asio::any_io_executor ex_;
    backend_config config_;
    std::shared_ptr<::webcpp::webrtc::runtime> runtime_;
    trystero::detail::construction_window constructions_;
    boost::asio::ssl::context tls_;
};

}  // namespace webcpp::trystero::native

#endif
