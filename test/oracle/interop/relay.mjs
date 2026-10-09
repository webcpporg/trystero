// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A Nostr relay for the interop test, NIP-01 only and in memory: REQ with kinds, since and #x filters, CLOSE, and
// EVENT, which it verifies like a public relay (the id is the SHA-256 of the canonical serialization, the signature a
// valid BIP-340 one) before forwarding it to the subscriptions open at that moment. Nothing is stored: Trystero's
// kinds are ephemeral.
import {createHash} from 'node:crypto'
import {schnorr} from '@noble/secp256k1'
import {WebSocketServer} from 'ws'

export const startRelay = (port = 0) =>
    new Promise(resolve => {
        const server = new WebSocketServer({port, host: '127.0.0.1'})
        const subscriptions = new Map() // socket -> Map(subId -> filter)
        const stats = {events: 0, rejected: 0}
        // options.afterSubscribe: a raw text sent after every subscription, when not null.
        const options = {afterSubscribe: null}
        const send = (socket, message) => socket.readyState === 1 && socket.send(JSON.stringify(message))
        const matches = (filter, event, topic) =>
            (!filter.kinds || filter.kinds.includes(event.kind)) &&
            (!filter['#x'] || filter['#x'].includes(topic)) &&
            (!filter.since || event.created_at >= filter.since)
        const verify = async event => {
            const serialized = JSON.stringify([0, event.pubkey, event.created_at, event.kind, event.tags, event.content])
            const id = createHash('sha256').update(serialized).digest('hex')
            if (id !== event.id) return 'invalid: event id does not match'
            const valid = await schnorr.verifyAsync(Buffer.from(event.sig, 'hex'), Buffer.from(id, 'hex'), Buffer.from(event.pubkey, 'hex'))
            return valid ? null : 'invalid: bad signature'
        }
        server.on('connection', socket => {
            subscriptions.set(socket, new Map())
            socket.on('close', () => subscriptions.delete(socket))
            socket.on('message', async data => {
                let message
                try {
                    message = JSON.parse(String(data))
                } catch {
                    return send(socket, ['NOTICE', 'invalid: not JSON'])
                }
                const [type, ...rest] = message
                if (type === 'REQ') {
                    const [subId, filter] = rest
                    subscriptions.get(socket)?.set(subId, filter)
                    send(socket, ['EOSE', subId])
                    if (options.afterSubscribe !== null && socket.readyState === 1) socket.send(options.afterSubscribe)
                } else if (type === 'CLOSE') {
                    subscriptions.get(socket)?.delete(rest[0])
                } else if (type === 'EVENT') {
                    const event = rest[0]
                    const problem = await verify(event).catch(error => 'invalid: ' + error.message)
                    if (problem) {
                        stats.rejected++
                        return send(socket, ['OK', event.id, false, problem])
                    }
                    stats.events++
                    send(socket, ['OK', event.id, true, ''])
                    const topic = event.tags.find(t => t[0] === 'x')?.[1]
                    for (const [other, subs] of subscriptions) {
                        for (const [subId, filter] of subs) {
                            if (matches(filter, event, topic)) send(other, ['EVENT', subId, event])
                        }
                    }
                }
            })
        })
        // broadcast(text): sends a raw text to every client, as a relay that accepts anything can.
        const broadcast = text => {
            for (const socket of subscriptions.keys()) socket.readyState === 1 && socket.send(text)
        }
        server.on('listening', () =>
            resolve({url: `ws://127.0.0.1:${server.address().port}`, stats, options, broadcast, close: () => server.close()}))
    })
