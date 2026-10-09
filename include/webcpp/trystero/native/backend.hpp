// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_NATIVE_BACKEND_HPP
#define WEBCPP_TRYSTERO_NATIVE_BACKEND_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
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

/** An Asio timer whose waits are allocated in the room's resource. */
class timer : public trystero::backend::timer {
public:
    timer(const boost::asio::any_io_executor& ex, std::pmr::memory_resource* memory)
        : timer_(ex), memory_(memory) {}

    void start(std::chrono::milliseconds delay, handler<void()> fire) override {
        timer_.expires_after(delay);
        timer_.async_wait(boost::asio::bind_allocator(
            std::pmr::polymorphic_allocator<>(memory_),
            [fire = std::move(fire)](boost::system::error_code ec) mutable {
                if (!ec) {
                    fire();
                }
            }));
    }

    void cancel() override { timer_.cancel(); }

private:
    boost::asio::steady_timer timer_;  // lint-world: Trystero's timers
    std::pmr::memory_resource* memory_;
};

// The factory makes peer connections over libdatachannel, which need exceptions: without them
// (WEBCPP_TRYSTERO_NO_EXCEPTIONS) it does not exist, and the timer, the sealer and the WebSockets
// remain.
#ifndef WEBCPP_TRYSTERO_NO_EXCEPTIONS

/** What a native backend makes its peer connections with, beside the room's
 * `room_config::ice_servers`. */
struct backend_config {
    /**
     * The local address every peer connection binds to, as `"127.0.0.1"`; empty means every
     * interface but the loopback.
     *
     * Bound to the loopback, the peers of one machine connect with no other network, which a test
     * wants; a peer bound there reaches no other machine.
     */
    std::pmr::string bind_address = {};
};

/** The native factory: one per application, beside the executor it runs on. */
class backend : public trystero::backend::factory {
public:
    /**
     * A backend whose TLS verifies relays against the system's certificate authorities.
     *
     * @param ex The executor everything the backend reports runs on.
     * @param config What its peer connections are made with; the default binds them to no address.
     */
    explicit backend(boost::asio::any_io_executor ex, backend_config config = {})
        : ex_(std::move(ex)),
          config_(std::move(config)),
          tls_(boost::asio::ssl::context::tls_client) {
        tls_.set_default_verify_paths();
        tls_.set_verify_mode(boost::asio::ssl::verify_peer);
    }

    /** The TLS configuration of the relays' connections, for an application with authorities of its
     * own. */
    boost::asio::ssl::context& tls_context() { return tls_; }

    std::shared_ptr<trystero::backend::websocket> connect(
        std::string_view url, trystero::backend::websocket::handlers h,
        std::pmr::memory_resource* memory) override {
        return connect_websocket(ex_, tls_, url, std::move(h), memory);
    }

    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
        return peer_connection::create(ex_, servers, config_.bind_address, memory);
    }

    std::shared_ptr<trystero::backend::timer> make_timer(
        std::pmr::memory_resource* memory) override {
        return std::allocate_shared<timer>(std::pmr::polymorphic_allocator<>(memory), ex_, memory);
    }

    std::shared_ptr<trystero::backend::sealer> make_sealer(
        std::array<std::uint8_t, 32> key, std::pmr::memory_resource* memory) override {
        return std::allocate_shared<sealer>(std::pmr::polymorphic_allocator<>(memory), ex_, key,
                                            memory);
    }

    void random(std::span<std::uint8_t> out) override {
        const auto size = static_cast<int>(out.size());
        if (RAND_bytes(out.data(), size) != 1) {  // lint-world: the room's keys and ids
            std::abort();                         // no secure randomness: nothing is safe
        }
    }

    std::int64_t unix_seconds() override {
        const auto now = std::chrono::system_clock::now();  // lint-world: Nostr's created_at
        return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    }

    double now_ms() override {
        const auto now = std::chrono::steady_clock::now();  // lint-world: Trystero's timing
        return std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
    }

private:
    boost::asio::any_io_executor ex_;
    backend_config config_;
    boost::asio::ssl::context tls_;
};

#endif  // WEBCPP_TRYSTERO_NO_EXCEPTIONS

}  // namespace webcpp::trystero::native

#endif
