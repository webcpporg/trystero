// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_ROOM_ERROR_HPP
#define WEBCPP_TRYSTERO_ROOM_ERROR_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/error.hpp>

#include <boost/system/error_code.hpp>

#include <optional>
#include <string_view>

namespace webcpp::trystero {

/**
 * The kind of signal that did not open, the `stage` of Trystero's `decrypt-failed` room error.
 */
enum class signal_stage {
    /** An offer. */
    offer,
    /** An answer. */
    answer,
};

/**
 * A problem a room or an app handled itself, what `room::on_error` and `app::on_error` report,
 * Trystero's `RoomError` without its `cause`.
 *
 * Every report carries a code, its message, and the app's and the room's ids; the other members
 * are set only for the codes Trystero sets them for, and are empty otherwise.
 *
 * | Code                  | Members set                                              |
 * | `decrypt_failed`      | `peer_id`, `stage`                                       |
 * | `peer_connect_failed` | `peer_id`, `turn_configured`                             |
 * | `room_full`           | `peer_id`                                                |
 * | `banned`              | `peer_id` and `reason`, when the peer gave them          |
 * | `plugin_failed`       | `plugin`, when the plugin is known                       |
 * | `handshake_failed`    | `peer_id`                                                |
 * | `peer_error`          | `peer_id`                                                |
 *
 * The views are valid for the duration of the listener's call.
 */
struct room_error {
    /** What happened, an @ref error; @ref room_error_code_name gives Trystero's name of it. */
    boost::system::error_code code;
    /** A description, Trystero's `message`: for `decrypt_failed` it names the stage, as "failed
     * to decrypt offer (incorrect password or corrupted signal)". */
    std::string_view message;
    /** The app's id. */
    std::string_view app_id;
    /** The room's id; empty for a report of the app's configuration. */
    std::string_view room_id;
    /** The id of the peer it concerns; empty when it concerns none. */
    std::string_view peer_id;
    /** For `decrypt_failed`, the kind of the signal that did not open. */
    std::optional<signal_stage> stage;
    /** For `peer_connect_failed`, whether a TURN server was configured: an ICE server with a
     * `turn:` or `turns:` URL. */
    bool turn_configured = false;
    /** For `banned`, the reason the peer gave, when it gave one. */
    std::string_view reason;
    /** For `plugin_failed`, the plugin that failed, when it is known. */
    std::string_view plugin;
};

/**
 * Trystero's name of a room error's code, its `RoomErrorCode`.
 *
 * @param code A code of @ref error_category.
 * @return "room-full", "banned", "relay-failed", "identity-failed", "plugin-failed",
 * "decrypt-failed", "peer-connect-failed", "handshake-failed" or "peer-error" for the @ref error
 * of that name, or an empty view for any other code: the others are no room error of Trystero's.
 */
inline std::string_view room_error_code_name(boost::system::error_code code) noexcept {
    if (code.category() != error_category()) {
        return {};
    }
    switch (static_cast<error>(code.value())) {
        case error::room_full: return "room-full";
        case error::banned: return "banned";
        case error::relay_failed: return "relay-failed";
        case error::identity_failed: return "identity-failed";
        case error::plugin_failed: return "plugin-failed";
        case error::decrypt_failed: return "decrypt-failed";
        case error::peer_connect_failed: return "peer-connect-failed";
        case error::handshake_failed: return "handshake-failed";
        case error::peer_error: return "peer-error";
        default: return {};
    }
}

}  // namespace webcpp::trystero

#endif
