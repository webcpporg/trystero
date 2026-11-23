// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

import {joinRoom} from 'trystero'
import {RTCPeerConnection} from 'werift'
import {openPage} from '../page.mjs'

// The peers meet on this machine alone: no ICE server, werift's WebRTC for Node.
const config = {
    appId: 'webcpp-trystero-differences',
    relayConfig: {urls: [process.env.TRYSTERO_RELAY_URL]},
    rtcConfig: {iceServers: []},
}
const other = openPage(async ({joinRoom, RTCPeerConnection, data, next}) => {
    const room = joinRoom({...data, rtcPolyfill: RTCPeerConnection}, 'lobby')
    await next()
    await room.leave()
}, config)
const room = joinRoom({...config, rtcPolyfill: RTCPeerConnection}, 'lobby')

// tag::example[]
const peerId = await new Promise(joined => {
    room.onPeerJoin(({peerId}) => joined(peerId))
})
const peers = room.getPeers()
console.log(`peers: ${Object.keys(peers).length}`)
const {userId, connection} = peers[peerId]
console.log(`the joined peer's entry: its ${userId ? 'user id' : 'nothing'}, with its ${connection.constructor.name}`)
// end::example[]
other.post('leave')
await Promise.all([room.leave(), other.done])
