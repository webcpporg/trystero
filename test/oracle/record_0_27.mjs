// Records webcpp::trystero's Phase 1 test vectors from Trystero 0.27.0's own code, run in Node
// (>= 23.6, which strips TypeScript's types). Usage:
//   node record.mjs <trystero-0.27.0 checkout> > phase1.json
// It imports packages/core/src/{primitives,crypto,utils,action-wire,actions}.ts and
// packages/nostr/src/protocol.ts unchanged; extensionless relative imports are resolved to .ts by
// the hook below. Randomness is pinned only around the calls whose output it decides (the GCM IV,
// genId's bytes, a request's id), by replacing crypto.getRandomValues or crypto.randomUUID for that
// call and restoring it after.
import {registerHooks} from 'node:module'
import {pathToFileURL} from 'node:url'
import path from 'node:path'
import fs from 'node:fs'

const rootUrl = pathToFileURL(path.resolve(process.argv[2] ?? '.') + '/')

registerHooks({
  resolve(specifier, context, next) {
    // nostr/src/protocol.ts imports two packages: Trystero's core, whose encodeBytes and toJson
    // are primitives.ts's, and the Schnorr signer, which topicToKind does not use.
    if (specifier === '@trystero-p2p/core') {
      return next(new URL('packages/core/src/primitives.ts', rootUrl).href, context)
    }
    if (specifier === '@noble/secp256k1') {
      return {url: 'data:text/javascript,export const schnorr = {}', shortCircuit: true}
    }
    if (specifier.startsWith('.') && !path.extname(specifier)) {
      const parent = new URL(context.parentURL)
      const candidate = new URL(specifier + '.ts', parent)
      if (fs.existsSync(candidate)) {
        return next(candidate.href, context)
      }
    }
    return next(specifier, context)
  }
})

const root = path.resolve(process.argv[2] ?? '.')
const src = p => pathToFileURL(path.join(root, p)).href
const prim = await import(src('packages/core/src/primitives.ts'))
const cry = await import(src('packages/core/src/crypto.ts'))
const utils = await import(src('packages/core/src/utils.ts'))

const nostr = await import(src('packages/nostr/src/protocol.ts'))
const pkg = JSON.parse(fs.readFileSync(path.join(root, 'packages/core/package.json'), 'utf8'))
const hex = b => Buffer.from(b).toString('hex')
const fromHex = h => new Uint8Array(Buffer.from(h, 'hex'))

const withRandom = async (bytes, f) => {
  const original = crypto.getRandomValues.bind(crypto)
  let at = 0
  crypto.getRandomValues = view => {
    for (let i = 0; i < view.length; i++) view[i] = bytes[(at++) % bytes.length]
    return view
  }
  try {
    return await f()
  } finally {
    crypto.getRandomValues = original
  }
}

const out = {trystero: pkg.version, recorded_with: `node ${process.version}`}

// Domain tags and topic texts.
out.domain_tags = ['room-aes', 'room-hmac', 'pbkdf2-master', 'ov-auth', 'e2e-salt', 'e2e-info',
  'pid', 'uid', 'user-proof'].map(n => ({name: n, version: n.startsWith('e2e') ? 2 : n === 'pid' || n === 'uid' ? 1 : 3,
  tag: prim.domainTag(n, n.startsWith('e2e') ? 2 : n === 'pid' || n === 'uid' ? 1 : 3)}))

const texts = ['', 'lobby', 'Trystero', '["Trystero","app","room"]', 'café \u{1F600}',
  'x'.repeat(200)]
out.hash_topic = []
for (const t of texts) out.hash_topic.push({text: t, topic: await cry.hashTopic(t)})

out.topic_path = [
  {parts: ['Trystero', 'example.com/app', 'lobby']},
  {parts: ['Trystero', 'a"b\\c', 'café']},
  {parts: ['["Trystero","app","room"]', 'peer0123456789abcdef']}
].map(c => ({...c, path: utils.topicPath(...c.parts)}))

// A room's two topics, as strategy.ts derives them: the root from topicPath(libName, app, room),
// a peer's from topicPath(<that path>, peerId), each through hashTopic.
out.room_topics = []
for (const [a, r, p] of [['example.com/app', 'lobby', 'T5sdTwb84yyey80e4qjv'],
  ['webcpp-trystero-quick-start', 'caf\u00e9', 'AbCdEfGhIjKlMnOpQrSt']]) {
  const rootText = utils.topicPath(prim.libName, a, r)
  const peerText = utils.topicPath(rootText, p)
  out.room_topics.push({app: a, room: r, peer_id: p, root_text: rootText,
    root_topic: await cry.hashTopic(rootText), peer_text: peerText,
    peer_topic: await cry.hashTopic(peerText), root_kind: nostr.topicToKind(await cry.hashTopic(rootText))})
}

// Base64url, strict.
out.base64url = [[], [0], [0xff, 0xfe], [1, 2, 3], [0xfb, 0xff, 0xbf], [...Array(33).keys()]]
  .map(b => ({bytes: hex(b), text: prim.toBase64Url(new Uint8Array(b))}))

// hash32, strToNum, shuffle, Nostr kinds.
out.hash32 = texts.map(t => ({text: t, value: prim.hash32(t)}))
out.str_to_num = texts.map(t => ({text: t, limit: 10000, value: utils.strToNum(t, 10000)}))
const relays = Array.from({length: 30}, (_, i) => `wss://relay${i}.example`)
out.shuffle = ['example.com/app', 'webcpp-trystero-quick-start', '', 'café'].map(app => ({
  app, seed: utils.strToNum(app), order: utils.shuffle(relays.map((_, i) => i), utils.strToNum(app))
}))
out.topic_to_kind = texts.map(t => ({topic: t, kind: nostr.topicToKind(t)}))

// Base62 ids: genId over pinned bytes (bytes >= 248 rejected).
const idBytes = [0, 1, 61, 62, 123, 247, 248, 249, 255, 10, 200, 100, 33, 34, 35, 36, 37, 38, 39, 40]
out.gen_id = [8, 12, 20, 22, 64].map(n => ({n, random_cycle: hex(idBytes),
  id: null}))
for (const c of out.gen_id) c.id = await withRandom(idBytes, () => utils.genId(c.n))

// encodeBase62 of a SHA-256 digest and the pid/uid derivation from a compact public key.
const compactPubs = ['WKn-ZIGevcwGIyyrzFoZNBdQq79v3jZ8Nl3S1Jvxm7I.HdKAMKoHOpFkK-nMlm9JN6qzGlcrJ8nd5I4L8ss1V40']
out.identity_ids = []
for (const cp of compactPubs) {
  const pd = await cry.sha256(`${prim.domainTag('pid', 1)}:${cp}`)
  const ud = await cry.sha256(`${prim.domainTag('uid', 1)}:${cp}`)
  const enc = (bytes, len) => {
    const m = 62n ** BigInt(len)
    let v = prim.bytesToBigInt(bytes) % m
    let s = ''
    for (let i = 0; i < len; i++) { s = prim.base62Alphabet[Number(v % 62n)] + s; v /= 62n }
    return s
  }
  out.identity_ids.push({compact_pub: cp, pid_digest: hex(pd), uid_digest: hex(ud),
    peer_id: enc(pd, prim.peerIdLength), user_id: enc(ud, prim.userIdLength)})
}

// Room keys: exported by wrapping the derivation's CryptoKeys in raw exports where WebCrypto allows.
const exportRaw = async k => hex(new Uint8Array(await crypto.subtle.exportKey('raw', k)))
const rooms = [['example.com/app', 'lobby'], ['webcpp-trystero-quick-start', 'café']]
out.room_key_no_password = []
for (const [a, r] of rooms) {
  const raw = await cry.sha256(prim.toJson([prim.domainTag('room-aes', 3), a, r]))
  out.room_key_no_password.push({app: a, room: r,
    preimage: prim.toJson([prim.domainTag('room-aes', 3), a, r]), key: hex(raw)})
}

// With a password the keys are not extractable; the vectors recompute them with WebCrypto
// step by step, as crypto.ts derives them, and prove the result against crypto.ts by sealing.
const passwords = ['hunter2', 'päss wörd']
out.room_keys_password = []
for (const [a, r] of rooms) for (const pw of passwords) {
  const salt = await cry.sha256(prim.toJson([prim.domainTag('pbkdf2-master', 3), a, r]))
  const base = await crypto.subtle.importKey('raw', prim.encodeBytes(pw), 'PBKDF2', false, ['deriveBits'])
  const master = new Uint8Array(await crypto.subtle.deriveBits(
    {name: 'PBKDF2', salt, iterations: 600_000, hash: 'SHA-256'}, base, 256))
  const hk = await crypto.subtle.importKey('raw', master, 'HKDF', false, ['deriveBits'])
  const derive = async info => hex(new Uint8Array(await crypto.subtle.deriveBits(
    {name: 'HKDF', hash: 'SHA-256', salt: new Uint8Array(32), info: prim.encodeBytes(info)}, hk, 256)))
  const iv = fromHex('000102030405060708090a0b')
  const sealed = await withRandom(iv, () => cry.encrypt(cry.genKey(pw, a, r), 'probe', 'offer|p|o'))
  out.room_keys_password.push({app: a, room: r, password: pw, salt: hex(salt),
    master: hex(master), aes_key: await derive(prim.domainTag('room-aes', 3)),
    hmac_key: await derive(prim.domainTag('room-hmac', 3)), probe_sealed: sealed})
}

// Room auth tokens (HMAC-SHA-256 under the password's HMAC key).
out.room_auth = []
for (const [a, r] of rooms) {
  const pw = passwords[0]
  for (const [peer, seq] of [['@topology', 0], ['AbCdEfGhIjKlMnOpQrSt', 7]]) {
    const token = await cry.signRoomAuth(pw, a, r, peer, seq)
    out.room_auth.push({app: a, room: r, password: pw, peer_id: peer, seq,
      payload: prim.toJson([prim.domainTag('ov-auth', 3), a, r, peer, seq]), token,
      verifies: await cry.verifyRoomAuth(pw, a, r, peer, seq, token),
      wrong_seq_verifies: await cry.verifyRoomAuth(pw, a, r, peer, seq + 1, token)})
  }
}

// AES-256-GCM sealing: base64url(iv || ciphertext || tag), with and without AAD.
out.seal = []
const ivs = ['000102030405060708090a0b', 'ffffffffffffffffffffffff']
const plains = ['', 'v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n', 'café \u{1F600}']
for (const [a, r] of rooms) for (const pw of ['', passwords[0]]) for (const ivh of ivs)
  for (const p of plains) for (const aad of [undefined, 'offer|AbCdEfGhIjKlMnOpQrSt|0123456789ab']) {
    const sealed = await withRandom(fromHex(ivh), () => cry.encrypt(cry.genKey(pw, a, r), p, aad))
    const opened = await cry.decrypt(cry.genKey(pw, a, r), sealed, aad)
    out.seal.push({app: a, room: r, password: pw, iv: ivh, plaintext: p,
      aad: aad ?? null, sealed, opened_equal: opened === p})
  }

// Texts decrypt refuses: each one must be refused by the port too.
const k0 = cry.genKey('', ...rooms[0])
const good = out.seal.find(s => s.password === '' && s.app === rooms[0][0] && s.aad === null && s.plaintext === 'café \u{1F600}').sealed
const refusals = ['', 'not base64!', good.slice(0, 20), good + '=', good.slice(0, -1) + (good.at(-1) === 'A' ? 'B' : 'A'),
  good.replace(/^./, c => (c === 'A' ? 'B' : 'A'))]
out.open_refused = []
for (const t of refusals) {
  let refused = false
  try { await cry.decrypt(k0, t) } catch { refused = true }
  out.open_refused.push({app: rooms[0][0], room: rooms[0][1], password: '', sealed: t, refused})
}

// The signal AAD.
const sh = await import(src('packages/core/src/signal-handler.ts')).catch(() => null)
out.signal_aad = sh ? [['offer', 'AbCdEfGhIjKlMnOpQrSt', '0123456789ab'], ['answer', 'x', 'y']]
  .map(([t, p, o]) => ({type: t, peer_id: p, offer_id: o, aad: sh.buildSignalAad(t, p, o)})) : null

// The action wire: frames of packages/core/src/action-wire.ts, between two of its managers, A and
// B, whose transports record each frame and hand it to the other manager's handleData a microtask
// later. A transport reports maxBulkFrameBytes only in the wide cases, as a connection whose SCTP
// takes 64 KiB messages does. A frame is recorded as hex, a chunk as its 14-byte header, its size
// and the SHA-256 of the whole frame, which proves it as well as its hex and keeps the fixture
// small; a payload of more than 64 bytes is named by its size, its bytes being (i * 31) mod 256.
const wire = await import(src('packages/core/src/action-wire.ts'))
const actionsMod = await import(src('packages/core/src/actions.ts'))
const patterned = n => Uint8Array.from({length: n}, (_, i) => (i * 31) & 255)
const settle = () => new Promise(resolve => setTimeout(resolve, 0))
const recordFrame = async (from, bytes, transferId, isOffer) => {
  const entry = {from, size: bytes.length}
  if (transferId !== undefined) entry.transfer_id = transferId
  if (isOffer) entry.is_offer = true
  if ((bytes[1] & 15) === 2) {
    entry.head = hex(bytes.subarray(0, 14))
    entry.sha256 = hex(await cry.sha256(bytes))
  } else {
    entry.hex = hex(bytes)
  }
  return entry
}

// Two peers whose managers (made by `make`, given its dependencies) exchange frames.
const wirePair = (make, wide, onFrame = () => {}) => {
  const frames = []
  const pending = []
  const managers = {}
  const transport = (from, to) => ({
    isOpen: true,
    bufferedAmount: 0,
    ...(wide ? {maxBulkFrameBytes: 64 * 1024} : {}),
    sendFrame: (bytes, transferId, isOffer) => {
      const copy = bytes.slice()
      const at = frames.push(null) - 1
      pending.push(recordFrame(from, copy, transferId, isOffer).then(e => { frames[at] = e }))
      onFrame(from, copy)
      queueMicrotask(() => managers[to].handleData(from, copy.buffer))
    },
    waitForDrain: () => true
  })
  const deps = (self, other) => ({
    resolveTransport: id => (id === other ? transport(self, other) : undefined),
    getTransport: id => (id === other ? transport(self, other) : undefined),
    getPeerIds: () => [other],
    canReceiveFromPeer: () => true,
    onPeerError: (id, e) => { throw new Error(`${self} dropped ${id}: ${e.message}`) }
  })
  managers.A = make(deps('A', 'B'))
  managers.B = make(deps('B', 'A'))
  return {managers, frames: async () => { await settle(); await Promise.all(pending); return frames }}
}

const payloadOf = (format, data) => format === 2
  ? (data.length > 64 ? {payload_size: data.length} : {payload_hex: hex(data)})
  : {payload_text: format === 0 ? data : prim.toJson(data)}

out.action_frames = []
const recordSend = async (name, action, data, format, {metadata, wide = false, gate, abortAfterChunk = false} = {}) => {
  const controller = new AbortController()
  const pair = wirePair(d => wire.createActionWireManager(d), wide, (from, bytes) => {
    if (abortAfterChunk && from === 'A' && (bytes[1] & 15) === 2) controller.abort()
  })
  const a = pair.managers.A.makeWireAction(action)
  const b = pair.managers.B.makeWireAction(action)
  let received = null
  b.onMessage((value, peer, meta) => { received = {value, peer, meta} })
  if (gate) b.onReceive(gate)
  let error = null
  await a.send(data, 'B', metadata, undefined, controller.signal).catch(e => { error = e.message })
  const frames = await pair.frames()
  out.action_frames.push({case: name, action, format, metadata: metadata === undefined ? null : prim.toJson(metadata),
    ...payloadOf(format, data), wide, frames, delivered: received !== null, send_error: error})
}

await recordSend('text', 'chat', 'hello, café', 0)
await recordSend('json', 'json', {n: 1, list: [true, null, 'x']}, 1)
await recordSend('binary', 'bin', Uint8Array.from({length: 10}, (_, i) => i), 2)
await recordSend('metadata', 'chat', 'hi', 0, {metadata: {a: 1}})
await recordSend('bulk', 'file', patterned(200 * 1024), 2, {metadata: {name: 'big.bin'}})
await recordSend('bulk_wide', 'file', patterned(200 * 1024), 2, {metadata: {name: 'big.bin'}, wide: true})
await recordSend('declined', 'file', patterned(20000), 2, {gate: () => false})
await recordSend('cancelled', 'file', patterned(200 * 1024), 2, {abortAfterChunk: true})

// A request and its response, through actions.ts's request action: the request's metadata names
// its id (crypto.randomUUID, pinned here), the response travels as the internal action
// "@_response" with that id, and a failure as a JSON null with the error's text.
const requestId = '0b1c2d3e-4f50-4617-8899-aabbccddeeff'
for (const [name, fails] of [['request', false], ['request_failed', true]]) {
  const pair = wirePair(d => actionsMod.createActionManager(d), false)
  const asker = pair.managers.A.makeAction('ask', {kind: 'request'})
  const answerer = pair.managers.B.makeAction('ask', {kind: 'request'})
  answerer.onRequest(async data => {
    if (fails) throw new Error('no answer')
    return `pong:${data}`
  })
  const uuid = crypto.randomUUID
  crypto.randomUUID = () => requestId
  let response = null
  let error = null
  try {
    response = await asker.request('ping', {target: 'B', metadata: {x: 1}})
  } catch (e) {
    error = e.message
  } finally {
    crypto.randomUUID = uuid
  }
  out.action_frames.push({case: name, action: 'ask', format: 0, metadata: prim.toJson({r: requestId, m: {x: 1}}),
    payload_text: 'ping', wide: false, frames: await pair.frames(), response, send_error: error,
    answer: {action: '@_response', format: fails ? 1 : 0,
      metadata: prim.toJson(fails ? {r: requestId, e: 'no answer'} : {r: requestId}),
      payload_text: fails ? 'null' : 'pong:ping'}})
}

process.stdout.write(JSON.stringify(out, null, 1) + '\n')
