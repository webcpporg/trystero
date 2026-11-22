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

#include <webcpp/document/parse.hpp>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero {

/**
 * What an app is made with: the part of Trystero's `joinRoom` configuration that every room of
 * the app shares.
 *
 * It names no peer id: 0.27.0 has no custom `peerId`, and the app draws its own (`app::self_id`).
 *
 * The app copies every part of it into `memory`. It is move-only, as its `log` is: pass it moved,
 * or build it in place with designated initializers.
 */
struct app_config {
    /** The application's id, Trystero's `appId`: rooms of different apps never meet. */
    std::pmr::string app_id = {};
    /**
     * The password shared by every room of this app, Trystero's `password`.
     *
     * The app copies it into its resource and keeps it unchanged. Different passwords require
     * separate apps with distinct peer ids. Empty means a passwordless domain. Signals are still
     * sealed with the app and room ids, and the room handshake then has no password challenge.
     */
    std::pmr::string password = {};
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
     * Where the app's and its rooms' diagnostics go: called with a category ("app", "nostr",
     * "signal", "webrtc", "room" or "action") and a sentence; empty means none, and then no text
     * is composed.
     *
     * The app moves the sink into `memory`, and composes each sentence there.
     */
    detail::logger::sink log = {};
    /**
     * The resource every allocation of the app goes through: its state, its rooms' keys and the
     * sentences of its log, and each room's whose `room_options::memory` is null; not null.
     *
     * What libdatachannel, OpenSSL and the browser's JavaScript allocate stays theirs. The app uses
     * the resource from its executor only, so it need not be thread-safe when that executor runs
     * on one thread. It must outlive the app, its rooms, retained values allocated in it and every
     * operation they leave pending: on Asio, until the io_context their executor belongs to has run
     * out of their work or has been destroyed, so a resource is declared before its io_context; in
     * the browser, until the app's and its rooms' pending promises have settled. The app and its
     * rooms allocate and free per event, so a resource that reuses what is freed (a pool) suits a
     * long-lived app, and a monotonic one only a short-lived one.
     *
     * @see "Allocation", in the guide.
     */
    std::pmr::memory_resource* memory = std::pmr::get_default_resource();
};

/**
 * What a room of an app is joined with: the part of Trystero's `joinRoom` configuration that is
 * the room's own.
 *
 * It has no handshake timeout: 0.27.0 removed `handshakeTimeoutMs`, and a peer has 10 s, its
 * default, to finish the handshake.
 */
struct room_options {
    /**
     * The most bytes the room reserves at once for the transfers it receives, across peers,
     * Trystero's `maxReceiveBytes`.
     */
    std::size_t max_receive_bytes = std::size_t{256} * 1024 * 1024;
    /**
     * The largest image a received JSON payload's document may take in the room, webcpp::document's
     * own default, 64 MiB, which Trystero JS, reading with `JSON.parse`, does not have.
     *
     * A payload's document grows to at most 96 times its text and 64 KiB, which every valid text
     * fits, and this bounds it whatever the text: each value is a node of 192 bytes, so a payload
     * of one-character values past some 350,000 of them is not delivered with the default.
     *
     * @see "JSON texts", in the guide.
     */
    std::uint64_t max_json_document_bytes = webcpp::document::parse_options().max_document_bytes;
    /**
     * The resource every allocation of the room goes through: its state, negotiations, peer
     * connections and data channels, the frames sent and received on them, relays, actions,
     * transfers and JSON.
     *
     * Null means the app's resource.
     *
     * It must outlive the room, every value it hands out and every operation it leaves pending: on
     * Asio, until its io_context has run out of the room's work or has been destroyed; in the
     * browser, until its pending promises have settled.
     */
    std::pmr::memory_resource* memory = nullptr;
};

/**
 * How the relays of an app's rooms connect, Trystero's `RelayConnectivity`, which
 * `app::set_relay_connectivity` sets for one app.
 */
enum class relay_connectivity {
    /** Connected: a relay closed is reconnected, Trystero's `'online'`. */
    online,
    /** Held closed: every relay socket is closed, and none is opened or reconnected until the app
     * is online again, Trystero's `'offline'`. */
    offline,
    /** Following the page, Trystero's `'auto'`, the default: offline from the page's `offline`
     * event until its `online` event; natively, where no such event exists, online.
     *
     * Tip: unlike 0.27.0, which also reconnects the relays when the page becomes visible again
     * (its `visibilitychange` listener), the port follows only the `online` and `offline` events.
     */
    automatic,
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
