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
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::protocol {

/**
 * The longest text a room reads from a relay, in UTF-8 bytes: 1 MiB, far beyond any message of the
 * protocol (an event's content is a signal, whose SDP is a few KiB) and of what public relays
 * accept (their events are capped at 64 to 512 KiB).
 *
 * The platform backend closes, on both targets, a relay's connection that sends a longer one,
 * before copying it, in a page before it enters WebAssembly, and the room reconnects as it does to
 * a relay that went away.
 */
inline constexpr std::size_t max_relay_message_bytes = std::size_t{1} << 20U;

/**
 * The most text a room holds unsent for one relay, in UTF-8 bytes: 4 MiB, a thousand signals and
 * more.
 *
 * A relay that stops reading lets the texts pile up; beyond this, the platform backend drops its
 * connection on both targets, which reports a close, and the room reconnects with its backoff.
 */
inline constexpr std::size_t max_queued_relay_bytes = std::size_t{4} << 20U;

/**
 * Trystero 0.27.0's default Nostr relays, its `defaultRelayUrls`, in its order: the order is part
 * of the relay derivation.
 */
inline constexpr std::array<std::string_view, 30> default_nostr_relays{
    "wss://bucket.coracle.social",
    "wss://fanfares.nostr1.com",
    "wss://no.str.cr",
    "wss://nostr-01.yakihonne.com",
    "wss://nostr-pub.wellorder.net",
    "wss://nostr.azzamo.net",
    "wss://nostr.chaima.info",
    "wss://nostr.data.haus",
    "wss://nostr.hifish.org",
    "wss://nostr.liberty.fans",
    "wss://nostr.overmind.lol",
    "wss://nostr.sathoarder.com",
    "wss://nostrelay.circum.space",
    "wss://relay.angor.io",
    "wss://relay.ditto.pub",
    "wss://relay.dwadziesciajeden.pl",
    "wss://relay.fundstr.me",
    "wss://relay.guggero.org",
    "wss://relay.masize.com",
    "wss://relay.minibolt.info",
    "wss://relay.mostro.network",
    "wss://relay.nostr.com",
    "wss://relay.nostr.net",
    "wss://relay.nostrhub.fr",
    "wss://relay.notoshi.win",
    "wss://relay.primal.net",
    "wss://relay.snort.social",
    "wss://relay.wavlake.com",
    "wss://soloco.nl",
    "wss://yabu.me",
};

/**
 * Trystero 0.27.0's `shuffle`: a Fisher-Yates shuffle driven by a 32-bit generator (mulberry32)
 * seeded with `seed`, so every peer of an app picks the same order.
 *
 * Tip: the generator is integer-only, so every target and every standard library shuffle alike.
 *
 * @tparam T The element type.
 * @param xs The elements, shuffled in place.
 * @param seed The seed; 0 stands for 1, as in Trystero.
 */
template <class T>
void seeded_shuffle(std::span<T> xs, std::uint32_t seed) {
    std::uint32_t state = seed != 0 ? seed : 1U;
    const auto rand = [&state] {
        state += 0x6d2b79f5U;
        std::uint32_t t = (state ^ (state >> 15U)) * (1U | state);
        t = (t + (t ^ (t >> 7U)) * (61U | t)) ^ t;
        return static_cast<double>(t ^ (t >> 14U)) / 4294967296.0;
    };
    for (std::size_t i = xs.size(); i != 0;) {
        const auto j = static_cast<std::size_t>(std::floor(rand() * static_cast<double>(i)));
        --i;
        std::swap(xs[i], xs[j]);
    }
}

/**
 * The relays an app uses when it names none, Trystero's `getRelays`: the defaults shuffled by
 * @ref str_to_num of the app id, the first `redundancy` kept.
 *
 * @param app_id The application's id.
 * @param defaults The relays to choose from, @ref default_nostr_relays for a room.
 * @param redundancy How many to keep.
 * @param memory The resource the list is allocated in.
 * @return The relays, as views into `defaults`.
 */
inline std::pmr::vector<std::string_view> derive_relays(
    std::string_view app_id, std::span<const std::string_view> defaults, std::size_t redundancy,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    std::pmr::vector<std::string_view> shuffled(defaults.begin(), defaults.end(), memory);
    seeded_shuffle(std::span(shuffled), static_cast<std::uint32_t>(str_to_num(app_id)));
    if (shuffled.size() > redundancy) {
        shuffled.resize(redundancy);
    }
    return shuffled;
}

}  // namespace webcpp::trystero::protocol

#endif
