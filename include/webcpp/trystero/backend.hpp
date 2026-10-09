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
 * Every handler runs after the call that caused it has returned, never inside it: a completion
 * (`done`) of a `peer_connection` or a `sealer`, and a `websocket`'s `close` when no socket can be
 * made, are posted even when their result is known at once. The room calls these operations from
 * inside handlers whose state a completion run there would change under them.
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

/** A data channel frame, in the memory resource of the room it is for. */
using bytes = std::pmr::vector<std::uint8_t>;

/** The error code every completion of a backend reports, empty on success. */
using boost::system::error_code;

// Its implicit move assignment may allocate: a std::pmr member copies when the two resources
// differ, which bugprone-exception-escape, expecting every move assignment not to throw, reports.
/**
 * A STUN or TURN server, an `RTCIceServer` of the `iceServers` Trystero gives `RTCPeerConnection`.
 *
 * A room copies those of its configuration into its own resource.
 *
 * @see "Rooms and actions", in the guide.
 */
struct ice_server {  // NOLINT(bugprone-exception-escape)
    /** The server's URLs, as `"stun:stun.l.google.com:19302"` or `"turn:host:3478"`. */
    std::pmr::vector<std::pmr::string> urls;
    /** The TURN server's user name; empty for a STUN server. */
    std::pmr::string username;
    /** The TURN server's credential; empty for a STUN server. */
    std::pmr::string credential;
};

/**
 * A one-shot timer, what the `setTimeout` calls of Trystero become.
 *
 * Starting it again replaces the pending expiry, and destroying it cancels it.
 *
 * @see "Design", in the guide.
 */
class timer {
public:
    /** Destroys the timer, which cancels a pending expiry. */
    virtual ~timer() = default;

    /**
     * Calls `fire` on the room's executor once `delay` has passed, in place of a pending expiry.
     *
     * @param delay How long to wait.
     * @param fire What to call when the timer expires; never called once it is cancelled.
     */
    virtual void start(std::chrono::milliseconds delay, handler<void()> fire) = 0;

    /** Cancels the pending expiry, if any, whose handler is then never called. */
    virtual void cancel() = 0;
};

/**
 * A WebSocket to a Nostr relay, as the `WebSocket` Trystero's relay manager opens.
 *
 * @see "Design", in the guide.
 */
class websocket {
public:
    /** What a WebSocket reports, each on the room's executor. */
    struct handlers {
        /** The connection opened: messages can be sent. */
        handler<void()> open;
        /** A text message arrived; the view is valid for the duration of the call. */
        handler<void(std::string_view)> message;
        /** The connection closed, or failed to open: called once, also when it never opened. */
        handler<void()> close;
    };

    /** Destroys the WebSocket, which closes it without reporting the close. */
    virtual ~websocket() = default;

    /**
     * Sends a text message; one sent before the connection opens or after it closes is dropped.
     *
     * @param text The message, read during the call.
     */
    virtual void send(std::string_view text) = 0;

    /** Closes the connection, which the handler `close` reports. */
    virtual void close() = 0;
};

/**
 * A WebRTC data channel, as the `RTCDataChannel` a Trystero peer exchanges its frames over.
 *
 * @see "Design", in the guide.
 */
class data_channel {
public:
    /** What a data channel reports, each on the room's executor. */
    struct handlers {
        /** The channel opened. */
        handler<void()> open;
        /** A binary message arrived, handed over in the room's resource. */
        handler<void(bytes)> message;
        /** The channel closed; called once. */
        handler<void()> close;
        /** The buffered amount fell to `low_threshold` or below after being above it. */
        handler<void()> buffered_amount_low;
    };

    /**
     * Trystero's backpressure threshold, its `bufferedAmountLowThreshold`: senders wait while more
     * than 64 KiB is buffered.
     */
    static constexpr std::size_t low_threshold = 0xffff;

    /** Destroys the channel, which closes it without reporting the close. */
    virtual ~data_channel() = default;

    /**
     * Replaces the handlers; a handler may replace them during its own call.
     *
     * @param h The new handlers; an empty one is not called.
     */
    virtual void set_handlers(handlers h) = 0;

    /**
     * Whether the channel is open, its `readyState` "open".
     *
     * @return Whether a message sent now would be sent.
     */
    [[nodiscard]] virtual bool is_open() const = 0;

    /**
     * Sends a binary message; one sent while the channel is not open is dropped.
     *
     * @param frame The message, read during the call.
     */
    virtual void send(std::span<const std::uint8_t> frame) = 0;

    /**
     * The bytes queued to send, its `bufferedAmount`.
     *
     * @return The number of bytes sent and not yet transmitted.
     */
    [[nodiscard]] virtual std::size_t buffered_amount() const = 0;

    /** Closes the channel, which the handler `close` reports. */
    virtual void close() = 0;
};

/** The state of a peer connection, as `RTCPeerConnection` reports it. */
enum class connection_state {
    /** Nothing has happened yet: "new". */
    fresh,
    /** The connection is being established: "connecting", or ICE's "checking". */
    connecting,
    /** The connection works: "connected", or ICE's "completed". */
    connected,
    /** The connection was lost, perhaps for a moment: "disconnected". */
    disconnected,
    /** The connection failed: "failed". */
    failed,
    /** The connection was closed: "closed". */
    closed,
};

/**
 * A WebRTC peer connection, as the `RTCPeerConnection` of a Trystero peer, whose negotiation the
 * signaling drives: it makes an offer or an answer only when asked to.
 *
 * @see "Design", in the guide.
 */
class peer_connection {
public:
    /** What a peer connection reports, each on the room's executor. */
    struct handlers {
        /** A local ICE candidate, the JSON of an `RTCIceCandidateInit`, valid for the call. */
        handler<void(std::string_view)> ice_candidate;
        /** The remote peer opened a data channel. */
        handler<void(std::shared_ptr<data_channel>)> data_channel;
        /** The connection's state changed. */
        handler<void(connection_state)> state;
    };

    /** Destroys the connection, which closes it without reporting the close. */
    virtual ~peer_connection() = default;

    /**
     * Replaces the handlers; a handler may replace them during its own call.
     *
     * @param h The new handlers; an empty one is not called.
     */
    virtual void set_handlers(handlers h) = 0;

    /**
     * Opens a data channel, like `createDataChannel()`.
     *
     * @param label The channel's label, read during the call.
     * @return The channel, made in the connection's resource, or null when the platform refuses
     * to open one, as on a closed connection.
     */
    virtual std::shared_ptr<data_channel> create_data_channel(std::string_view label) = 0;

    /**
     * Creates the offer or the answer the signaling state calls for and applies it, like
     * `setLocalDescription()`.
     *
     * @param done Called with the description's type ("offer" or "answer") and its SDP, views
     * valid for the call, or with `error::backend_failure` and empty views.
     */
    virtual void set_local_description(
        handler<void(error_code, std::string_view type, std::string_view sdp)> done) = 0;

    /**
     * Applies the remote peer's offer or answer, like `setRemoteDescription()`.
     *
     * @param type "offer" or "answer", read during the call.
     * @param sdp The description's SDP, read during the call.
     * @param done Called once it is applied, or with `error::backend_failure` when it is refused.
     */
    virtual void set_remote_description(std::string_view type, std::string_view sdp,
                                        handler<void(error_code)> done) = 0;

    /**
     * Adds a remote ICE candidate, like `addIceCandidate()`.
     *
     * @param candidate_json The JSON of an `RTCIceCandidateInit`, read during the call.
     * @param done Called once it is added, or with `error::backend_failure` when it is refused.
     */
    virtual void add_ice_candidate(std::string_view candidate_json,
                                   handler<void(error_code)> done) = 0;

    /**
     * Whether a remote description was applied, which a candidate needs before it is added.
     *
     * @return Whether the connection has a `remoteDescription`.
     */
    [[nodiscard]] virtual bool has_remote_description() const = 0;

    /** Closes the connection and its channels. */
    virtual void close() = 0;
};

/**
 * AES-GCM under one room key, in Trystero's sealed text form (protocol/signal.hpp), what Trystero's
 * `encrypt` and `decrypt` do with `crypto.subtle`.
 *
 * @see "The protocol", in the guide.
 */
class sealer {
public:
    /** Destroys the sealer. */
    virtual ~sealer() = default;

    /**
     * Seals a text under a fresh random IV.
     *
     * @param plaintext The text, read during the call.
     * @param done Called with the sealed text, a view valid for the call, or with
     * `error::backend_failure` and an empty view.
     */
    virtual void seal(std::string_view plaintext,
                      handler<void(error_code, std::string_view sealed)> done) = 0;

    /**
     * Opens a sealed text.
     *
     * @param sealed The sealed text, read during the call.
     * @param done Called with the plaintext, a view valid for the call, or with an error and an
     * empty view: `trystero::error::wrong_password` when the text was sealed under another key or
     * is not a sealed text.
     */
    virtual void open(std::string_view sealed,
                      handler<void(error_code, std::string_view plaintext)> done) = 0;
};

/**
 * Makes everything above, and gives a room its randomness and its clocks: one per page in the
 * browser, one per application natively, one per test.
 *
 * Each object it makes is allocated, with its handlers and what it hands to the room, in the memory
 * resource it is given, the room's: that resource must outlive the object and every operation the
 * object leaves pending.
 *
 * @see "Allocation", in the guide.
 */
class factory {
public:
    /** Destroys the factory, which must outlive every room that uses it. */
    virtual ~factory() = default;

    /**
     * Opens a WebSocket to a relay.
     *
     * @param url The relay's `ws://` or `wss://` URL, read during the call.
     * @param h What the WebSocket reports.
     * @param memory The room's resource, which the WebSocket, its handlers and its texts are
     * allocated in.
     * @return The WebSocket, which reports a close when it cannot connect.
     */
    virtual std::shared_ptr<websocket> connect(std::string_view url, websocket::handlers h,
                                               std::pmr::memory_resource* memory) = 0;

    /**
     * Makes a peer connection that gathers its candidates through `servers`.
     *
     * @param servers The STUN and TURN servers, read during the call.
     * @param memory The room's resource, which the connection, its channels, its handlers and its
     * texts are allocated in.
     * @return The connection, or null when the platform refuses to make one: the room then
     * reports the negotiation with that peer as failed, through `room::on_error`.
     */
    virtual std::shared_ptr<peer_connection> make_peer_connection(
        std::span<const ice_server> servers, std::pmr::memory_resource* memory) = 0;

    /**
     * Makes a timer.
     *
     * @param memory The room's resource, which the timer and its waits are allocated in.
     * @return The timer, not started.
     */
    virtual std::shared_ptr<timer> make_timer(std::pmr::memory_resource* memory) = 0;

    /**
     * Makes a sealer under a room's key.
     *
     * @param key The raw AES-256 key, as `protocol::seal_key` derives it.
     * @param memory The room's resource, which the sealer, its texts and its completions are
     * allocated in.
     * @return The sealer.
     */
    virtual std::shared_ptr<sealer> make_sealer(std::array<std::uint8_t, 32> key,
                                                std::pmr::memory_resource* memory) = 0;

    /**
     * Fills `out` with bytes from a cryptographically secure source: they become secret keys and
     * nonces.
     *
     * @param out The bytes to fill.
     */
    virtual void random(std::span<std::uint8_t> out) = 0;

    /**
     * Wall-clock seconds since the Unix epoch, for Nostr's `created_at` and `since`.
     *
     * @return The current time, in whole seconds.
     */
    virtual std::int64_t unix_seconds() = 0;

    /**
     * A monotonic clock in milliseconds, for rate limits and timeouts, as `performance.now()`.
     *
     * @return The current time, in milliseconds from an origin of the backend's.
     */
    virtual double now_ms() = 0;
};

}  // namespace webcpp::trystero::backend

#endif
