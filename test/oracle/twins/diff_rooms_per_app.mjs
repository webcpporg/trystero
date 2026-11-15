// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

import {joinRoom} from 'trystero'

const config = {appId: 'webcpp-trystero-differences', relayConfig: {urls: [process.env.TRYSTERO_RELAY_URL]}}
const rooms = []
// What a join gives, a room kept until the end or the error that refused it.
const join = roomId => {
    try {
        rooms.push(joinRoom(config, roomId))
        return 'a room'
    } catch (error) {
        return error.message
    }
}
// tag::example[]
for (let n = 1; n <= 64; ++n) join(`room-${n}`)
console.log(`joins 1 to 64: ${rooms.length} rooms`)
console.log(`join 65: ${join('room-65')}`)
const leaving = rooms[0].leave()
console.log(`join 66, while room 1 is leaving: ${join('room-66')}`)
await leaving
console.log(`join 67, once room 1 has left: ${join('room-67')}`)
// end::example[]
await Promise.all(rooms.slice(1).map(room => room.leave()))
