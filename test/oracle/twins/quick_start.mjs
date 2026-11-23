// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

import {joinRoom} from 'trystero'
import {RTCPeerConnection} from 'werift'
import {openPage} from '../page.mjs'

// Trystero has one peer id per app and page, so bob is another page, and each peer prints the
// other's name where the C++ program prints the ids it gave its apps. The peers meet on this machine
// alone: no ICE server, werift's WebRTC for Node.
const config = {
    appId: 'webcpp-trystero-quick-start',
    relayConfig: {urls: [process.env.TRYSTERO_RELAY_URL]},
    rtcConfig: {iceServers: []},
}

// bob's part: waits for a message, posts the line the twin prints for it, answers its sender, and
// leaves the room.
const bob = openPage(async ({joinRoom, RTCPeerConnection, data, post}) => {
    const room = joinRoom({...data, rtcPolyfill: RTCPeerConnection}, 'lobby')
    const chat = room.makeAction('chat')
    const [text, {peerId}] = await new Promise(received => {
        chat.onMessage((...message) => received(message))
    })
    post(`alice: ${text}`)
    await chat.send('hello back', {target: peerId})
    await room.leave()
}, config)

// alice greets each peer that joins, prints the answer, and leaves.
const room = joinRoom({...config, rtcPolyfill: RTCPeerConnection}, 'lobby')
const chat = room.makeAction('chat')
room.onPeerJoin(({peerId}) => chat.send('hello', {target: peerId}))
const answer = new Promise(received => {
    chat.onMessage(text => received(text))
})
console.log(await bob.next())
console.log(`bob: ${await answer}`)
await Promise.all([room.leave(), bob.done])
