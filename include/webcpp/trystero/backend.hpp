// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_BACKEND_HPP
#define WEBCPP_TRYSTERO_BACKEND_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/handler.hpp>

#include <boost/system/error_code.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * The seam between the room and a platform: what a target must provide for a room to run on it. The
 * browser provides it over `emscripten::val` (webcpp/trystero/browser), a native target over
 * libdatachannel, Boost.Beast and OpenSSL (webcpp/trystero/native), and the tests over memory
 * (test/support).
 *
 * Tip: every handler a backend invokes runs on the room's executor, one at a time, and may destroy
 * the object that invoked it. A backend whose events arrive on other threads (libdatachannel) posts
 * them there first.
 *
 * Text crosses this interface as `std::string_view`, valid for the duration of the call or of the
 * handler: a backend reads what it is given before returning, and a handler copies what it keeps.
 * The one exception is a data channel message, handed over by value: the receiver keeps the frame,
 * and taking ownership of it is what spares a copy.
 *
 * Tip: every method that makes an object takes the room's memory resource last, and the backend
 * allocates the object, its handlers and what it hands to the room (a frame, a text) through it, on
 * the room's executor only.
 */
namespace webcpp::trystero::backend {

using bytes = std::pmr::vector<std::uint8_t>;
using error_code = boost::system::error_code;

/** A STUN or TURN server; a room copies those of its configuration into its own resource. */
struct ice_server {
    std::pmr::vector<std::pmr::string> urls;
    std::pmr::string username;
    std::pmr::string credential;
};

/** A one-shot timer; starting it again replaces the pending expiry, and destroying it cancels it.
 */
class timer {
public:
    virtual ~timer() = default;
    virtual void start(std::chrono::milliseconds delay, handler<void()> fire) = 0;
    virtual void cancel() = 0;
};

class websocket {
public:
    struct handlers {
        handler<void()> open;
        handler<void(std::string_view)> message;
        handler<void()> close;  // also when it never opened
    };

    virtual ~websocket() = default;
    virtual void send(std::string_view text) = 0;
    virtual void close() = 0;
};

class data_channel {
public:
    struct handlers {
        handler<void()> open;
        handler<void(bytes)> message;
        handler<void()> close;
        handler<void()> buffered_amount_low;  // below `low_threshold` after being above it
    };

    /** Trystero's backpressure threshold: senders wait while more than 64 KiB is buffered. */
    static constexpr std::size_t low_threshold = 0xffff;

    virtual ~data_channel() = default;
    virtual void set_handlers(handlers h) = 0;
    virtual bool is_open() const = 0;
    virtual void send(std::span<const std::uint8_t> frame) = 0;
    virtual std::size_t buffered_amount() const = 0;
    virtual void close() = 0;
};

enum class connection_state { fresh, connecting, connected, disconnected, failed, closed };

class peer_connection {
public:
    struct handlers {
        handler<void(std::string_view)> ice_candidate;  // the JSON of an RTCIceCandidateInit
        handler<void(std::shared_ptr<data_channel>)> data_channel;
        handler<void(connection_state)> state;
    };

    virtual ~peer_connection() = default;
    virtual void set_handlers(handlers h) = 0;
    /** A data channel, made in the connection's resource. */
    virtual std::shared_ptr<data_channel> create_data_channel(std::string_view label) = 0;
    /**
     * Creates the offer or the answer the signaling state calls for and applies it, like
     * `setLocalDescription()`; `done` sees its type ("offer" or "answer") and SDP.
     */
    virtual void set_local_description(
        handler<void(error_code, std::string_view type, std::string_view sdp)> done) = 0;
    virtual void set_remote_description(std::string_view type, std::string_view sdp,
                                        handler<void(error_code)> done) = 0;
    virtual void add_ice_candidate(std::string_view candidate_json,
                                   handler<void(error_code)> done) = 0;
    virtual bool has_remote_description() const = 0;
    virtual void close() = 0;
};

/** AES-GCM under one room key, in Trystero's sealed text form (protocol/signal.hpp). */
class sealer {
public:
    virtual ~sealer() = default;
    virtual void seal(std::string_view plaintext,
                      handler<void(error_code, std::string_view sealed)> done) = 0;
    /** Completes with `trystero::error::wrong_password` when the text was sealed under another key.
     */
    virtual void open(std::string_view sealed,
                      handler<void(error_code, std::string_view plaintext)> done) = 0;
};

/** Makes everything above; one per page in the browser, one per test. */
class factory {
public:
    virtual ~factory() = default;
    virtual std::shared_ptr<websocket> connect(std::string_view url, websocket::handlers h,
                                               std::pmr::memory_resource* memory) = 0;
    virtual std::shared_ptr<peer_connection> make_peer_connection(
        std::span<const ice_server> servers, std::pmr::memory_resource* memory) = 0;
    virtual std::shared_ptr<timer> make_timer(std::pmr::memory_resource* memory) = 0;
    virtual std::shared_ptr<sealer> make_sealer(std::array<std::uint8_t, 32> key,
                                                std::pmr::memory_resource* memory) = 0;
    /** Bytes from a cryptographically secure source: they become secret keys and nonces. */
    virtual void random(std::span<std::uint8_t> out) = 0;
    /** Wall-clock seconds since the Unix epoch, for Nostr's `created_at` and `since`. */
    virtual std::int64_t unix_seconds() = 0;
    /** A monotonic clock in milliseconds, for rate limits and timeouts. */
    virtual double now_ms() = 0;
};

}  // namespace webcpp::trystero::backend

#endif
