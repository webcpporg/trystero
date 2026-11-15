// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

import {getRelaySockets, joinRoom} from 'trystero'

const config = {appId: 'webcpp-trystero-differences', relayConfig: {urls: [process.env.TRYSTERO_RELAY_URL]}}
// tag::example[]
const lobby = joinRoom(config, 'lobby')
const kitchen = joinRoom(config, 'kitchen')
const sockets = Object.values(getRelaySockets())
console.log(`the WebSockets the two rooms opened to their one relay: ${sockets.length}`)
// end::example[]
await Promise.all([lobby.leave(), kitchen.leave()])
