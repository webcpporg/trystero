// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Writes <output-directory>/expected.json: values computed by Trystero JS 0.26 itself, from the inputs of
// <cases>/protocol.json, which the C++ protocol tests must reproduce byte for byte. b2 runs it:
// `b2 libs/trystero/test/oracle//oracle` compares what it writes with test/fixtures/expected, and
// `b2 libs/trystero/test/oracle//update-expected` writes that directory again.
//
//   node oracle.mjs <cases> <output-directory>
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

// Trystero draws its peer id, selfId, from Math.random once, when utils.mjs is loaded: from the
// answerer fixture's draws while the modules load, so that the signals carrying it are the same
// every run. Draw i is ((i * multiplier + offset) % 62 + 0.5) / 62, which picks character
// (i * multiplier + offset) % 62 of Trystero's alphabet.
const random = Math.random
{
    const {multiplier, offset} = cases.answerer.self_id_random
    let draw = 0
    Math.random = () => ((((draw++ * multiplier) + offset) % 62) + 0.5) / 62
}
const core = join(dirname(fileURLToPath(import.meta.url)), 'node_modules/@trystero-p2p/core/dist')
const {sha1, hashWith, deriveRoomNamespace, encrypt, decrypt, genKey} = await import(join(core, 'crypto.mjs'))
const {strToNum, getRelays, toHex, toJson, topicPath, shuffle, selfId} = await import(join(core, 'utils.mjs'))
const {createActionWireManager} = await import(join(core, 'action-wire.mjs'))
const {SharedPeerManager} = await import(join(core, 'shared-peer.mjs'))
const {createSignalHandler} = await import(join(core, 'signal-handler.mjs'))
Math.random = random

// The bytes getRandomValues gives while a fixture draws them, the same every run: byte i of a draw
// is (i * multiplier + offset) % 256. Trystero draws an IV with it when it seals, and noble's
// secp256k1 a key and a signature's auxiliary bytes.
const patterned = ({multiplier, offset}) => array => array.map((_, i) => (i * multiplier + offset) % 256)
const getRandomValues = crypto.getRandomValues.bind(crypto)

// Trystero's Nostr strategy draws its key once, when it is loaded: from the bytes of the event
// messages' fixture, so that its events are the same every run and the C++ signer can sign them.
crypto.getRandomValues = patterned(cases.event_messages.random)
const {defaultRelayUrls, createEvent, joinRoom} = await import('@trystero-p2p/nostr')
crypto.getRandomValues = getRandomValues

const hex = bytes => Buffer.from(bytes).toString('hex')
const out = {}

// Topics: Trystero's sha1 text (each digest byte in base 36) of the root and self topic paths.
out.topics = []
for (const [appId, roomId, peerId] of cases.topics) {
    const root = topicPath('Trystero', appId, roomId)
    out.topics.push({
        app_id: appId,
        room_id: roomId,
        peer_id: peerId,
        root_topic: await sha1(root),
        self_topic: await sha1(topicPath(root, peerId)),
        room_token: await deriveRoomNamespace(appId, roomId),
    })
}

// strToNum sums UTF-16 code units, so non-ASCII text must be summed the same way.
out.str_to_num = cases.str_to_num.map(text => ({
    text,
    unbounded: strToNum(text),
    mod_10000: strToNum(text, 10_000),
}))

// Relay selection: the default list shuffled with a seed derived from the app id, first five kept.
out.default_relays = defaultRelayUrls
out.relays = cases.relays.map(
    appId => ({app_id: appId, relays: getRelays({appId}, defaultRelayUrls, cases.relay_redundancy, true)}))
out.shuffle = cases.shuffle.seeds.map(seed => ({seed, order: shuffle([...Array(cases.shuffle.length).keys()], seed)}))

// The password handshake: sha256 hex of "challenge:password:appId:roomId".
out.handshake = cases.handshake.map(([challenge, password, appId, roomId]) => ({
    challenge, password, app_id: appId, room_id: roomId,
}))
for (const entry of out.handshake) {
    entry.hash = toHex(await hashWith('SHA-256', `${entry.challenge}:${entry.password}:${entry.app_id}:${entry.room_id}`))
}

// Sealed signals: "iv,iv,...$base64(ciphertext)" with a 16-byte IV; the tag makes the ciphertext 16 bytes longer.
const sealed = cases.sealed
const key = genKey(sealed.password, sealed.app_id, sealed.room_id)
const plaintext = sealed.plaintext
// A fixed IV keeps the fixture deterministic; Trystero draws it from getRandomValues.
crypto.getRandomValues = patterned(sealed.random)
const sealedText = await encrypt(key, plaintext)
crypto.getRandomValues = getRandomValues
out.sealed = {
    plaintext,
    text: sealedText,
    key_material: toHex(await hashWith('SHA-256', `${sealed.password}:${sealed.app_id}:${sealed.room_id}`)),
}

// Nostr event ids: sha256 of JSON.stringify([0, pubkey, created_at, kind, tags, content]).
const pubkey = cases.nostr_events.pubkey
out.nostr_events = []
for (const content of cases.nostr_events.contents) {
    const topic = out.topics[0].root_topic
    const kind = strToNum(topic, 10_000) + 20_000
    const createdAt = cases.nostr_events.created_at
    out.nostr_events.push({pubkey, created_at: createdAt, kind, topic, content,
        id: toHex(await hashWith('SHA-256', toJson([0, pubkey, createdAt, kind, [['x', topic]], content])))})
}

// Action frames, captured from Trystero's own action wire manager sending to a fake peer.
const actions = cases.actions
const chunked = actions.chunked
const captureActions = async () => {
    const frames = []
    let accept = () => {}
    const peer = {channel: null, sendData: bytes => frames.push(hex(bytes))}
    const wire = createActionWireManager({
        getPeer: () => peer,
        getPeerIds: () => ['peer'],
        canReceiveFromPeer: () => true,
        onPeerError: (_, error) => { throw error },
    })
    const send = async (type, data, metadata) => {
        frames.length = 0
        await wire.makeInternalAction(type).send(data, 'peer', metadata)
        return [...frames]
    }
    const result = {
        text: await send(...actions.text),
        json: await send(...actions.json),
        binary: await send(actions.binary[0], new Uint8Array(actions.binary[1])),
        text_with_metadata: await send(...actions.text_with_metadata),
        empty_text: await send(...actions.empty_text),
        thirty_two_byte_name: await send(...actions.thirty_two_byte_name),
    }
    // A payload above the inline limit becomes an offer, then chunks once the receiver accepts.
    frames.length = 0
    const big = new Uint8Array(chunked.length).map((_, i) => (i * chunked.multiplier) % 256)
    const sending = wire.makeInternalAction(chunked.type).send(big, 'peer', chunked.metadata)
    await new Promise(resolve => setTimeout(resolve, 10))
    const offer = Buffer.from(frames[0], 'hex')
    const id = offer.readUInt32BE(36)
    wire.handleData('peer', new Uint8Array([2, 3, ...[id >>> 24, (id >>> 16) & 255, (id >>> 8) & 255, id & 255]]).buffer)
    await sending
    result.chunked = {payload: hex(big), frames: [...frames]}
    return result
}
out.actions = await captureActions()

// The control frames a receiver answers with: accepted, rejected with a reason, cancelled.
{
    const frames = []
    const peer = {channel: null, sendData: bytes => frames.push(hex(bytes))}
    const receiver = createActionWireManager({
        getPeer: () => peer,
        getPeerIds: () => ['peer'],
        canReceiveFromPeer: () => true,
        onPeerError: () => {},
        maxReceiveBytes: cases.controls.max_receive_bytes,
    })
    receiver.makeInternalAction(chunked.type).onMessage(() => {})
    const offer = Buffer.from(out.actions.chunked.frames[0], 'hex')
    receiver.handleData('peer', new Uint8Array(offer).buffer)  // the chunked payload: above maxReceiveBytes, so rejected
    out.controls = {rejected_too_large: frames[0]}
}

// Room envelope and presence frames, captured from Trystero's shared peer manager over a fake physical peer.
{
    const envelope = cases.envelope
    const frames = []
    const physical = {
        isDead: false,
        connection: {connectionState: 'connected', iceConnectionState: 'connected', getSenders: () => []},
        channel: {readyState: 'open'},
        sendData: bytes => frames.push(hex(bytes)),
        setHandlers: () => {},
        destroy: () => {},
    }
    const manager = new SharedPeerManager()
    const token = await deriveRoomNamespace(envelope.app_id, envelope.room_id)
    const membership = manager.registerRoom(envelope.app_id, envelope.room_id, Promise.resolve(token), {
        active: true,
        onPeer: proxy => proxy.sendData(new Uint8Array(envelope.frame)),
        onDetach: () => {},
    })
    await new Promise(resolve => setTimeout(resolve, 0))
    membership.connect('remote-peer', physical, envelope.timeout)
    await new Promise(resolve => setTimeout(resolve, 0))
    membership.leave()
    out.envelope = {room_token: token, frames: [...frames]}
}

// Event messages, written by Trystero's own createEvent, `["EVENT",{kind,tags,created_at,content,
// pubkey,id,sig}]`, whose topics and contents hold the characters JSON.stringify escapes and those
// it writes as they are: the controls U+0000 to U+001F, `"`, `\`, U+2028, U+2029, DEL and astral
// ones. The key is the one the strategy drew when it was loaded, and the clock and the signature's
// auxiliary bytes are the fixture's; noble's keygen makes the key again from the same 48 bytes, so
// that the C++ signer signs with it.
{
    const messages = cases.event_messages
    const draw = patterned(messages.random)
    const {secretKey} = schnorr.keygen(draw(new Uint8Array(48)))
    const now = Date.now
    Date.now = () => messages.created_at * 1000
    crypto.getRandomValues = draw
    out.event_messages = {
        secret_key: hex(secretKey),
        aux: hex(draw(new Uint8Array(32))),
        created_at: messages.created_at,
        events: [],
    }
    for (const [topic, content] of messages.events) {
        const message = await createEvent(topic, content)
        out.event_messages.events.push({topic, content, id: JSON.parse(message)[1].id, message})
    }
    crypto.getRandomValues = getRandomValues
    Date.now = now
}

// Signals' texts, written as Trystero writes each, with toJson (JSON.stringify), their members in
// the order Trystero gives them: an announcement {peerId, passive, nonce} (the topic strategy's
// announce, then the Nostr strategy's nonce), an offer {peerId, offerId, offer} and the offerer's
// candidate {peerId, offerId, candidate, passive} (handleAnnouncement), an answer to an offer
// without an id {peerId, answer}, and the answerer's answer and candidate, whose offerId follows
// them {peerId, answer, offerId} and {peerId, candidate, offerId, passive} (handleOffer's
// publishAnswerSignal); their strings hold the event messages' characters.
out.signal_texts = cases.signal_texts.map(signal => ({signal, text: toJson(signal)}))

// Signals whose objects hold a key twice, as Trystero's own signal handler reads them: it runs in a
// room of the oracle's, which records the peer id it looks up (reusePeer), the sealed text it opens
// (toPlain), by kind, and an offer's id, and opens nothing, so that the handler stops there. A
// passive room ignores a passive sender before it looks the peer up, which a second run, in such a
// room, shows. `read` is null when Trystero ignores the signal.
const readSignal = async text => {
    const run = async passive => {
        const read = {}
        const ctx = {
            appId: 'app',
            roomId: 'room',
            config: {},
            peerStates: {},
            rootTopicPlaintext: topicPath('Trystero', 'app', 'room'),
            rootTopicP: Promise.resolve('root'),
            selfTopicP: Promise.resolve('self'),
            isPassive: passive,
            isActive: true,
            // The room leaves once it looked the peer up, so that an announcement makes no offer.
            isLeaving: () => read.peer_id !== undefined,
            reusePeer: peerId => {
                read.peer_id = peerId
                return false
            },
            toPlain: async ({type, sdp}) => {
                read[type] = sdp
                if (type === 'offer') read.offer_id = ctx.peerStates[read.peer_id].answerReplay.offerId
                throw new Error('the oracle opens nothing')
            },
            initPeer: () => ({
                isDead: false,
                setHandlers: () => {},
                signal: async () => {},
                destroy: () => {},
            }),
            offerManager: {checkout: async () => Promise.reject(new Error('the oracle makes no offer'))},
            connectPeer: () => {},
            disconnectPeer: () => {},
            checkDeactivate: () => {},
            announceIntervals: [],
            announceIntervalMs: 5333,
        }
        await createSignalHandler(ctx)(0)('root', text, () => {})
        return read
    }
    const read = await run(false)
    if (read.peer_id === undefined) return null
    read.passive = (await run(true)).peer_id === undefined
    return read
}
out.duplicate_keys = {signals: []}
for (const text of cases.duplicate_keys.signals) {
    out.duplicate_keys.signals.push({text, read: await readSignal(text)})
}

// Relay events whose objects hold a key twice, as Trystero's own Nostr strategy reads them. A room
// of the cases' app joins through a WebSocket of the oracle's, which hands it each case's members
// as an event of the room's subscription, keys as they are written and values as toJson writes
// them, `{root}` being the room's root topic. Every content is an answer that does not open, so
// that the room reports its sender (onJoinError) exactly when it acted on the event: when it read
// the event's topic as the room's root topic and its content as the signal. After each case comes
// an event of the oracle's own, whose report closes the case's. `acted` is the sender the room
// reported, or null; in what is written the subscription's id, which Trystero draws, is `sub`.
class OracleSocket {
    static opened = []

    constructor(url) {
        this.url = url
        this.readyState = 0
        this.sent = []
        OracleSocket.opened.push(this)
        setTimeout(() => {
            this.readyState = 1
            this.onopen?.()
        }, 0)
    }

    send(text) {
        this.sent.push(text)
    }

    close() {
        this.readyState = 3
    }
}
const until = async (done, what) => {
    for (let waited = 0; !done(); waited += 10) {
        if (waited >= 10_000) throw new Error(`the oracle waited 10 s for ${what}`)
        await new Promise(resolve => setTimeout(resolve, 10))
    }
}
{
    const events = cases.duplicate_keys.events
    const rootTopic = await sha1(topicPath('Trystero', events.app_id, events.room_id))
    const eventText = (subscription, members) => {
        const fields = members.map(([key, value]) => `"${key}":${toJson(value)}`).join(',')
        return `["EVENT",${toJson(subscription)},{${fields}}]`.replaceAll('{root}', rootTopic)
    }
    const acted = []
    const WebSocket = globalThis.WebSocket
    globalThis.WebSocket = OracleSocket
    const room = joinRoom({appId: events.app_id, relayConfig: {urls: ['wss://relay.oracle.invalid']}},
        events.room_id, {onJoinError: ({peerId}) => acted.push(peerId)})
    const [socket] = OracleSocket.opened
    const subscribed = () => socket.sent.map(text => JSON.parse(text))
        .find(([type, , filter]) => type === 'REQ' && filter['#x'].includes(rootTopic))
    await until(subscribed, 'the subscription to the root topic')
    const subscription = subscribed()[1]
    out.duplicate_keys.root_topic = rootTopic
    out.duplicate_keys.events = []
    for (const [i, members] of events.texts.entries()) {
        acted.length = 0
        socket.onmessage({data: eventText(subscription, members)})
        const closing = `oracle-${i}`
        socket.onmessage({data: eventText(subscription, [
            ['tags', [['x', '{root}']]],
            ['content', toJson({peerId: closing, answer: 'x'})],
        ])})
        await until(() => acted.includes(closing), `the report of ${closing}`)
        const senders = acted.filter(peerId => peerId !== closing)
        if (senders.length > 1) throw new Error(`one event reported ${senders.length} senders`)
        out.duplicate_keys.events.push({text: eventText('sub', members), acted: senders[0] ?? null})
    }
    await room.leave()
    globalThis.WebSocket = WebSocket
}

// The answerer's signals, as Trystero's own signal handler publishes them (handleOffer): for each
// case, a room of the oracle's, as readSignal's, gets an offer on its self topic, sealed under the
// room's key, opens it, and hands it to its answering peer, a stand-in that gives an answer and
// then a candidate, as an RTCIceCandidate's toJSON in JSON.stringify's text (peer.mjs); the room
// seals each with Trystero's encrypt, the fixture's IV drawn every time, and publishes the answer,
// then the candidate it held. `sealed` holds every text sealed, the offers' and the room's, with
// its plaintext; `published`, the topic and the text of each signal the room sent, in order.
{
    const answerer = cases.answerer
    const key = genKey(answerer.password, answerer.app_id, answerer.room_id)
    const root = topicPath('Trystero', answerer.app_id, answerer.room_id)
    const candidate = JSON.stringify(answerer.candidate)
    const sealed = []
    const seal = async plaintext => {
        const text = await encrypt(key, plaintext)
        if (!sealed.some(entry => entry.plaintext === plaintext)) sealed.push({plaintext, text})
        return text
    }
    crypto.getRandomValues = patterned(answerer.random)
    const offer = await seal(answerer.offer_sdp)
    out.answerer = {
        self_id: selfId,
        app_id: answerer.app_id,
        room_id: answerer.room_id,
        answer_sdp: answerer.answer_sdp,
        candidate,
        sealed,
        cases: [],
    }
    for (const {peerId, offerId} of answerer.offers) {
        const handlers = {}
        const errors = []
        const published = []
        const ctx = {
            appId: answerer.app_id,
            roomId: answerer.room_id,
            config: {},
            peerStates: {},
            rootTopicPlaintext: root,
            rootTopicP: sha1(root),
            selfTopicP: sha1(topicPath(root, selfId)),
            isPassive: false,
            isActive: true,
            isLeaving: () => false,
            reusePeer: () => false,
            toPlain: async ({type, sdp}) => ({type, sdp: await decrypt(key, sdp)}),
            toCipher: async ({type, sdp}) => ({type, sdp: await seal(sdp)}),
            initPeer: () => ({
                isDead: false,
                setHandlers: given => Object.assign(handlers, given),
                signal: async description => {
                    if (description.type !== 'offer' || description.sdp !== answerer.offer_sdp) {
                        throw new Error('the oracle answers its own offer alone')
                    }
                    handlers.signal({type: 'answer', sdp: answerer.answer_sdp})
                    handlers.signal({type: 'candidate', sdp: candidate})
                },
                destroy: () => {},
            }),
            offerManager: {checkout: async () => Promise.reject(new Error('the oracle makes no offer'))},
            connectPeer: () => {},
            disconnectPeer: () => {},
            checkDeactivate: () => {},
            onJoinError: ({error}) => errors.push(error),
            announceIntervals: [],
            announceIntervalMs: 5333,
        }
        const text = toJson({peerId, ...(offerId === undefined ? {} : {offerId}), offer})
        await createSignalHandler(ctx)(0)(await ctx.selfTopicP, text,
            (topic, message) => published.push({topic, text: message}))
        await until(() => published.length === 2 || errors.length > 0, `the answer to ${peerId}`)
        if (errors.length > 0) throw new Error(`the room reported ${errors[0]}`)
        out.answerer.cases.push({offer: text, published})
    }
    crypto.getRandomValues = getRandomValues
}

// JSON payloads, as Trystero sends one: the value an action is given, written by toJson
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
