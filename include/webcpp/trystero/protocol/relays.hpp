// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PROTOCOL_RELAYS_HPP
#define WEBCPP_TRYSTERO_PROTOCOL_RELAYS_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/protocol/topics.hpp>

#include <array>
#include <cmath>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace webcpp::trystero::protocol {

/**
 * The longest text a room reads from a relay, in UTF-8 bytes: 1 MiB, far beyond any message of the
 * protocol (an event's content is a signal, whose SDP is a few KiB) and of what public relays
 * accept (their events are capped at 64 to 512 KiB).
 *
 * Both backends close a relay's connection that sends a longer one, before copying it, and the
 * room reconnects as it does to a relay that went away.
 */
inline constexpr std::size_t max_relay_message_bytes = std::size_t{1} << 20U;

/**
 * The most text a room holds unsent for one relay, in UTF-8 bytes: 4 MiB, a thousand signals and
 * more.
 *
 * A relay that stops reading lets the texts pile up; beyond this, both backends drop its
 * connection, which reports a close, and the room reconnects with its backoff.
 */
inline constexpr std::size_t max_queued_relay_bytes = std::size_t{4} << 20U;

/**
 * Trystero 0.26's default Nostr relays, its `defaultRelayUrls`, in its order: the order is part of
 * the relay derivation.
 */
inline constexpr std::array<std::string_view, 29> default_nostr_relays{
    "wss://0x-nostr-relay.fly.dev",
    "wss://bendernostur.duckdns.org:8443",
    "wss://cdn.satellite.earth",
    "wss://nos.lol",
    "wss://nostr.chaima.info",
    "wss://nostr.christiansass.de",
    "wss://nostr.rblb.it",
    "wss://nostr.red5d.dev",
    "wss://nostr.robosats.org",
    "wss://nostr.stakey.net",
    "wss://nostrcity-club.fly.dev",
    "wss://relay-dev.gulugulu.moe",
    "wss://relay.aarpia.com",
    "wss://relay.agentry.com",
    "wss://relay.bitmacro.cloud",
    "wss://relay.bullishbounty.com",
    "wss://relay.degmods.com",
    "wss://relay.flashapp.me",
    "wss://relay.grigic.org",
    "wss://relay.hackshed.dev",
    "wss://relay.kaleidoswap.com",
    "wss://relay.layer.systems",
    "wss://relay.nostr.blockhenge.com",
    "wss://relay.nostr.dev.br",
    "wss://relay.nostrmap.net",
    "wss://relay.novospes.com",
    "wss://relay.piazza.today",
    "wss://relay.routstr.com",
    "wss://testr.nymble.world",
};

/**
 * Trystero's `shuffle`: Fisher-Yates driven by `sin(seed++) * 10000`, so every peer of an app picks
 * the same order.
 *
 * Tip: `std::sin` and V8's `Math.sin` agree on these inputs; the oracle's fixtures check it.
 *
 * @tparam Sequence A sequence with `size()` and random access, as `std::pmr::vector`.
 * @param items The items, shuffled in the copy returned.
 * @param seed The seed, @ref str_to_num of the app id for the relays.
 * @return The items, shuffled.
 */
template <class Sequence>
Sequence shuffle(Sequence items, double seed) {
    const auto rand = [&seed] {
        const double x = std::sin(seed++) * 10000;
        return x - std::floor(x);
    };
    for (std::size_t i = items.size(); i != 0;) {
        const auto j = static_cast<std::size_t>(std::floor(rand() * static_cast<double>(i)));
        --i;
        std::swap(items[i], items[j]);
    }
    return items;
}

/**
 * The relays an app uses when it names none, Trystero's `getRelays`: the defaults shuffled by the
 * app id, the first `redundancy` kept.
 *
 * @param app_id The application's id, whose @ref str_to_num seeds the shuffle.
 * @param defaults The relays to choose from, @ref default_nostr_relays for a room.
 * @param redundancy How many to keep.
 * @param memory The resource the list is allocated in.
 * @return The relays, as views into `defaults`.
 */
inline std::pmr::vector<std::string_view> derive_relays(
    std::string_view app_id, std::span<const std::string_view> defaults, std::size_t redundancy,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    auto shuffled =
        shuffle(std::pmr::vector<std::string_view>(defaults.begin(), defaults.end(), memory),
                str_to_num(app_id));
    if (shuffled.size() > redundancy) {
        shuffled.resize(redundancy);
    }
    return shuffled;
}

}  // namespace webcpp::trystero::protocol

#endif
