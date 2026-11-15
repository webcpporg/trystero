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
    relayConfig: {urls: [process.env.TRYSTERO_RELAY_URL]},
    rtcConfig: {iceServers: []},
}
const firstApp = 'webcpp-trystero-differences'
const secondApp = 'webcpp-trystero-differences-second'
// The other peer, another page, in the rooms this page joins: it posts the id it meets first in
// each.
const other = openPage(async ({joinRoom, RTCPeerConnection, data, post, next}) => {
    const {config, firstApp, secondApp} = data
    const join = (appId, roomId) => joinRoom({...config, appId, rtcPolyfill: RTCPeerConnection}, roomId)
    const rooms = [join(firstApp, 'lobby'), join(firstApp, 'kitchen'), join(secondApp, 'lobby')]
    post(await Promise.all(rooms.map(room => new Promise(met => (room.onPeerJoin = met)))))
    await next()
    await Promise.all(rooms.map(room => room.leave()))
}, {config, firstApp, secondApp})

// tag::example[]
// Every room of the page announces the page's selfId, which Trystero draws once, as it loads.
const join = (appId, roomId) => joinRoom({...config, appId, rtcPolyfill: RTCPeerConnection}, roomId)
const rooms = [join(firstApp, 'lobby'), join(firstApp, 'kitchen'), join(secondApp, 'lobby')]
// What the other peer meets in those three rooms.
const [inLobby, inKitchen, inSecondLobby] = await other.next()
console.log(`two rooms of one app: ${inLobby === inKitchen ? 'one id' : 'two ids'}`)
console.log(`rooms of two apps: ${inLobby === inSecondLobby ? 'one id' : 'two ids'}`)
// end::example[]
other.post('leave')
await Promise.all([...rooms.map(room => room.leave()), other.done])
