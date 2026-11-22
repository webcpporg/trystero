// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_ERROR_HPP
#define WEBCPP_TRYSTERO_ERROR_HPP

#include <webcpp/trystero/config.hpp>

#include <boost/system/error_code.hpp>

#include <cstddef>
#include <string>
#include <type_traits>

namespace webcpp::trystero {

/**
 * What an operation of the library completes with when it does not succeed.
 *
 * Each value is one of the errors Trystero rejects a promise with or reports to a room's
 * `onError`, as a code of @ref error_category rather than a message: an operation completes with
 * it, and `room::on_error` reports it in a @ref room_error, with the context Trystero gives it and
 * `room_error_code_name` gives Trystero's name of a room error's code.
 *
 * @see "Rooms and actions", in the guide.
 */
enum class error {
    /** The room was left while the operation was pending, Trystero's "room left". */
    room_left = 1,
    /** The target is not an active peer of the room, Trystero's "no active peer with id". */
    no_such_peer,
    /** The peer went away before the operation finished, Trystero's "peer disconnected". */
    peer_disconnected,
    /** The action's name is not 1 to 32 UTF-8 bytes without NUL, as Trystero requires. */
    invalid_action_name,
    /** The action's metadata does not fit one frame, Trystero's "action metadata is too large". */
    metadata_too_large,
    /** The receiver refused a transfer, or a received payload is not of the awaited type. */
    transfer_rejected,
    /** The receiver neither accepted nor refused a transfer in time. */
    transfer_timed_out,
    /** The data channel stopped draining while a transfer waited for it. */
    channel_stalled,
    /**
     * A peer sharing a connection answered the room's password handshake under another password,
     * or sent a handshake message the protocol does not expect, Trystero's "incorrect password for
     * overlapping room".
     *
     * A signal under another password is `decrypt_failed`.
     */
    wrong_password,
    /** A connected peer did not finish the handshake in time, Trystero's "handshake timed out". */
    handshake_timed_out,
    /**
     * A peer's handshake failed, 0.27.0's `handshake-failed` room error, which
     * `room_error_code_name` names.
     *
     * Nothing reports it before Phase 2's handshake: the password
     * handshake of rooms sharing a connection reports its failures as `wrong_password` and
     * `handshake_timed_out`, which name what failed.
     */
    handshake_failed,
    /** The browser or the native stack refused an operation. */
    backend_failure,
    /**
     * The app's configuration holds something it cannot use, as an ICE server whose URL WebRTC
     * refuses; the app goes on without it, and `app::on_error` reports it with no peer.
     */
    invalid_configuration,
    /** The app already holds a room of that id; a room is joined once per app. */
    already_joined,
    /** The app already holds as many rooms as a peer accepts, 64 of them. */
    too_many_rooms,
    /** The room holds as many peers as it accepts, 0.27.0's "room full". */
    room_full,
    /** The peer is banned from the room, 0.27.0's "peer banned". */
    banned,
    /** Every signaling relay failed, 0.27.0's `relay-failed` room error. */
    relay_failed,
    /** A peer's or the endpoint's identity could not be resolved, 0.27.0's `identity-failed`. */
    identity_failed,
    /** A plugin of the room failed to set up, 0.27.0's `plugin-failed`. */
    plugin_failed,
    /**
     * A signal could not be opened: another room password, a corrupted text, or a plaintext that
     * is not UTF-8, 0.27.0's `decrypt-failed`.
     *
     * A room reports it for an offer or an answer, at most
     * once a minute per peer and stage, with the stage in the report and its message, as
     * "failed to decrypt offer (incorrect password or corrupted signal)".
     */
    decrypt_failed,
    /** The peer could not be connected after the SDP exchange, 0.27.0's `peer-connect-failed`. */
    peer_connect_failed,
    /** A peer reported an error, 0.27.0's `peer-error`. */
    peer_error,
    /** A listener that takes one at a time is already set, 0.27.0's "already registered". */
    already_subscribed,
    /** The target of a send is not connected, 0.27.0's "no active peer with id". */
    not_connected,
    /** An action was aborted by its caller, 0.27.0's "operation aborted". */
    action_aborted,
    /** The receiver of a request answered with a failure, 0.27.0's `rejected` action error. */
    request_rejected,
    /** A request was not answered in time, 0.27.0's "request timed out". */
    request_timed_out,
    /**
     * The action's name was made the other kind of action first, a message action or a request
     * action, 0.27.0's "action type cannot be redefined", which it throws.
     */
    action_redefined,
};

/**
 * The category of @ref error, named "webcpp.trystero", whose messages are fixed texts.
 *
 * A program reaches its one instance through @ref error_category.
 */
class error_category_impl final : public boost::system::error_category {
public:
    /**
     * The category's name.
     *
     * @return "webcpp.trystero".
     */
    const char* name() const noexcept override { return "webcpp.trystero"; }

    /**
     * The message of a value of @ref error, as a `std::string`.
     *
     * @param ev The value, an @ref error as an `int`.
     * @return The message, or "unknown webcpp.trystero error" for a value @ref error does not name.
     */
    std::string message(int ev) const override { return text(ev); }

    /**
     * The message of a value of @ref error without a `std::string`.
     *
     * A room logs through this form, which allocates nothing: the messages are static texts, and
     * the buffer is not used.
     *
     * @param ev The value, an @ref error as an `int`.
     * @return The message, a static text.
     */
    const char* message(int ev, char* /*buffer*/, std::size_t /*length*/) const noexcept override {
        return text(ev);
    }

private:
    static const char* text(int ev) noexcept {
        switch (static_cast<error>(ev)) {
            case error::room_left: return "room left";
            case error::no_such_peer: return "no active peer with this id";
            case error::peer_disconnected: return "peer disconnected";
            case error::invalid_action_name:
                return "action names must contain 1 to 32 UTF-8 bytes and no NUL";
            case error::metadata_too_large: return "action metadata is too large";
            case error::transfer_rejected: return "the receiver rejected the transfer";
            case error::transfer_timed_out: return "the action transfer timed out";
            case error::channel_stalled: return "the data channel stopped draining";
            case error::wrong_password: return "the peer uses another room password";
            case error::handshake_timed_out: return "the handshake timed out";
            case error::handshake_failed: return "handshake failed";
            case error::backend_failure: return "the WebRTC backend refused an operation";
            case error::invalid_configuration: return "the configuration is unusable";
            case error::already_joined: return "the app has already joined a room of that id";
            case error::too_many_rooms: return "the app holds as many rooms as a peer accepts, 64";
            case error::room_full: return "room full";
            case error::banned: return "peer banned";
            case error::relay_failed: return "all signaling relays failed";
            case error::identity_failed: return "identity resolution failed";
            case error::plugin_failed: return "plugin setup failed";
            case error::decrypt_failed:
                return "failed to decrypt offer or answer (incorrect password or corrupted signal)";
            case error::peer_connect_failed:
                return "could not connect to peer after exchanging SDP";
            case error::peer_error: return "peer error";
            case error::already_subscribed: return "handler already registered; unsubscribe first";
            case error::not_connected: return "no active peer with this id";
            case error::action_aborted: return "operation aborted";
            case error::request_rejected: return "request rejected";
            case error::request_timed_out: return "request timed out";
            case error::action_redefined: return "action type cannot be redefined";
        }
        return "unknown webcpp.trystero error";
    }
};

/**
 * The category of every @ref error code.
 *
 * @return The one instance of @ref error_category_impl.
 */
const inline boost::system::error_category& error_category() noexcept {
    static const error_category_impl category;
    return category;
}

/**
 * An error code of @ref error_category, which `boost::system::error_code` finds by
 * argument-dependent lookup.
 *
 * @param e The error.
 * @return The code of `e`.
 */
inline boost::system::error_code make_error_code(error e) noexcept {
    return {static_cast<int>(e), error_category()};
}

}  // namespace webcpp::trystero

/** Makes @ref webcpp::trystero::error convertible to `boost::system::error_code`. */
template <>
struct boost::system::is_error_code_enum<webcpp::trystero::error> : std::true_type {};

#endif
