// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Writes <output-directory>/expected.json: values computed by Trystero JS 0.27.0 itself, the
// published @trystero-p2p/core and @trystero-p2p/nostr this directory's package-lock.json pins,
// from the inputs of <cases>/protocol.json, which the C++ protocol and signaling tests must
// reproduce byte for byte. b2 runs it: `b2 libs/trystero/test/oracle//cases-protocol` compares
// what it writes with test/fixtures/oracle, and `b2 libs/trystero/test/oracle//update-expected`
// writes that directory again.
//
//   node oracle.mjs <cases> <output-directory>
//
// The oracle waits on nothing but what it runs: every value it records is the end of a chain of
// promises or callbacks of Trystero's own, never a wall-clock delay.
import {mkdirSync, readFileSync, writeFileSync} from 'node:fs'
import {dirname, join} from 'node:path'
import {fileURLToPath} from 'node:url'
import {schnorr} from '@noble/secp256k1'

if (process.argv.length !== 4) {
    console.error('usage: node oracle.mjs <cases> <output-directory>')
    process.exit(2)
}
const [casesDirectory, outputDirectory] = process.argv.slice(2)
const cases = JSON.parse(readFileSync(join(casesDirectory, 'protocol.json'), 'utf8'))

// The modules of the published packages, by their files: the signal handler and the topics are
// not exported from the packages' entry points.
const modules = join(dirname(fileURLToPath(import.meta.url)), 'node_modules/@trystero-p2p')
const {hashTopic, genKey, encrypt, decrypt} = await import(join(modules, 'core/dist/crypto.mjs'))
const {topicPath, toJson, libName, candidateType} = await import(join(modules, 'core/dist/utils.mjs'))
const {createSignalHandler, buildSignalAad, createPeerState} =
    await import(join(modules, 'core/dist/signal-handler.mjs'))
const {createEvent, topicToKind} = await import(join(modules, 'nostr/dist/protocol.mjs'))
const {joinRoom} = await import('@trystero-p2p/nostr')

// The bytes getRandomValues gives while a fixture draws them, the same every run: byte i of a draw
// is (i * multiplier + offset) % 256. Trystero draws a GCM IV with it when it seals, and noble's
// secp256k1 a key and a signature's auxiliary bytes.
const patterned = ({multiplier, offset}) => array => array.map((_, i) => (i * multiplier + offset) % 256)
const getRandomValues = crypto.getRandomValues.bind(crypto)

const hex = bytes => Buffer.from(bytes).toString('hex')
const out = {}

// A room's root topic, as 0.27.0's strategy names it: hashTopic of topicPath(libName, app, room).
const rootTopicText = (appId, roomId) => topicPath(libName, appId, roomId)
const rootTopic = (appId, roomId) => hashTopic(rootTopicText(appId, roomId))

// Event messages, written by 0.27.0's own createEvent, `["EVENT",{kind,tags,created_at,content,
// pubkey,id,sig}]`, whose topics and contents hold the characters JSON.stringify escapes and those
// it writes as they are: the controls U+0000 to U+001F, `"`, `\`, U+2028, U+2029, DEL and astral
// ones. noble's keygen makes the key from the fixture's 48 bytes, so that the C++ signer signs
// with it, and the clock and the signature's auxiliary bytes are the fixture's.
const messages = cases.event_messages
const draw = patterned(messages.random)
const {secretKey} = schnorr.keygen(draw(new Uint8Array(48)))
const pinned = async (seconds, f) => {
    const now = Date.now
    Date.now = () => seconds * 1000
    crypto.getRandomValues = draw
    try {
        return await f()
    } finally {
        crypto.getRandomValues = getRandomValues
        Date.now = now
    }
}
out.event_messages = {
    secret_key: hex(secretKey),
    aux: hex(draw(new Uint8Array(32))),
    created_at: messages.created_at,
    events: [],
}
await pinned(messages.created_at, async () => {
    const signer = {secretKey, pubkey: hex(schnorr.getPublicKey(secretKey))}
    for (const [topic, content] of messages.events) {
        const message = await createEvent(topic, content, signer)
        out.event_messages.events.push({topic, content, id: JSON.parse(message)[1].id, message})
    }
})

// Nostr event ids, sha256 of JSON.stringify([0, pubkey, created_at, kind, tags, content]), as
// 0.27.0's createEvent computes them for an event of the fixture's room's root topic, under the
// fixture's public key: createEvent takes the public key it writes from its signer, and the id
// covers no secret.
{
    const events = cases.nostr_events
    const topic = await rootTopic(events.app_id, events.room_id)
    const signer = {secretKey, pubkey: events.pubkey}
    out.nostr_events = []
    await pinned(events.created_at, async () => {
        for (const content of events.contents) {
            const event = JSON.parse(await createEvent(topic, content, signer))[1]
            out.nostr_events.push({pubkey: event.pubkey, created_at: event.created_at, kind: event.kind,
                topic, content, id: event.id})
        }
    })
    if (out.nostr_events.some(e => e.kind !== topicToKind(topic))) {
        throw new Error('createEvent wrote a kind topicToKind does not give')
    }
}

// Signals' texts, written as 0.27.0 writes each, with toJson (JSON.stringify), their members in
// the order its signal handler gives them: an announcement {peerId} with the Nostr strategy's
// nonce after it, an offer {peerId, offerId, offer}, and the offerer's and the answerer's answer and
// candidate {peerId, offerId, answer} and {peerId, offerId, candidate}; their strings hold the event
// messages' characters.
out.signal_texts = cases.signal_texts.map(signal => ({signal, text: toJson(signal)}))

/**
 * The context of a signal handler of the oracle's: a room of the app, the room and the self id that
 * `room` names, whose key is that of a room without a password, sealing and opening with each
 * signal's AAD as the strategy does (toPlain, toCipher), and which makes no offer and no peer.
 * `hooks` replaces any member.
 */
const signalContext = async ({app_id: appId, room_id: roomId, self_id: selfId}, hooks) => {
    const root = rootTopicText(appId, roomId)
    const key = genKey('', appId, roomId)
    const withKey = f => async (signal, peerId, offerId) => ({
        type: signal.type,
        sdp: await f(key, signal.sdp, buildSignalAad(signal.type, peerId, offerId)),
    })
    return {
        appId,
        getRoomId: () => roomId,
        config: {},
        peerStates: new Map(),
        getRootTopicPlaintext: () => root,
        rootTopicP: hashTopic(root),
        selfTopicP: hashTopic(topicPath(root, selfId)),
        toPlain: withKey(decrypt),
        toCipher: withKey(encrypt),
        isLeaving: () => false,
        onError: () => {},
        checkoutOffers: () => Promise.reject(new Error('the oracle makes no offer')),
        claimLeasedOffer: () => false,
        reclaimLeasedOffer: () => {},
        encryptOffer: () => Promise.reject(new Error('the oracle makes no offer')),
        initPeer: () => {
            throw new Error('the oracle makes no peer here')
        },
        connectPeer: () => {},
        disconnectPeer: () => {},
        announceIntervals: [],
        announceIntervalMs: 5333,
        getSelfIdNow: () => selfId,
        selfIdP: Promise.resolve(selfId),
        ...hooks,
    }
}

// Signals whose objects hold a key twice, as 0.27.0's own signal handler reads them: each runs in
// a room of the oracle's, on its root topic, which records the peer id it acts on
// (onPeerDiscovered) and the sealed text it opens (toPlain), with its offer id, by kind, and opens
// nothing, so that the handler stops there. The room connects to no peer it would dial
// (shouldConnectPeer), so that an announcement goes no further; a candidate is opened only for a
// peer the room answers, so a second run, whose room answers every peer, reads the candidates.
// `read` is null when 0.27.0 ignores the signal.
{
    const room = cases.duplicate_keys
    const readSignal = async (text, answering) => {
        const read = {}
        const ctx = await signalContext(room, {
            onPeerDiscovered: peerId => {
                read.peer_id = peerId
            },
            shouldConnectPeer: (_, direction) => direction === 'inbound',
            toPlain: async ({type, sdp}, peerId, offerId) => {
                read[type] = sdp
                read.offer_id = offerId
                throw new Error('the oracle opens nothing')
            },
        })
        if (answering) {
            // Every peer is one the room answers.
            const state = createPeerState()
            const peer = {isDead: false, setHandlers: () => {}, signal: async () => {}, destroy: () => {}}
            state.inbound = {phase: 'answering', peer, offerId: '', expiryTimer: undefined,
                didSendAnswer: false, replay: {offer: '', offerId: '', messages: [], lastSentAt: [], relays: []}}
            ctx.peerStates = new (class extends Map {
                get(peerId) {
                    return super.get(peerId) ?? state
                }
            })()
        }
        const {handleMessage} = createSignalHandler(ctx)
        await handleMessage(0)(await ctx.rootTopicP, text, async () => {})
        return read
    }
    out.duplicate_keys = {self_id: room.self_id, signals: []}
    for (const text of room.signals) {
        const read = await readSignal(text, false)
        const second = await readSignal(text, true)
        if (second.candidate !== undefined) {
            read.candidate = second.candidate
            read.offer_id = second.offer_id
        }
        out.duplicate_keys.signals.push({text, read: read.peer_id === undefined ? null : read})
    }
}

// Relay events whose objects hold a key twice, as 0.27.0's own Nostr strategy reads them. A room
// of the cases' app joins through a WebSocket of the oracle's, which hands it each case's members
// as an event of the room's subscription, keys as they are written and values as toJson writes
// them, `{root}` being the room's root topic. Every content is an answer that does not open, so
// that the room reports its sender (onError, decrypt-failed) exactly when it acted on the event:
// when it read the event's topic as the room's root topic and its content as the signal. After
// each case comes an event of the oracle's own, whose report closes the case's. `acted` is the
// sender the room reported, or null; in what is written the subscription's id, which Trystero
// draws, is `sub`.
class OracleSocket {
    static opened = []

    constructor(url) {
        this.url = url
        this.readyState = 0
        this.sent = []
        this.watchers = []
        OracleSocket.opened.push(this)
        queueMicrotask(() => {
            this.readyState = 1
            this.onopen?.()
        })
    }

    /** Resolves to the first text sent, or already sent, for which `wanted` holds. */
    awaitSent(wanted) {
        const found = this.sent.find(wanted)
        return found !== undefined ? Promise.resolve(found)
            : new Promise(resolve => this.watchers.push({wanted, resolve}))
    }

    send(text) {
        this.sent.push(text)
        this.watchers = this.watchers.filter(({wanted, resolve}) => !(wanted(text) && (resolve(text), true)))
    }

    close() {
        this.readyState = 3
    }
}
{
    const events = cases.duplicate_keys.events
    const root = await rootTopic(events.app_id, events.room_id)
    const eventText = (subscription, members) => {
        const fields = members.map(([key, value]) => `"${key}":${toJson(value)}`).join(',')
        return `["EVENT",${toJson(subscription)},{${fields}}]`.replaceAll('{root}', root)
    }
    const WebSocket = globalThis.WebSocket
    globalThis.WebSocket = OracleSocket
    const room = joinRoom({appId: events.app_id, relayConfig: {urls: ['wss://relay.oracle.invalid']}},
        events.room_id)
    // The reports of decrypt-failed, by sender, each resolving whoever waits for it.
    const acted = []
    const waiting = new Map()
    room.onError(error => {
        if (error.code !== 'decrypt-failed') {
            return
        }
        acted.push(error.peerId)
        waiting.get(error.peerId)?.()
    })
    const reported = peerId => new Promise(resolve => waiting.set(peerId, resolve))
    const [socket] = OracleSocket.opened
    const request = await socket.awaitSent(text => {
        const [type, , filter] = JSON.parse(text)
        return type === 'REQ' && filter['#x'].includes(root)
    })
    const subscription = JSON.parse(request)[1]
    out.duplicate_keys.root_topic = root
    out.duplicate_keys.events = []
    for (const [i, members] of events.texts.entries()) {
        acted.length = 0
        const closing = `${events.closing_prefix}${i}`.padEnd(20, '0')
        const closed = reported(closing)
        socket.onmessage({data: eventText(subscription, members)})
        socket.onmessage({data: eventText(subscription, [
            ['tags', [['x', '{root}']]],
            ['content', toJson({peerId: closing, offerId: events.offer_id, answer: 'x'})],
        ])})
        await closed
        const senders = acted.filter(peerId => peerId !== closing)
        if (senders.length > 1) throw new Error(`one event reported ${senders.length} senders`)
        out.duplicate_keys.events.push({text: eventText('sub', members), acted: senders[0] ?? null})
    }
    await room.leave()
    globalThis.WebSocket = WebSocket
}

// The answerer's signals, as 0.27.0's own signal handler publishes them (handleOffer): for each
// case, a room of the oracle's gets an offer on its self topic, sealed under the room's key with
// the offer's AAD, opens it, and hands it to its answering peer, a stand-in that gives an answer
// and then a candidate, as an RTCIceCandidate's toJSON in JSON.stringify's text (peer.mjs); the
// room seals each with its AAD, the fixture's IV drawn every time, and publishes the answer, then
// the candidate it held, on the offerer's topic. Each published signal carries the plaintext its
// sealed text holds and that sealed text, so that a test can put its own in its place.
{
    const answerer = cases.answerer
    const candidate = JSON.stringify(answerer.candidate)
    out.answerer = {
        self_id: answerer.self_id,
        app_id: answerer.app_id,
        room_id: answerer.room_id,
        offer_sdp: answerer.offer_sdp,
        answer_sdp: answerer.answer_sdp,
        candidate,
        cases: [],
    }
    crypto.getRandomValues = patterned(answerer.random)
    for (const {peerId, offerId} of answerer.offers) {
        const sealed = new Map()
        const published = []
        let settle
        const settled = new Promise((resolve, reject) => {
            settle = {resolve, reject}
        })
        const handlers = {}
        const ctx = await signalContext(answerer, {
            onError: error => settle.reject(new Error(`the room reported ${error.code}`)),
            initPeer: () => ({
                isDead: false,
                setHandlers: given => Object.assign(handlers, given),
                signal: async description => {
                    if (description.type !== 'offer' || description.sdp !== answerer.offer_sdp) {
                        throw new Error('the oracle answers its own offer alone')
                    }
                    handlers.signal({type: 'answer', sdp: answerer.answer_sdp})
                    handlers.signal({type: candidateType, sdp: candidate})
                },
                destroy: () => {},
            }),
        })
        const toCipher = ctx.toCipher
        ctx.toCipher = async (signal, ...rest) => {
            const cipher = await toCipher(signal, ...rest)
            sealed.set(cipher.sdp, signal.sdp)
            return cipher
        }
        const offer = (await ctx.toCipher({type: 'offer', sdp: answerer.offer_sdp}, peerId, offerId)).sdp
        const text = toJson({peerId, offerId, offer})
        const {handleMessage} = createSignalHandler(ctx)
        await handleMessage(0)(await ctx.selfTopicP, text, async (topic, message) => {
            const signal = JSON.parse(message)
            const text = signal.answer ?? signal.candidate
            published.push({topic, text: message, sealed: text, plaintext: sealed.get(text)})
            if (published.length === 2) settle.resolve()
        })
        await settled
        out.answerer.cases.push({offer: text, published})
    }
    crypto.getRandomValues = getRandomValues
}

// JSON payloads, as 0.27.0 sends one: the value an action is given, written by toJson
// (JSON.stringify), here each case's text as JSON.parse reads it. Their numbers are those a double
// holds, written as JavaScript writes them, and their keys in JavaScript's order: array indices
// first, in increasing order, then the other keys in the order they were written.
out.payload_texts = cases.payload_texts.map(text => ({text, written: toJson(JSON.parse(text))}))
// And numbers a double cannot hold, which JSON.parse rounds to the nearest double, or to Infinity,
// which JSON.stringify writes null.
out.payload_beyond_doubles =
    cases.payload_beyond_doubles.map(text => ({text, written: toJson(JSON.parse(text))}))

mkdirSync(outputDirectory, {recursive: true})
const target = join(outputDirectory, 'expected.json')
writeFileSync(target, JSON.stringify(out, null, 4) + '\n')
console.log('wrote ' + target)
process.exit(0)
