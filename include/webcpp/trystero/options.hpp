// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_OPTIONS_HPP
#define WEBCPP_TRYSTERO_OPTIONS_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/log.hpp>

#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero {

/**
 * What an app is made with: the part of Trystero's `joinRoom` configuration that every room of
 * the app shares, its peer id among it, and the one connection per remote peer its rooms share.
 *
 * The app copies every part of it into `memory`. It is move-only, as its `log` is: pass it moved,
 * or build it in place with designated initializers.
 */
struct app_config {
    /** The application's id, Trystero's `appId`: rooms of different apps never meet. */
    std::pmr::string app_id = {};
    /**
     * This program's id in every room of the app, Trystero's `selfId`; empty means 20 random
     * characters, drawn once for the app.
     *
     * Other peers ignore an id longer than `protocol::max_peer_id_bytes` (64): the app replaces
     * one with 20 random characters, and reports it with `error::invalid_configuration`.
     */
    std::pmr::string self_id = {};
    /**
     * The Nostr relays to use, Trystero's `relayConfig.urls`; empty means Trystero's defaults,
     * derived from the app id.
     */
    std::pmr::vector<std::pmr::string> relay_urls = {};
    /**
     * How many of the default relays the derivation keeps, Trystero's `relayConfig.redundancy`;
     * ignored when `relay_urls` names the relays.
     */
    std::size_t relay_redundancy = 5;
    /**
     * The STUN and TURN servers; empty means Trystero's defaults, @ref default_ice_servers.
     *
     * A server WebRTC would refuse (a URL that is not `stun:`, `stuns:`, `turn:` or `turns:`
     * `host[:port]`, or a TURN server without a user name and a credential) is dropped when the
     * app is made, and reported with `error::invalid_configuration`; the app goes on with the
     * others, on every backend.
     */
    std::pmr::vector<backend::ice_server> ice_servers = {};
    /**
     * How long a connection with no room bound to it after a departure stays open, Trystero's
     * `sharedPeerIdleMs`; 0 closes it at once.
     *
     * A negative value is replaced with 0, and reported with `error::invalid_configuration`.
     */
    std::chrono::milliseconds idle_timeout{123'333};
    /**
     * Where the app's and its rooms' diagnostics go: called with a category ("app", "nostr",
     * "signal", "webrtc", "room" or "action") and a sentence; empty means none, and then no text
     * is composed.
     *
     * The app moves the sink into `memory`, and composes each sentence there.
     */
    detail::logger::sink log = {};
    /**
     * The resource every allocation of the app goes through: its state, its connections, the peer
     * connections and data channels its rooms' negotiations make, the frames received on them, the
     * sentences of its log; not null.
     *
     * What libdatachannel, OpenSSL and the browser's JavaScript allocate stays theirs. The app uses
     * the resource from its executor only, so it need not be thread-safe when that executor runs
     * on one thread. It must outlive the app, its rooms and every operation they leave pending: on
     * Asio, until the io_context their executor belongs to has run out of their work or has been
     * destroyed, so a resource is declared before its io_context; in the browser, until the app's
     * and its rooms' pending promises have settled. The app and its rooms allocate and free per
     * event, so a resource that reuses what is freed (a pool) suits a long-lived app, and a
     * monotonic one only a short-lived one.
     *
     * @see "Allocation", in the guide.
     */
    std::pmr::memory_resource* memory = std::pmr::get_default_resource();
};

/**
 * What a room of an app is joined with: the part of Trystero's `joinRoom` configuration that is
 * the room's own.
 */
struct room_options {
    /**
     * The room's password, Trystero's `password`: part of the key the signals are sealed with,
     * and a challenge added to the handshake, so peers with another password never join.
     *
     * Empty means none: the signals are still sealed, under a key of the app and the room alone,
     * as Trystero's `genKey` does, and the handshake has no challenge.
     */
    std::pmr::string password = {};
    /**
     * How long a connected peer has to finish the handshake before it is dropped, Trystero's
     * `handshakeTimeoutMs`.
     */
    std::chrono::milliseconds handshake_timeout{10'000};
    /**
     * The most bytes the room reserves at once for the transfers it receives, across peers,
     * Trystero's `maxReceiveBytes`.
     */
    std::size_t max_receive_bytes = std::size_t{256} * 1024 * 1024;
};

/**
 * Trystero's default STUN servers, its `defaultIceServers`: Google's and Cloudflare's.
 *
 * @param memory The resource the list and its strings are allocated in.
 * @return The servers, an app's when its configuration names none.
 */
inline std::pmr::vector<backend::ice_server> default_ice_servers(
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::vector<backend::ice_server> out(memory);
    for (const std::string_view url : {
             "stun:stun.l.google.com:19302",
             "stun:stun1.l.google.com:19302",
             "stun:stun2.l.google.com:19302",
             "stun:stun.cloudflare.com:3478",
         }) {
        backend::ice_server server{
            .urls = std::pmr::vector<std::pmr::string>(memory),
            .username = std::pmr::string(memory),
            .credential = std::pmr::string(memory),
        };
        server.urls.emplace_back(url);
        out.push_back(std::move(server));
    }
    return out;
}

}  // namespace webcpp::trystero

#endif
