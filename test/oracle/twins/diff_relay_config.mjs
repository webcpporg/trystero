// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

import {getRelaySockets, joinRoom} from 'trystero'
import {RTCPeerConnection} from 'werift'
import {openPage} from '../page.mjs'

const relay = process.env.TRYSTERO_RELAY_URL
const config = {appId: 'webcpp-trystero-differences', rtcPolyfill: RTCPeerConnection}
// Two configurations. The second relay is the same local relay under a path, which it accepts as
// it accepts any, and its STUN server one where nothing answers; the first has no ICE server, so
// that the peers meet on this machine alone.
const first = {relayConfig: {urls: [relay]}, rtcConfig: {iceServers: []}}
const second = {
    relayConfig: {urls: [`${relay}/second`]},
    rtcConfig: {iceServers: [{urls: 'stun:127.0.0.1:19'}]},
}
// The other peer, another page: the lobby with the first configuration, then, once this page asks,
// the kitchen with the second.
const other = openPage(async ({joinRoom, RTCPeerConnection, data: {first, second}, next}) => {
    const config = {appId: 'webcpp-trystero-differences', rtcPolyfill: RTCPeerConnection}
    const lobby = joinRoom({...config, ...first}, 'lobby')
    await next()
    const kitchen = joinRoom({...config, ...second}, 'kitchen')
    await next()
    await Promise.all([lobby.leave(), kitchen.leave()])
}, {first, second})
// The configuration whose URLs, and only they, are `urls`, each configuration's read by `urlsOf`.
const which = (urls, urlsOf) => {
    const same = c => JSON.stringify([...urls].sort()) === JSON.stringify([...urlsOf(c)].sort())
    return same(first) ? "the first configuration's" : same(second) ? "the second configuration's" : 'another'
}
// The relays the rooms use: the page's relay sockets, which every room of the page shares.
const relaysOf = () => which(Object.keys(getRelaySockets()), c => c.relayConfig.urls)
// The ICE servers of the connection a room meets a peer on.
const iceUrls = rtcConfig => rtcConfig.iceServers.flatMap(server => [server.urls].flat())
const iceServersOf = (room, peer) =>
    which(iceUrls(room.getPeers()[peer].getConfiguration()), c => iceUrls(c.rtcConfig))
const met = room => new Promise(joined => (room.onPeerJoin = joined))

// tag::example[]
// A configuration is a room's: each room is joined with its own.
const lobby = joinRoom({...config, ...first}, 'lobby')
const lobbyPeer = await met(lobby)
// The kitchen, joined with the second configuration once the lobby has met the other peer.
other.post('kitchen')
const kitchen = joinRoom({...config, ...second}, 'kitchen')
const kitchenPeer = await met(kitchen)
console.log(`the lobby's relays: ${relaysOf()}`)
console.log(`the kitchen's relays: ${relaysOf()}`)
console.log(`the lobby's connection: ${iceServersOf(lobby, lobbyPeer)} ICE servers`)
console.log(`the kitchen's connection: ${iceServersOf(kitchen, kitchenPeer)} ICE servers`)
// end::example[]
other.post('leave')
await Promise.all([lobby.leave(), kitchen.leave(), other.done])
