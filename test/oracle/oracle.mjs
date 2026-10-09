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
import {defaultRelayUrls} from '@trystero-p2p/nostr'

if (process.argv.length !== 4) {
    console.error('usage: node oracle.mjs <cases> <output-directory>')
    process.exit(2)
}
const [casesDirectory, outputDirectory] = process.argv.slice(2)
const cases = JSON.parse(readFileSync(join(casesDirectory, 'protocol.json'), 'utf8'))

const core = join(dirname(fileURLToPath(import.meta.url)), 'node_modules/@trystero-p2p/core/dist')
const {sha1, hashWith, deriveRoomNamespace, encrypt, genKey} = await import(join(core, 'crypto.mjs'))
const {strToNum, getRelays, toHex, toJson, topicPath, shuffle} = await import(join(core, 'utils.mjs'))
const {createActionWireManager} = await import(join(core, 'action-wire.mjs'))
const {SharedPeerManager} = await import(join(core, 'shared-peer.mjs'))

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
const getRandomValues = crypto.getRandomValues.bind(crypto)
crypto.getRandomValues = array => array.map((_, i) => (i * sealed.random.multiplier + sealed.random.offset) % 256)
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

mkdirSync(outputDirectory, {recursive: true})
const target = join(outputDirectory, 'expected.json')
writeFileSync(target, JSON.stringify(out, null, 4) + '\n')
console.log('wrote ' + target)
process.exit(0)
