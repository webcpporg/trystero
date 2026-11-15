// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

import {joinRoom} from 'trystero'

const config = {appId: 'webcpp-trystero-differences', relayConfig: {urls: [process.env.TRYSTERO_RELAY_URL]}}
const first = joinRoom(config, 'lobby')
const second = joinRoom(config, 'lobby')
console.log('the first join: a room')
console.log(`the second join: ${second === first ? 'the room of the first join' : 'another room'}`)
await first.leave()
