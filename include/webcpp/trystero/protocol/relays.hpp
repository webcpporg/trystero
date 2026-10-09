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
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace webcpp::trystero::protocol {

/** Trystero 0.26's default Nostr relays, in its order: the order is part of the relay derivation. */
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
 * Trystero's `shuffle`: Fisher-Yates driven by `sin(seed++) * 10000`, so every peer of an app picks the same order.
 * Tip: `std::sin` and V8's `Math.sin` agree on these inputs; the oracle's fixtures check it.
 */
template <class T>
std::vector<T> shuffle(std::vector<T> items, double seed) {
    auto rand = [&seed] {
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
 * The relays an app uses when it names none: the defaults shuffled by the app id, the first `redundancy` kept. The
 * views are into `defaults`.
 */
inline std::vector<std::string_view> derive_relays(std::string_view app_id, std::span<const std::string_view> defaults, std::size_t redundancy) {
    auto shuffled = shuffle(std::vector<std::string_view>(defaults.begin(), defaults.end()), str_to_num(app_id));
    if (shuffled.size() > redundancy) shuffled.resize(redundancy);
    return shuffled;
}

}  // namespace webcpp::trystero::protocol

#endif
