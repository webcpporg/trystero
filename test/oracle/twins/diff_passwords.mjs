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
// The other peer, another page, with the same passwords for the same rooms.
const other = openPage(async ({joinRoom, RTCPeerConnection, data, next}) => {
    const lobby = joinRoom({...data, password: 'one', rtcPolyfill: RTCPeerConnection}, 'lobby')
    const kitchen = joinRoom({...data, password: 'two', rtcPolyfill: RTCPeerConnection}, 'kitchen')
    await next()
    await Promise.all([lobby.leave(), kitchen.leave()])
}, config)

// tag::example[]
// A password is a room's: one page joins rooms of different passwords.
const lobby = joinRoom({...config, password: 'one', rtcPolyfill: RTCPeerConnection}, 'lobby')
const kitchen = joinRoom({...config, password: 'two', rtcPolyfill: RTCPeerConnection}, 'kitchen')
// What the two rooms meet.
const met = room => new Promise(joined => (room.onPeerJoin = joined))
const [inLobby, inKitchen] = await Promise.all([met(lobby), met(kitchen)])
console.log('the lobby, with the password "one": joined')
console.log('the kitchen, with the password "two": joined')
console.log(`the peer in both rooms: ${inLobby === inKitchen ? 'one peer' : 'two peers'}`)
// end::example[]
other.post('leave')
await Promise.all([lobby.leave(), kitchen.leave(), other.done])
