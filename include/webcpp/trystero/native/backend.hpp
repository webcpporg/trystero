// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_NATIVE_BACKEND_HPP
#define WEBCPP_TRYSTERO_NATIVE_BACKEND_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/native/peer_connection.hpp>
#include <webcpp/trystero/native/sealer.hpp>
#include <webcpp/trystero/native/websocket.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <openssl/rand.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

/**
 * The native target: the backend interface over libdatachannel for WebRTC, Boost.Beast for the relays' WebSockets,
 * OpenSSL for TLS and AES-GCM, and Asio for timers. Everything it reports runs on the executor it is given; the
 * application runs that executor's context, `io_context::run()` or otherwise.
 */
namespace webcpp::trystero::native {

class timer : public trystero::backend::timer {
public:
    explicit timer(boost::asio::any_io_executor ex) : timer_(std::move(ex)) {}

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

// The factory makes peer connections over libdatachannel, which need exceptions: without them
// (WEBCPP_TRYSTERO_NO_EXCEPTIONS) it does not exist, and the timer, the sealer and the WebSockets remain.
#ifndef WEBCPP_TRYSTERO_NO_EXCEPTIONS

/** What a native backend makes its peer connections with, beside the room's `room_config::ice_servers`. */
struct backend_config {
    /**
     * The local address every peer connection binds to, as `"127.0.0.1"`; empty means every interface but the loopback.
     *
     * Bound to the loopback, the peers of one machine connect with no other network, which a test wants; a peer bound
     * there reaches no other machine.
     */
    std::string bind_address = {};
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
        : ex_(std::move(ex)), config_(std::move(config)), tls_(boost::asio::ssl::context::tls_client) {
        tls_.set_default_verify_paths();
        tls_.set_verify_mode(boost::asio::ssl::verify_peer);
    }

    /** The TLS configuration of the relays' connections, for an application with authorities of its own. */
    boost::asio::ssl::context& tls_context() { return tls_; }

    std::shared_ptr<trystero::backend::websocket> connect(std::string_view url, trystero::backend::websocket::handlers h) override {
        return connect_websocket(ex_, tls_, url, std::move(h));
    }

    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(const std::vector<trystero::backend::ice_server>& servers) override {
        return peer_connection::create(ex_, servers, config_.bind_address);
    }

    std::unique_ptr<trystero::backend::timer> make_timer() override { return std::make_unique<timer>(ex_); }

    std::shared_ptr<trystero::backend::sealer> make_sealer(std::array<std::uint8_t, 32> key) override {
        return std::make_shared<sealer>(ex_, key);
    }

    void random(std::span<std::uint8_t> out) override {
        if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) std::abort();  // no secure randomness: nothing is safe
    }

    std::int64_t unix_seconds() override {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    double now_ms() override {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

private:
    boost::asio::any_io_executor ex_;
    backend_config config_;
    boost::asio::ssl::context tls_;
};

#endif  // WEBCPP_TRYSTERO_NO_EXCEPTIONS

}  // namespace webcpp::trystero::native

#endif
