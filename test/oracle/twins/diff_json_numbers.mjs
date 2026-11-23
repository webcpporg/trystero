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
// The other peer, another page, which posts what it receives for this page to print.
const other = openPage(async ({joinRoom, RTCPeerConnection, data, post, next}) => {
    const room = joinRoom({...data, rtcPolyfill: RTCPeerConnection}, 'lobby')
    room.makeAction('payload').onMessage(received => post(received))
    await next()
    await room.leave()
}, config)
const room = joinRoom({...config, rtcPolyfill: RTCPeerConnection}, 'lobby')

// tag::example[]
// The numbers of the payload's JSON text, none of which a double holds: an integer past 2^53, a
// decimal of 34 digits, and a number past a double's range.
const numbers = ['9007199254740993', '0.1000000000000000000000000000000001', '1e999']
const text = `[${numbers.join(',')}]`
// This page sends the payload its JSON text makes, once it meets the other peer.
const payload = room.makeAction('payload')
await new Promise(joined => room.onPeerJoin(joined))
await payload.send(JSON.parse(text))
// What the other peer receives, number by number.
const received = await other.next()
numbers.forEach((number, i) => console.log(`${number} arrives as ${JSON.stringify(received[i])}`))
// end::example[]
other.post('leave')
await Promise.all([room.leave(), other.done])
