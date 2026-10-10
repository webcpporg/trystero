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
 * Each value is one of the errors Trystero rejects a promise with or reports to `onPeerError`,
 * as a code of @ref error_category rather than a message: an operation completes with it, and
 * `room::on_error` reports it with the peer it concerns.
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
     * A peer's signal or handshake used another password, or the peer sent a handshake message the
     * protocol does not expect, Trystero's "incorrect password for overlapping room".
     */
    wrong_password,
    /** A connected peer did not finish the handshake in time, Trystero's "handshake timed out". */
    handshake_timed_out,
    /**
     * Reserved: nothing reports it today.
     *
     * A handshake message of the wrong shape reports `wrong_password`, as Trystero's handshake
     * does.
     */
    handshake_failed,
    /** The browser or the native stack refused an operation. */
    backend_failure,
    /**
     * The room's configuration names something no backend can use, as an ICE server whose URL
     * WebRTC refuses; the room goes on without it, and `room::on_error` reports it with no peer.
     */
    invalid_configuration,
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
            case error::handshake_failed: return "the handshake failed";
            case error::backend_failure: return "the WebRTC backend refused an operation";
            case error::invalid_configuration: return "the room's configuration is unusable";
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
