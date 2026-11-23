// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The names of Trystero 0.27.0's identity handshake that the handshake manager and the steps share:
 the member that names a step and the reasons a handshake fails. Light on purpose, it pulls in no
 parser and no cryptography.
*/
#ifndef WEBCPP_TRYSTERO_PROTOCOL_HANDSHAKE_REASON_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_HANDSHAKE_REASON_HPP

#include <webcpp/trystero/config.hpp>

#include <string_view>

namespace webcpp::trystero::protocol {

/** The member that names a step of the handshake, 0.27.0's `hsStepKey`: `__${libId}_id`. */
inline constexpr std::string_view handshake_step_key = "__trystero_id";

/** The reasons a handshake fails, 0.27.0's messages without their `Trystero: ` prefix. */
namespace handshake_reason {
/** A step of the wrong shape, a key that does not derive the id claimed, or a signature that does
 * not verify, 0.27.0's `peerIdentityErr`. */
inline constexpr std::string_view identity = "peer identity verification failed";
/** The fingerprints claimed are not those of the description this side applied. */
inline constexpr std::string_view dtls_mismatch = "DTLS fingerprint mismatch";
/** A description of the connection has no fingerprint. */
inline constexpr std::string_view dtls_missing = "DTLS fingerprint missing";
/** The peer sent more than 16 messages the handshake had not read. */
inline constexpr std::string_view too_many_messages = "too many pending handshake messages";
/** The handshake did not complete in 10 seconds. */
inline constexpr std::string_view timed_out = "handshake timed out after 10000ms";
/** The peer left while its handshake ran, 0.27.0's `handshakeDisconnectErr`. */
inline constexpr std::string_view disconnected = "peer disconnected during handshake";
}  // namespace handshake_reason

}  // namespace webcpp::trystero::protocol

#endif
