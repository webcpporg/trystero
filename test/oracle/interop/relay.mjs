// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A Nostr relay for the interop test, the examples that run over a relay and their twins, NIP-01 only and in memory:
// REQ with kinds, since and #x filters, CLOSE, and EVENT, which it verifies like a public relay (the id is the SHA-256
// of the canonical serialization, the signature a valid BIP-340 one) before forwarding it to the subscriptions open
// at that moment. Nothing is stored for the clients: Trystero's kinds are ephemeral. For the interop test's checks
// it keeps what it was sent (`received`), tells how many clients subscribe to a topic (`subscribed`), and can hold the
// events a predicate picks back until `release` (`options.hold`).
import {createHash} from 'node:crypto'
import {schnorr} from '@noble/secp256k1'
import {WebSocketServer} from 'ws'

export const startRelay = (port = 0) =>
    new Promise(resolve => {
        const server = new WebSocketServer({port, host: '127.0.0.1'})
        const subscriptions = new Map() // socket -> Map(subId -> filter)
        const stats = {events: 0, rejected: 0}
        // Every event forwarded, in the order it was: {topic, peerId, offerId, signal}, of its content as JSON, `signal`
        // being 'announce' for a content without offer, answer or candidate, else the first of those it has; all null
        // for a content that is not JSON.
        const received = []
        // Events that options.hold picked, in order, forwarded by release().
        const held = []
        // options.afterSubscribe: a raw text sent after every subscription, when not null.
        // options.strike: {action, delay, topic}, when not null: the next subscription to `topic` takes it, and `delay`
        // milliseconds later the relay resets that client's TCP connection (action 'reset') or sends it a text longer
        // than 1 MiB (action 'too-long'). A C++ peer announces 233 ms after it subscribed, so a delay just below that
        // ends its connection as that write starts.
        // options.hold: (topic, content) => boolean, when not null: an event it picks is acknowledged but held back from
        // the subscriptions until release().
        const options = {afterSubscribe: null, strike: null, hold: null}
        const tooLong = 'x'.repeat(1024 * 1024 + 1)
        const strikeLater = (socket, {action, delay}) =>
            setTimeout(() => {
                if (socket.readyState !== 1) return
                if (action === 'reset') socket._socket.resetAndDestroy()
                else socket.send(tooLong)
            }, delay)
        const send = (socket, message) => socket.readyState === 1 && socket.send(JSON.stringify(message))
        const contentOf = event => {
            try {
                return JSON.parse(event.content)
            } catch {
                return null
            }
        }
        const signalOf = content =>
            ['offer', 'answer', 'candidate'].find(k => typeof content?.[k] === 'string') ?? 'announce'
        const forward = (event, topic) => {
            const content = contentOf(event)
            received.push({
                topic, peerId: content?.peerId ?? null, offerId: content?.offerId ?? null, signal: content ? signalOf(content) : null,
            })
            for (const [other, subs] of subscriptions) {
                for (const [subId, filter] of subs) {
                    if (matches(filter, event, topic)) send(other, ['EVENT', subId, event])
                }
            }
        }
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
                    if (options.strike !== null && filter?.['#x']?.includes(options.strike.topic)) {
                        strikeLater(socket, options.strike)
                        options.strike = null
                    }
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
                    // Forwarded before the OK: a client that waits for its OK knows every subscription has the event
                    // before anything it sends next.
                    const topic = event.tags.find(t => t[0] === 'x')?.[1]
                    if (options.hold?.(topic, contentOf(event))) held.push([event, topic])
                    else forward(event, topic)
                    send(socket, ['OK', event.id, true, ''])
                }
            })
        })
        // broadcast(text): sends a raw text to every client, as a relay that accepts anything can.
        const broadcast = text => {
            for (const socket of subscriptions.keys()) socket.readyState === 1 && socket.send(text)
        }
        // release(): ends options.hold and forwards what it held, in order.
        const release = () => {
            options.hold = null
            for (const [event, topic] of held.splice(0)) forward(event, topic)
        }
        // subscribed(topic): how many open clients subscribe to `topic`.
        const subscribed = topic =>
            [...subscriptions.entries()].filter(([socket, subs]) =>
                socket.readyState === 1 && [...subs.values()].some(f => f?.['#x']?.includes(topic))).length
        server.on('listening', () =>
            resolve({
                url: `ws://127.0.0.1:${server.address().port}`, stats, options, received, broadcast, release, subscribed,
                close: () => server.close(),
            }))
    })
