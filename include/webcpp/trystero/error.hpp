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

/** What an operation of the library completes with when it does not succeed. */
enum class error {
    room_left = 1,          // the room was left while the operation was pending
    no_such_peer,           // the target is not an active peer of the room
    peer_disconnected,      // the peer went away before the operation finished
    invalid_action_name,    // an action name must be 1 to 32 UTF-8 bytes without NUL
    metadata_too_large,     // metadata must fit one frame
    transfer_rejected,      // the receiver refused a transfer; the reason is logged
    transfer_timed_out,     // the receiver neither accepted nor refused in time
    channel_stalled,        // the data channel stopped draining
    wrong_password,         // a peer's signal or handshake used another password
    handshake_timed_out,    // a connected peer did not finish the handshake in time
    handshake_failed,       // the handshake's messages were not what the protocol expects
    backend_failure,        // the browser or the native stack refused an operation
};

class error_category_impl : public boost::system::error_category {
public:
    const char* name() const noexcept override { return "webcpp.trystero"; }

    std::string message(int ev) const override { return text(ev); }

    /** The message without a `std::string`: a room logs through this form, which allocates nothing. */
    const char* message(int ev, char* /*buffer*/, std::size_t /*length*/) const noexcept override { return text(ev); }

private:
    static const char* text(int ev) noexcept {
        switch (static_cast<error>(ev)) {
            case error::room_left: return "room left";
            case error::no_such_peer: return "no active peer with this id";
            case error::peer_disconnected: return "peer disconnected";
            case error::invalid_action_name: return "action names must contain 1 to 32 UTF-8 bytes and no NUL";
            case error::metadata_too_large: return "action metadata is too large";
            case error::transfer_rejected: return "the receiver rejected the transfer";
            case error::transfer_timed_out: return "the action transfer timed out";
            case error::channel_stalled: return "the data channel stopped draining";
            case error::wrong_password: return "the peer uses another room password";
            case error::handshake_timed_out: return "the handshake timed out";
            case error::handshake_failed: return "the handshake failed";
            case error::backend_failure: return "the WebRTC backend refused an operation";
        }
        return "unknown webcpp.trystero error";
    }
};

inline const boost::system::error_category& error_category() noexcept {
    static const error_category_impl category;
    return category;
}

inline boost::system::error_code make_error_code(error e) noexcept { return {static_cast<int>(e), error_category()}; }

}  // namespace webcpp::trystero

template <>
struct boost::system::is_error_code_enum<webcpp::trystero::error> : std::true_type {};

#endif
