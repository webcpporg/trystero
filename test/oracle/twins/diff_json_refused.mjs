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
// The payloads the other peer sends, as JSON text, each an array of its label and a value: one
// nested 257 deep, one holding a lone surrogate, and a last one sent after them.
const payloads = [
    ['nested 257 deep', '['.repeat(256) + ']'.repeat(256)],
    ['holding a lone surrogate', '"\\ud800"'],
    ['sent after them', '"plain"'],
]
// The other peer, another page, which sends each payload JSON.parse makes of its text once it
// meets this page, each once the one before is sent.
const other = openPage(async ({joinRoom, RTCPeerConnection, data: {config, texts}, next}) => {
    const room = joinRoom({...config, rtcPolyfill: RTCPeerConnection}, 'lobby')
    const payload = room.makeAction('payload')
    await new Promise(joined => (room.onPeerJoin = joined))
    for (const text of texts) await payload.send(JSON.parse(text))
    await next()
    await room.leave()
}, {config, texts: payloads.map(([label, value]) => `["${label}",${value}]`)})
const room = joinRoom({...config, rtcPolyfill: RTCPeerConnection}, 'lobby')

// tag::example[]
// What this page delivers of them, by label, once the last has come.
const payload = room.makeAction('payload')
const delivered = []
await new Promise(last => {
    payload.onMessage = ([label]) => {
        delivered.push(label)
        if (label === payloads.at(-1)[0]) last()
    }
})
for (const [label] of payloads) {
    console.log(`the payload ${label}: ${delivered.includes(label) ? 'delivered' : 'not delivered'}`)
}
// end::example[]
other.post('leave')
await Promise.all([room.leave(), other.done])
