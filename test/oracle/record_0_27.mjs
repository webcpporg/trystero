// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Records webcpp::trystero's test vectors, for both phases of the port, from Trystero 0.27.0's own
// code, run in Node (>= 23.6, which strips TypeScript's types). Usage:
//   node record.mjs <trystero-0.27.0 checkout> [<previous output>] > trystero-0.27.0.json
//   node record.mjs --dist <node_modules> [<previous output>] > trystero-0.27.0.json
// The first form imports packages/core/src/*.ts and packages/nostr/src/protocol.ts of the pinned
// checkout unchanged; extensionless relative imports are resolved to .ts by the hook below. The
// second imports the same modules from the published packages' dist/*.mjs under <node_modules>
// (@trystero-p2p/core and @trystero-p2p/nostr 0.27.0), so that the oracle lane proves the packages
// a JavaScript peer runs equal the pinned source: both forms write the same bytes.
// Randomness is pinned only around the calls whose output it decides (the GCM IV, genId's bytes, a
// request's id, the handshake's UUID nonces), by replacing crypto.getRandomValues or
// crypto.randomUUID for that call and restoring it after. WebCrypto's ECDSA signatures are random:
// with <previous output>, each signature recorded there is kept when 0.27.0's own verification
// still accepts it, and only a missing one is signed anew, so that a run over the previous output
// writes it again byte for byte.
import {registerHooks} from 'node:module'
import {pathToFileURL} from 'node:url'
import path from 'node:path'
import fs from 'node:fs'
import {createECDH, createHash} from 'node:crypto'

const distMode = process.argv[2] === '--dist'
const argv = distMode ? process.argv.slice(3) : process.argv.slice(2)
const rootUrl = pathToFileURL(path.resolve(argv[0] ?? '.') + '/')
const corePrimitives = distMode
  ? new URL('@trystero-p2p/core/dist/primitives.mjs', rootUrl).href
  : new URL('packages/core/src/primitives.ts', rootUrl).href

registerHooks({
  resolve(specifier, context, next) {
    // nostr's protocol imports two packages: Trystero's core, whose encodeBytes and toJson
    // are primitives's, and the Schnorr signer, which topicToKind does not use.
    if (specifier === '@trystero-p2p/core') {
      return next(corePrimitives, context)
    }
    if (specifier === '@noble/secp256k1') {
      return {url: 'data:text/javascript,export const schnorr = {}', shortCircuit: true}
    }
    if (!distMode && specifier.startsWith('.') && !path.extname(specifier)) {
      const parent = new URL(context.parentURL)
      const candidate = new URL(specifier + '.ts', parent)
      if (fs.existsSync(candidate)) {
        return next(candidate.href, context)
      }
    }
    return next(specifier, context)
  }
})

const root = path.resolve(argv[0] ?? '.')
// A source path of the checkout, or the same module of the published packages in --dist mode.
const src = p => {
  if (!distMode) return pathToFileURL(path.join(root, p)).href
  const [, pkg, file] = /^packages\/(core|nostr)\/src\/(.+)\.ts$/.exec(p)
  return pathToFileURL(path.join(root, '@trystero-p2p', pkg, 'dist', file + '.mjs')).href
}
const previous = argv[1] ? JSON.parse(fs.readFileSync(argv[1], 'utf8')) : null
const prim = await import(src('packages/core/src/primitives.ts'))
const cry = await import(src('packages/core/src/crypto.ts'))
const utils = await import(src('packages/core/src/utils.ts'))

const nostr = await import(src('packages/nostr/src/protocol.ts'))
const pkg = JSON.parse(fs.readFileSync(path.join(root, distMode ? '@trystero-p2p/core/package.json' : 'packages/core/package.json'), 'utf8'))
const hex = b => Buffer.from(b).toString('hex')
// The published dist inlines primitives.ts's id lengths, which it then does not export.
const peerIdLength = prim.peerIdLength ?? 20
const userIdLength = prim.userIdLength ?? 22
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
const tagVersions = {'room-aes': 3, 'room-hmac': 3, 'pbkdf2-master': 3, 'ov-auth': 3, 'e2e-salt': 2,
  'e2e-info': 2, pid: 1, uid: 1, 'user-proof': 3, 'id-hs': 2, 'identity-self-test': 1}
out.domain_tags = Object.entries(tagVersions).map(([n, version]) => ({name: n, version,
  tag: prim.domainTag(n, version)}))

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
    peer_id: enc(pd, peerIdLength), user_id: enc(ud, userIdLength)})
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

// ---------------------------------------------------------------------------------------------
// Phase 2: identity, signatures and the handshake (identity.ts, crypto.ts, handshake.ts).
// Every key is fixed: its secret scalar is SHA-256 of a label, its point computed by node:crypto.
// A signature is WebCrypto's, made by 0.27.0's ecdsaSign (low S) and kept across runs by `signed`.
const ident = await import(src('packages/core/src/identity.ts'))
const hs = await import(src('packages/core/src/handshake.ts'))
const b64 = prim.toBase64Url
const p256N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551n
const bigHex = v => v.toString(16).padStart(64, '0')

const fixedKey = label => {
  const d = createHash('sha256').update(`webcpp-trystero-vector:${label}`).digest()
  const ecdh = createECDH('prime256v1')
  ecdh.setPrivateKey(d)
  const point = new Uint8Array(ecdh.getPublicKey(null, 'uncompressed'))
  const x = b64(point.subarray(1, 33))
  const y = b64(point.subarray(33))
  return {label, d: b64(new Uint8Array(d)), x, y, point, compact: `${x}.${y}`,
    jwk: {kty: 'EC', crv: 'P-256', d: b64(new Uint8Array(d)), x, y}}
}
const signingKeyOf = async k => crypto.subtle.importKey('jwk', {...k.jwk, ext: true, key_ops: ['sign']},
  cry.ecdsaKeyAlgo, false, ['sign'])
const verifyKeyOf = async k => crypto.subtle.importKey('raw', k.point, cry.ecdsaKeyAlgo, false, ['verify'])

// Signatures of the previous output, by label, and those this run records.
const previousSignatures = previous?.signatures ?? {}
out.signatures = {}
const signed = async (label, key, payload) => {
  const kept = previousSignatures[label]
  if (kept && await cry.ecdsaVerify(await verifyKeyOf(key), prim.fromBase64Url(kept), payload)) {
    out.signatures[label] = kept
    return prim.fromBase64Url(kept)
  }
  const sig = await cry.ecdsaSign(await signingKeyOf(key), payload)
  out.signatures[label] = b64(sig)
  return sig
}

const keys = Object.fromEntries(['endpoint-a', 'user-a', 'endpoint-b', 'user-b', 'user-export']
  .map(l => [l, fixedKey(l)]))

// The ids of each fixed key, through 0.27.0's getVerifiedPubKeyRecord (its peerId and userId).
out.p256_keys = []
for (const k of Object.values(keys)) {
  const rec = await ident.getVerifiedPubKeyRecord(k.compact)
  out.p256_keys.push({label: k.label, d: k.d, uncompressed: hex(k.point), compact: k.compact,
    peer_id: rec.peerId, user_id: rec.userId})
}

// Compact keys 0.27.0 refuses (pubJwkToUncompressedBytes) and one it takes; a point off the curve
// passes the text check and fails only at the key's import (getVerifiedPubKeyRecord gives null).
const goodCompact = keys['user-a'].compact
const offCurve = `${goodCompact.slice(0, 44)}${b64(new Uint8Array(32).fill(1))}`
out.compact_keys = []
for (const text of [goodCompact, goodCompact.slice(0, 86), goodCompact + 'A', goodCompact.replace('.', '_'),
  `${goodCompact.slice(0, 42)}=.${goodCompact.slice(44)}`, offCurve]) {
  out.compact_keys.push({text, text_valid: ident.pubJwkToUncompressedBytes(text) !== null,
    record: (await ident.getVerifiedPubKeyRecord(text)) !== null})
}

// The exported user identity: `trystero_uk_` + base64url(JSON {d, x, y}), and what import takes.
const exportKey = keys['user-export']
const imported = await ident.importUserIdentity(exportKey.jwk)
const userKeyText = ident.exportUserIdentity(imported)
out.user_identity = {label: exportKey.label, jwk: {d: exportKey.d, x: exportKey.x, y: exportKey.y},
  user_id: imported.userId, text: userKeyText,
  json: prim.toJson({d: exportKey.d, x: exportKey.x, y: exportKey.y})}
const wrap = obj => `trystero_uk_${b64(prim.encodeBytes(prim.toJson(obj)))}`
const otherKey = keys['user-a']
const nBytes = prim.fromBase64Url(b64(Uint8Array.from(Buffer.from(bigHex(p256N), 'hex'))))
const importCases = [
  ['the exported text', userKeyText],
  ['the text with spaces around it', `  ${userKeyText}\n`],
  ['the JSON text', prim.toJson({kty: 'EC', crv: 'P-256', d: exportKey.d, x: exportKey.x, y: exportKey.y})],
  ['another prefix', userKeyText.replace('trystero_uk_', 'trystero_pk_')],
  ['the payload padded', userKeyText + '='],
  ['d of another key', wrap({d: otherKey.d, x: exportKey.x, y: exportKey.y})],
  ['d zero', wrap({d: b64(new Uint8Array(32)), x: exportKey.x, y: exportKey.y})],
  ['d the group order', wrap({d: b64(nBytes), x: exportKey.x, y: exportKey.y})],
  ['d of 31 bytes', wrap({d: b64(prim.fromBase64Url(exportKey.d).subarray(1)), x: exportKey.x, y: exportKey.y})],
  ['crv P-384', wrap({crv: 'P-384', d: exportKey.d, x: exportKey.x, y: exportKey.y})],
  ['no y', wrap({d: exportKey.d, x: exportKey.x})],
  ['empty', '']
]
out.user_identity_imports = []
for (const [name, text] of importCases) {
  let userId = null
  try { userId = (await ident.importUserIdentity(text)).userId } catch {}
  out.user_identity_imports.push({case: name, text, accepted: userId !== null, user_id: userId})
}

// The user-proof: 0.27.0 exports its payload, signature and verification (identity.ts) but its
// core never sends one; the port keeps the protocol functions, proven here.
const proofParams = {appId: 'example.com/app', roomId: 'lobby', peerId: out.p256_keys[0].peer_id,
  peerPubJwk: keys['endpoint-a'].compact, seq: 7}
const proofPayload = ident.userEndpointProofPayload(proofParams)
const proofSig = await signed('user_proof', keys['user-a'], proofPayload)
out.user_proof = {app: proofParams.appId, room: proofParams.roomId, peer_id: proofParams.peerId,
  peer_compact: proofParams.peerPubJwk, seq: proofParams.seq, user_id: out.p256_keys[1].user_id,
  user_compact: keys['user-a'].compact, payload: prim.decodeBytes(proofPayload),
  digest: hex(await cry.sha256(prim.decodeBytes(proofPayload))), proof: b64(proofSig),
  verifies: await ident.verifyUserEndpointProof({...proofParams, userId: out.p256_keys[1].user_id,
    userPubJwk: keys['user-a'].compact, proof: b64(proofSig)}),
  other_seq_verifies: await ident.verifyUserEndpointProof({...proofParams, seq: 8,
    userId: out.p256_keys[1].user_id, userPubJwk: keys['user-a'].compact, proof: b64(proofSig)})}

// Low S: 0.27.0 signs low (ecdsaSign normalizes) and verifyWithP256 refuses a high S.
const sAt = sig => prim.bytesToBigInt(sig.subarray(32))
const withS = (sig, s) => { const o = sig.slice(); o.set(Buffer.from(bigHex(s), 'hex'), 32); return o }
const withR = (sig, r) => { const o = sig.slice(); o.set(Buffer.from(bigHex(r), 'hex'), 0); return o }
const lowSig = proofSig
const highSig = withS(lowSig, p256N - sAt(lowSig))
const halfN = p256N >> 1n
out.low_s = []
for (const [name, sig] of [['signed', lowSig], ['its high form', highSig], ['s = (n - 1) / 2', withS(lowSig, halfN)],
  ['s = (n + 1) / 2', withS(lowSig, halfN + 1n)], ['s = n - 1', withS(lowSig, p256N - 1n)],
  ['s = 0', withS(lowSig, 0n)], ['r = 0', withR(lowSig, 0n)], ['r = n', withR(lowSig, p256N)]]) {
  out.low_s.push({case: name, signature: hex(sig), is_low_s: cry.isLowSP256Signature(sig),
    normalized: hex(cry.normalizeLowSP256Signature(sig)),
    verifies: await ident.verifyWithP256(sig, proofPayload, keys['user-a'].compact)})
}

// The handshake's DTLS fingerprints: every `a=fingerprint:` line's value, trimmed, lowercased,
// deduplicated and sorted, as extractDtlsFingerprints reads a description. It is private to
// handshake.ts, so a run of the handshake below proves it: each side sends what it read.
const fpA = 'sha-256 4F:2A:9C:11:00:7B:EE:31:58:AA:0D:6C:93:12:47:BF:C8:21:5E:70:66:D4:3A:91:0B:CF:28:7E:55:19:A3:F0'
const fpB = 'sha-256 0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F9:0A:1B:2C:3D:4E:5F:60:71:82:93:A4:B5:C6:D7:E8:F9'
const fpA2 = 'sha-1 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44'
const sdpA = `v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=fingerprint:${fpA}\r\na=fingerprint:${fpA2}\r\na=fingerprint:${fpA.toLowerCase()}\r\n`
const sdpB = `v=0\r\no=- 3 4 IN IP4 127.0.0.1\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=fingerprint: ${fpB} \r\n`

// One run of createIdentityHandshake between A (the initiator) and B, over queues, each side's
// connection described by the SDPs above; `tamper` may change a message on its way.
const nonces = ['5a1f0c2e-8b3d-4e6f-9a70-b1c2d3e4f506', 'c0ffee00-1234-4abc-8def-0123456789ab']
const identityOf = (endpoint, user, prefix) => ({
  endpoint: {peerId: out.p256_keys.find(k => k.label === endpoint.label).peer_id,
    signPubJwk: {kty: 'EC', crv: 'P-256', x: endpoint.x, y: endpoint.y},
    sign: payload => signed(`${prefix}:endpoint`, endpoint, payload)},
  user: {userId: out.p256_keys.find(k => k.label === user.label).user_id,
    signPubJwk: {kty: 'EC', crv: 'P-256', x: user.x, y: user.y},
    sign: payload => signed(`${prefix}:user`, user, payload)}
})
const runHandshake = async (name, {tamper = (from, step, msg) => msg, sdps = [sdpA, sdpB, sdpB, sdpA]} = {}) => {
  const a = identityOf(keys['endpoint-a'], keys['user-a'], `${name}:a`)
  const b = identityOf(keys['endpoint-b'], keys['user-b'], `${name}:b`)
  const app = 'example.com/app'
  const room = 'lobby'
  const queues = {A: [], B: []}
  const waiters = {A: [], B: []}
  const messages = []
  const deliver = (to, payload) => {
    const w = waiters[to].shift()
    if (w) w(payload)
    else queues[to].push(payload)
  }
  const sender = (from, to) => async data => {
    const step = data[`__trystero_id`]
    const sent = tamper(from, step, structuredClone(data))
    messages.push({from, step, json: prim.toJson(sent)})
    deliver(to, {data: sent})
  }
  const receiver = side => () => new Promise(resolve => {
    const queued = queues[side].shift()
    if (queued) resolve(queued)
    else waiters[side].push(resolve)
  })
  const peerHandle = (local, remote) => ({connection: {localDescription: {sdp: local}, remoteDescription: {sdp: remote}}})
  const verified = {}
  const make = (side, id) => hs.createIdentityHandshake({endpointIdentityP: Promise.resolve(id.endpoint), appId: app,
    roomId: room, getUserIdentity: () => Promise.resolve(id.user),
    onVerified: (peerId, v) => { verified[side] = {peer_id: peerId, user_id: v.userId} }})
  const uuid = crypto.randomUUID
  let at = 0
  crypto.randomUUID = () => nonces[at++]
  const results = {}
  // A side that fails sends nothing more: the other one waits (until room.ts's 10 s timeout),
  // recorded as 'waits'; the run unblocks it with a message it refuses.
  let firstFailure = null
  const fail = (side, other, e) => {
    results[side] = firstFailure && firstFailure !== side ? 'waits' : e.message
    if (!firstFailure) {
      firstFailure = side
      deliver(other, {data: null})
    }
  }
  try {
    await Promise.all([
      make('A', a).run(b.endpoint.peerId, sender('A', 'B'), receiver('A'), true, peerHandle(sdps[0], sdps[1]))
        .then(() => { results.A = null }, e => fail('A', 'B', e)),
      make('B', b).run(a.endpoint.peerId, sender('B', 'A'), receiver('B'), false, peerHandle(sdps[2], sdps[3]))
        .then(() => { results.B = null }, e => fail('B', 'A', e))
    ])
  } finally {
    crypto.randomUUID = uuid
  }
  return {case: name, app, room, initiator: {peer_id: a.endpoint.peerId, user_id: a.user.userId,
    endpoint: keys['endpoint-a'].label, user: keys['user-a'].label, sdp_local: sdps[0], sdp_remote: sdps[1]},
  responder: {peer_id: b.endpoint.peerId, user_id: b.user.userId, endpoint: keys['endpoint-b'].label,
    user: keys['user-b'].label, sdp_local: sdps[2], sdp_remote: sdps[3]},
  nonces, messages, initiator_error: results.A, responder_error: results.B, verified}
}

out.handshake = []
const ok = await runHandshake('verified')
// The transcript both sides sign: toJson([domainTag('id-hs', 2), app, room, init peer, resp peer,
// init nonce, resp nonce, init dtls, resp dtls, init user, resp user]), as encodeTranscript writes it.
const step1 = JSON.parse(ok.messages[0].json)
const step2 = JSON.parse(ok.messages[1].json)
const transcript = prim.toJson([prim.domainTag('id-hs', 2), ok.app, ok.room, ok.initiator.peer_id,
  ok.responder.peer_id, step1.n, step2.n, step1.dtls, step2.dtls, ok.initiator.user_id, ok.responder.user_id])
ok.transcript = transcript
ok.transcript_digest = hex(await cry.sha256(transcript))
ok.transcript_verified = {
  step2_endpoint: await ident.verifyWithP256(prim.fromBase64Url(step2.ps), prim.encodeBytes(transcript), keys['endpoint-b'].compact),
  step2_user: await ident.verifyWithP256(prim.fromBase64Url(step2.us), prim.encodeBytes(transcript), keys['user-b'].compact)}
out.handshake.push(ok)
const swapStep = (side, step, edit) => (from, s, msg) => (from === side && s === step ? edit(msg) : msg)
out.handshake.push(await runHandshake('another remote fingerprint', {sdps: [sdpA, sdpB, sdpB, sdpA.replace(/4F:2A/g, '4F:2B')]}))
out.handshake.push(await runHandshake('a user id of another key', {tamper: swapStep('A', 1, m => ({...m, uid: out.p256_keys[3].user_id}))}))
out.handshake.push(await runHandshake('an endpoint key of another peer id', {tamper: swapStep('B', 2, m => ({...m, pk: keys['endpoint-a'].compact}))}))
out.handshake.push(await runHandshake('a short nonce', {tamper: swapStep('A', 1, m => ({...m, n: '0123456789abcde'}))}))
out.handshake.push(await runHandshake('a user signature of another key', {tamper: swapStep('B', 2, m => ({...m, us: m.ps}))}))
out.handshake.push(await runHandshake('a high-S endpoint signature', {tamper: swapStep('A', 3, m => {
  const sig = prim.fromBase64Url(m.ps)
  return {...m, ps: b64(withS(sig, p256N - sAt(sig)))}
})}))
out.handshake.push(await runHandshake('no fingerprint in the remote description', {sdps: [sdpA, 'v=0\r\n', sdpB, sdpA]}))

// The handshake's frames, as room.ts sends them through its internal actions `@_hsdata` (a step,
// JSON) and `@_hsready` ('' when admitted, {ok: false, err} when refused), between two managers.
out.handshake_frames = []
for (const [name, action, data] of [['step 1', 'hsdata', JSON.parse(ok.messages[0].json)],
  ['ready', 'hsready', ''], ['room full', 'hsready', {ok: false, err: 'room full'}],
  ['banned', 'hsready', {ok: false, err: 'peer banned', reason: 'spam'}]]) {
  const pair = wirePair(d => actionsMod.createActionManager(d), false)
  const sending = pair.managers.A.makeInternalAction(action)
  const receiving = pair.managers.B.makeInternalAction(action)
  let received = null
  receiving.onMessage(value => { received = value })
  await sending.send(data, {target: 'B'})
  const frames = await pair.frames()
  out.handshake_frames.push({case: name, action: `@_${action}`, payload_json: prim.toJson(data), frames,
    delivered: received !== null && prim.toJson(received) === prim.toJson(data)})
}

// A ping: room.ts's internal request action `@_ping`, a request of '' answered '' through
// `@_response` (0.26.0's `@_pong` is gone), its id pinned as a request's is above.
{
  const pair = wirePair(d => actionsMod.createActionManager(d), false)
  const asker = pair.managers.A.makeInternalRequestAction('ping')
  const answerer = pair.managers.B.makeInternalRequestAction('ping')
  answerer.onRequest(() => '')
  const uuid = crypto.randomUUID
  crypto.randomUUID = () => requestId
  let response = null
  try {
    response = await asker.request('', {target: 'B'})
  } finally {
    crypto.randomUUID = uuid
  }
  out.ping_frames = {action: '@_ping', request_id: requestId, response, frames: await pair.frames()}
}

// The bounds of 0.27.0's identity caches and handshake: constants identity.ts, handshake.ts and
// room.ts do not export, copied from them (identity.ts's maxSig*, createBoundedCache's 4096 default,
// handshake.ts's defaultHandshakeTimeoutMs, maxBufferedHandshakeMessages and n.length < 16, and the
// maxPayloadBytes of room.ts's hsdata and hsready actions).
out.identity_limits = {pub_key_records: 4096, signature_cache_entries: 8192,
  signature_cache_bytes: 2 * 1024 * 1024, signature_cache_payload_bytes: 4096,
  handshake_timeout_ms: 10000, buffered_handshake_messages: 16, handshake_nonce_min_chars: 16,
  hsdata_max_payload_bytes: 64 * 1024, hsready_max_payload_bytes: 128 * 1024}

// Signatures in the order of their labels: concurrent signing finishes in any order.
out.signatures = Object.fromEntries(Object.entries(out.signatures).sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0)))
process.stdout.write(JSON.stringify(out, null, 1) + '\n')
