// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

import {joinRoom} from 'trystero'

const config = {appId: 'webcpp-trystero-differences', relayConfig: {urls: [process.env.TRYSTERO_RELAY_URL]}}
const room = joinRoom(config, 'lobby')
// tag::example[]
const greeting = room.makeAction('greeting')
const outcome = await greeting.send('hello', {target: 'a-peer-that-has-not-joined'}).then(
    () => 'succeeded',
    error => `failed: ${error.message}`,
)
console.log(`the send to a peer that has not joined: ${outcome}`)
// end::example[]
await room.leave()
