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
#include <webcpp/trystero/native/peer_connection.hpp>
#include <webcpp/trystero/native/sealer.hpp>
#include <webcpp/trystero/native/websocket.hpp>

#include <openssl/rand.h>
#include <boost/asio/any_io_executor.hpp>  // lint-world: posts handlers only
#include <boost/asio/bind_allocator.hpp>   // lint-world: posts handlers only
#include <boost/asio/ssl/context.hpp>      // lint-world: the relays' TLS
#include <boost/asio/steady_timer.hpp>     // lint-world: Trystero's timers

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <string>

/**
 * The native target: the backend interface over libdatachannel for WebRTC, Boost.Beast for the
 * relays' WebSockets, OpenSSL for TLS and AES-GCM, and Asio for timers. Everything it reports runs
 * on the executor it is given; the application runs that executor's context, blocking on it or
 * polling it.
 */
namespace webcpp::trystero::native {

/**
 * An Asio timer whose waits are allocated in the room's resource.
 *
 * Tip: Asio cannot take back an expiry it has already queued, which it then completes as a
 * success; so each wait carries its number, and `cancel`, a later `start` and the destructor end
 * it, by changing the number or freeing it, before its handler runs.
 *
 * @see "The native backend", in the guide.
 */
class timer : public trystero::backend::timer {
public:
    /**
     * Constructs a timer, not started.
     *
     * @param ex The executor it fires on.
     * @param memory The room's resource, which each wait is allocated in; it must outlive the
     * timer's pending waits.
     */
    timer(const boost::asio::any_io_executor& ex, std::pmr::memory_resource* memory)
        : timer_(ex),
          memory_(memory),
          waits_(
              std::allocate_shared<std::uint64_t>(std::pmr::polymorphic_allocator<>(memory), 0)) {}

    /**
     * Calls `fire` on the executor once `delay` has passed, in place of a pending expiry.
     *
     * @param delay How long to wait.
     * @param fire What to call when the timer expires; never called once the timer is cancelled,
     * started again or destroyed, even when its expiry was already queued.
     */
    void start(std::chrono::milliseconds delay, handler<void()> fire) override {
        const std::uint64_t wait = ++*waits_;
        timer_.expires_after(delay);
        timer_.async_wait(boost::asio::bind_allocator(
            std::pmr::polymorphic_allocator<>(memory_),
            [waits = std::weak_ptr<std::uint64_t>(waits_), wait,
             fire = std::move(fire)](boost::system::error_code ec) mutable {
                if (const auto current = waits.lock(); !ec && current && *current == wait) {
                    fire();
                }
            }));
    }

    /** Cancels the pending expiry, if any, whose handler is then never called. */
    void cancel() override {
        ++*waits_;
        timer_.cancel();
    }

private:
    boost::asio::steady_timer timer_;  // lint-world: Trystero's timers
    std::pmr::memory_resource* memory_;
    // The number of the wait under way: a handler that finds another, or none, does nothing.
    std::shared_ptr<std::uint64_t> waits_;
};

// The factory makes peer connections over libdatachannel, which need exceptions: without them
// (WEBCPP_TRYSTERO_NO_EXCEPTIONS) it does not exist, and the timer, the sealer and the WebSockets
// remain.
#ifndef WEBCPP_TRYSTERO_NO_EXCEPTIONS

/**
 * What a native backend makes its peer connections with, beside the room's
 * `room_config::ice_servers`.
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
     * The most peer connections the backend makes in any minute, for every room it serves
     * together: beyond, it makes none, and a room reports each negotiation it cannot start.
     *
     * Tip: a room makes at most 64 in any 57 s however its peers behave, so the default lets four
     * rooms do so at once; an application with more rooms raises it.
     */
    std::size_t max_peer_connections_per_minute = 256;

    /** What the relays' WebSockets tolerate of a relay that stops reading or answering. */
    websocket_limits relay_limits = {};
};

/**
 * The native factory: one per application, beside the executor it runs on.
 *
 * It makes each object in the memory resource the room gives it, `room_config::memory`: the
 * WebSockets, peer connections, data channels, timers and sealers, their handlers, and the frames
 * and texts they hand to the room. What libdatachannel and OpenSSL allocate stays theirs. Its
 * peer connections need exceptions: without them (`WEBCPP_TRYSTERO_NO_EXCEPTIONS`) it does not
 * exist.
 *
 * @see "The native backend", in the guide.
 */
class backend : public trystero::backend::factory {
public:
    /**
     * A backend whose TLS verifies relays against the system's certificate authorities.
     *
     * @param ex The executor everything the backend reports runs on.
     * @param config What its peer connections are made with; the default binds them to no address.
     * @throws std::exception A `boost::system::system_error` when OpenSSL cannot make the TLS
     * context or load the system's certificate authorities.
     */
    explicit backend(boost::asio::any_io_executor ex, backend_config config = {})
        : ex_(std::move(ex)),
          config_(std::move(config)),
          constructions_(config_.max_peer_connections_per_minute, std::chrono::minutes(1),
                         std::pmr::get_default_resource()),
          tls_(boost::asio::ssl::context::tls_client) {
        tls_.set_default_verify_paths();
        tls_.set_verify_mode(boost::asio::ssl::verify_peer);
    }

    /**
     * The TLS configuration of the relays' connections, for an application with authorities of its
     * own.
     *
     * @return The context every `wss://` connection of the backend is made with.
     */
    boost::asio::ssl::context& tls_context() { return tls_; }

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
        return connect_websocket(ex_, tls_, url, std::move(h), memory, config_.relay_limits);
    }

    /**
     * Makes a peer connection of libdatachannel, bound to `backend_config::bind_address`.
     *
     * @param servers The STUN and TURN servers, read during the call.
     * @param memory The room's resource, which the connection, its channels, its handlers and its
     * texts are allocated in.
     * @return The connection, or null when the backend made
     * `backend_config::max_peer_connections_per_minute` in the last minute, or when libdatachannel
     * refuses to make one, by throwing.
     */
    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
        if (!constructions_.take(now_ms())) {
            return nullptr;
        }
        try {
            return peer_connection::create(ex_, servers, config_.bind_address, memory);
        } catch (...) {
            return nullptr;
        }
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
    boost::asio::any_io_executor ex_;
    backend_config config_;
    trystero::detail::construction_window constructions_;
    boost::asio::ssl::context tls_;
};

#endif  // WEBCPP_TRYSTERO_NO_EXCEPTIONS

}  // namespace webcpp::trystero::native

#endif
