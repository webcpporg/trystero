// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The interop test: C++ peers (peer_room.hpp), built with Emscripten (peer.cpp) or native (native_peer.cpp), and
// Trystero 0.27.0 peers (js.html, forger.html) in the same rooms, in headless Chrome, over a local Nostr relay
// (relay.mjs) or, with --public, Trystero's public relays.
//
//   node interop/run.mjs [--public] [--browser <interop_browser.js>] [--native <native_peer>] [--chrome <path>] [--verbose]
// Add --scenario <name fragment> to reproduce one scenario; no match fails the run.
//
// With --browser, the browser's C++ peer meets Trystero JS; with --native, the native C++ peer (a process) does.
// b2's driven tests interop_browser and interop_native (../Jamfile) pass the program they linked, after their
// argument: the browser peer's JavaScript, whose directory, which holds its wasm too, is served, or the native peer's
// executable.
//
// Every peer gathers its ICE candidates on this machine alone: the C++ peers get the STUN server stun:127.0.0.1:9,
// where nothing answers, in place of Trystero's public ones, the native peer binds its connections to 127.0.0.1, and
// the Trystero JS peers have no ICE server at all.
// Chrome is --chrome, the CHROME environment variable, or, on macOS, the newest chrome-headless-shell in Playwright's
// cache; elsewhere CHROME is required. Whichever it is, a full Chrome or the headless shell, it runs headless, and
// makes no request of its own beyond the machine.
//
// Chrome, its profile, the relay, the server and the native peers are gone when the run ends, however it ends: the
// scenarios' verdict, a failure outside every scenario, or SIGTERM, SIGINT or SIGHUP (the bound of b2's driven test
// sends SIGTERM). A SIGKILL leaves the profile, which the next run removes.
import {spawn} from 'node:child_process'
import {createECDH, createHash} from 'node:crypto'
import {createInterface} from 'node:readline'
import {existsSync, mkdtempSync, readFileSync, readdirSync, rmSync} from 'node:fs'
import {createServer} from 'node:http'
import {homedir, tmpdir} from 'node:os'
import {basename, dirname, extname, join, normalize, resolve} from 'node:path'
import {fileURLToPath, pathToFileURL} from 'node:url'
import {startRelay} from './relay.mjs'

const here = dirname(fileURLToPath(import.meta.url))
const args = process.argv.slice(2)
const option = name => (args.includes(name) ? args[args.indexOf(name) + 1] : undefined)
const usePublic = args.includes('--public')
const verbose = args.includes('--verbose')
const scenarioFilter = option('--scenario')
const browserPeer = option('--browser')
const nativePeer = option('--native')
if (!browserPeer && !nativePeer) throw new Error('pass --browser <interop_browser.js> or --native <native_peer>')
const buildDir = browserPeer && dirname(resolve(browserPeer))

// Every wait of the driver is for something that must happen: its bound only ends a run that hangs, naming what did not
// happen, so it is far above the time anything takes on the most loaded machine. No scenario waits for something not
// to happen: what must not happen is checked once what must happen has, and again at the scenario's end.
const bound = 240_000

const findChrome = () => {
    const explicit = option('--chrome') ?? process.env.CHROME
    if (explicit) return explicit
    if (process.platform !== 'darwin') {
        throw new Error('CHROME is not set: off macOS, the interop test runs the Chrome that CHROME or --chrome names')
    }
    const cache = join(homedir(), 'Library/Caches/ms-playwright')
    const shells = existsSync(cache) ? readdirSync(cache).filter(d => d.startsWith('chromium_headless_shell-')).sort() : []
    for (const dir of shells.reverse()) {
        for (const sub of ['chrome-headless-shell-mac-arm64', 'chrome-headless-shell-mac-x64']) {
            const path = join(cache, dir, sub, 'chrome-headless-shell')
            if (existsSync(path)) return path
        }
    }
    throw new Error(`no Chrome found: set CHROME, pass --chrome, or install Playwright's chrome-headless-shell in ${cache}`)
}

// ---- what a run leaves behind, removed however it ends ------------------------------------------------------------
const left = {chrome: null, profile: null, server: null, relay: null, natives: new Set()}
const exited = child => child.exitCode !== null || child.signalCode !== null
const exit = child => new Promise(resolve => (exited(child) ? resolve() : child.once('exit', resolve)))
let cleaning = null
// The profile goes only once Chrome has exited: Chrome writes into it as it shuts down, and removing it meanwhile fails
// with ENOTEMPTY (measured: 40 of 40 runs of the headless shell killed right after a page opened).
const cleanUp = () => (cleaning ??= (async () => {
    for (const child of left.natives) child.kill()
    left.relay?.close()
    left.server?.close()
    if (left.chrome && !exited(left.chrome)) {
        const stubborn = setTimeout(() => left.chrome.kill('SIGKILL'), 5_000)
        left.chrome.kill()
        await exit(left.chrome)
        clearTimeout(stubborn)
    }
    if (left.profile) rmSync(left.profile, {recursive: true, force: true, maxRetries: 3})
})())
for (const [signal, number] of [['SIGHUP', 1], ['SIGINT', 2], ['SIGTERM', 15]]) {
    process.on(signal, () => cleanUp().finally(() => process.exit(128 + number)))
}

// A profile is named after its run's process id: one whose run is no longer alive, killed with SIGKILL, goes now.
const profiles = join(tmpdir(), 'trystero-interop-')
const alive = pid => {
    try {
        process.kill(pid, 0)
        return true
    } catch (error) {
        return error.code === 'EPERM'
    }
}
for (const name of readdirSync(tmpdir())) {
    const pid = Number(/^trystero-interop-([0-9]+)-/.exec(name)?.[1])
    if (pid && !alive(pid)) rmSync(join(tmpdir(), name), {recursive: true, force: true, maxRetries: 3})
}

// ---- static files: the pages, the wasm build, and Trystero from test/oracle/node_modules ---------------------------
const types = {'.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json'}
const roots = {...(buildDir ? {'/build/': buildDir} : {}), '/node_modules/': join(here, '../node_modules'), '/': here}
const server = createServer((req, res) => {
    const path = decodeURIComponent(new URL(req.url, 'http://x').pathname)
    const [prefix, root] = Object.entries(roots).find(([p]) => path.startsWith(p))
    const file = normalize(join(root, path.slice(prefix.length)))
    if (!file.startsWith(root) || !existsSync(file)) {
        res.writeHead(404).end()
        return
    }
    res.writeHead(200, {'content-type': types[extname(file)] ?? 'application/octet-stream'}).end(readFileSync(file))
})
let base

// ---- Chrome over the DevTools protocol -----------------------------------------------------------------------------
let devtools
const startChrome = async () => {
    left.profile = mkdtempSync(`${profiles}${process.pid}-`)
    const chrome = spawn(findChrome(), [
        // Headless whatever the binary: the headless shell is, and a full Chrome would want a display.
        '--headless=new',
        // The port Chrome chooses, which it writes into the profile's DevToolsActivePort: no two runs collide.
        '--remote-debugging-port=0', `--user-data-dir=${left.profile}`,
        // None of Chrome's own requests. A full Chrome for Testing 156 still asks Google for accounts, update checks
        // and the component updater's downloads with these five flags (measured), so no host resolves but 127.0.0.1,
        // which makes those fail at once: every address of the test is 127.0.0.1. With --public, the pages resolve
        // Trystero's relays.
        '--no-first-run', '--no-default-browser-check', '--disable-component-update', '--disable-background-networking',
        '--disable-sync', ...(usePublic ? [] : ['--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1']),
        // Host candidates as IP addresses: headless Chrome cannot resolve its own mDNS names.
        '--disable-features=WebRtcHideLocalIpsWithMdns',
        // Chrome's sandbox refuses to start as root, as in a container.
        ...(process.getuid?.() === 0 ? ['--no-sandbox'] : []),
        'about:blank',
    ], {stdio: ['ignore', 'ignore', 'pipe']})
    left.chrome = chrome
    // What Chrome says on standard error, the last of it, for a Chrome that does not start.
    let said = ''
    chrome.stderr.on('data', data => (said = (said + data).slice(-2000)))
    const end = Date.now() + bound
    for (;;) {
        const active = join(left.profile, 'DevToolsActivePort')
        const port = existsSync(active) ? readFileSync(active, 'utf8').split('\n')[0] : ''
        if (/^[0-9]+$/.test(port)) {
            devtools = `http://127.0.0.1:${port}`
            try {
                await fetch(`${devtools}/json/version`)
                return
            } catch {}
        }
        if (Date.now() > end || exited(chrome)) throw new Error(`Chrome did not start; it said:\n${said}`)
        await new Promise(r => setTimeout(r, 100))
    }
}

const openPage = async (path, label) => {
    const target = await (await fetch(`${devtools}/json/new?about:blank`, {method: 'PUT'})).json()
    const ws = new WebSocket(target.webSocketDebuggerUrl)
    let id = 0
    const pending = new Map()
    const problems = []
    // Every call settles within ms, `bound` unless it says: a closing tab may never answer.
    const send = (method, params = {}, ms = bound) => new Promise(resolve => {
        const i = ++id
        const timer = setTimeout(() => {
            pending.delete(i)
            resolve(undefined)
        }, ms)
        pending.set(i, result => {
            clearTimeout(timer)
            resolve(result)
        })
        ws.send(JSON.stringify({id: i, method, params}))
    })
    ws.onmessage = m => {
        const d = JSON.parse(m.data)
        if (d.id && pending.has(d.id)) {
            pending.get(d.id)(d.result)
            pending.delete(d.id)
        }
        if (d.method === 'Runtime.exceptionThrown') problems.push(`${label}: exception ${d.params.exceptionDetails.exception?.description ?? d.params.exceptionDetails.text}`)
        if (d.method === 'Runtime.consoleAPICalled' && d.params.type === 'error') problems.push(`${label}: console.error ${d.params.args.map(a => a.value ?? a.description).join(' ')}`)
    }
    await new Promise(r => (ws.onopen = r))
    await send('Runtime.enable')
    await send('Page.navigate', {url: base + path})
    // A call that does not answer in time fails by name, with the expression it ran.
    const evaluate = async (expression, ms = bound) => {
        const answer = await send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true}, ms)
        if (answer === undefined) throw new Error(`${label}: ${expression} did not answer in ${ms / 1000} s`)
        if (answer.exceptionDetails) throw new Error(`${label}: ${expression}: ${answer.exceptionDetails.exception?.description ?? answer.exceptionDetails.text}`)
        return answer.result?.value
    }
    return {
        label,
        problems,
        events: async () => (await evaluate('window.events ?? []')) ?? [],
        log: async () => (await evaluate("document.getElementById('log')?.innerText ?? ''")) ?? '',
        leave: () => evaluate('window.leave()'),
        offline: () => evaluate('window.offline()'),
        online: () => evaluate('window.online()'),
        value: (expression, ms) => evaluate(expression, ms),
        close: async () => {
            ws.close()
            await fetch(`${devtools}/json/close/${target.id}`).catch(() => {})
        },
    }
}

// ---- the native peer: a process with the interface of a page -------------------------------------------------------
// How many native peers died by a signal the driver did not send, so far, and before the scenario under way.
let deaths = 0
let deathsBefore = 0
const openNative = (params, label) => {
    const args = Object.entries(params).flatMap(([k, v]) => [`--${k}`, String(v)])
    const child = spawn(nativePeer, args, {stdio: ['pipe', 'pipe', 'pipe']})
    left.natives.add(child)
    child.on('exit', () => left.natives.delete(child))
    const events = []
    const lines = []
    const problems = []
    createInterface({input: child.stdout}).on('line', line => {
        try {
            events.push(JSON.parse(line))
        } catch {
            problems.push(`${label}: not JSON on stdout: ${line}`)
        }
    })
    createInterface({input: child.stderr}).on('line', line => lines.push(line))
    // A signal other than the driver's own SIGTERM is a crash: named at once, and every wait of the scenario ends.
    child.on('exit', (code, signal) => {
        if (signal !== null && signal !== 'SIGTERM') {
            problems.push(`${label}: killed by ${signal}`)
            console.log(`  ${label} killed by ${signal}`)
            ++deaths
        } else if (code && code !== 143) {
            problems.push(`${label}: exited with ${code}`)
        }
    })
    return {
        label,
        problems,
        events: async () => events,
        log: async () => lines.join('\n'),
        value: async () => undefined,
        leave: async () => child.stdin.write('leave\n'),
        offline: async () => child.stdin.write('offline\n'),
        online: async () => child.stdin.write('online\n'),
        abort: async () => child.stdin.write('abort\n'),
        close: async () => {
            child.kill()
            await exit(child)
        },
    }
}

// ---- scenarios ---------------------------------------------------------------------------------------------------
const checksum = bytes => bytes.reduce((sum, b, i) => (sum + b * (i + 1)) >>> 0, 0)
const pattern = (size, step) => new Uint8Array(size).map((_, i) => (i * step) % 256)
const fromCpp = {size: 100_000, checksum: checksum(pattern(100_000, 13)), metadata: '{"name":"from-cpp.bin"}'}
const fromJs = {size: 100_000, checksum: checksum(pattern(100_000, 7)), metadata: '{"name":"from-js.bin"}'}
const wideFromCpp = {size: 1024 * 1024, checksum: checksum(pattern(1024 * 1024, 13))}
const wideFromJs = {size: 1024 * 1024, checksum: checksum(pattern(1024 * 1024, 7))}

// Waits for `condition`, at most `ms`, and no longer once a peer of the scenario died by a signal: what it waits for
// may never come, and the death is the failure to name.
const until = async (condition, ms = bound) => {
    const end = Date.now() + ms
    while (Date.now() < end && deaths === deathsBefore) {
        if (await condition()) return true
        await new Promise(r => setTimeout(r, 250))
    }
    return condition()
}

const has = (events, kind, f = () => true) => events.some(e => e.kind === kind && f(e))
const count = (events, kind, f = () => true) => events.filter(e => e.kind === kind && f(e)).length
// A peer's own ids, once it joined its room: its peer id and its user's.
const idsOf = async page => {
    const joined = async () => (await page.events()).find(e => e.kind === 'joined')
    if (!(await until(async () => Boolean(await joined())))) throw new Error(`${page.label} did not join its room`)
    const {self, user} = await joined()
    return {self, user}
}

/**
 * What a peer must have seen of the partner `of` (its ids): its join with its user, its chat message and the echo of
 * its own, its JSON value and its file, its request, the answer to its own request, a ping, and its own sends.
 */
const exchanged = (events, of, partnerIsCpp) => {
    const from = e => e.from === of.self
    const file = partnerIsCpp ? fromCpp : fromJs
    const [name, other] = partnerIsCpp ? ['C++', 'Trystero JS'] : ['Trystero JS', 'C++']
    return has(events, 'peer-join', e => e.peer === of.self && e.user === of.user) &&
        has(events, 'chat', e => from(e) && e.text === `hello from ${name}`) &&
        has(events, 'chat', e => from(e) && e.text === `echo: hello from ${other}`) &&
        has(events, 'drink', e => from(e) && e.value?.drink === (partnerIsCpp ? 'cafezinho' : 'negroni')) &&
        has(events, 'file', e => from(e) && e.size === file.size && e.checksum === file.checksum && e.metadata === file.metadata) &&
        has(events, 'request', e => from(e) && e.text === `question from ${name}`) &&
        has(events, 'response', e => e.peer === of.self && e.error === '' && e.text === `answer to: question from ${other}`) &&
        has(events, 'ping', e => e.peer === of.self && e.error === '') &&
        ['chat', 'drink', 'file'].every(what => has(events, 'sent', e => e.peer === of.self && e.what === what && e.error === ''))
}

// Waits until a C++ peer and a page exchanged everything both ways; names what is missing when they did not.
const exchange = async (cpp, js) => {
    const c = await idsOf(cpp), j = await idsOf(js)
    const done = async () => exchanged(await cpp.events(), j, false) && exchanged(await js.events(), c, true)
    if (await until(done)) return null
    const missing = []
    if (!exchanged(await cpp.events(), j, false)) missing.push(`${cpp.label} of ${js.label}`)
    if (!exchanged(await js.events(), c, true)) missing.push(`${js.label} of ${cpp.label}`)
    return `the exchange is incomplete: ${missing.join(', ')}`
}

const results = []
// The C++ peers' STUN server: the discard port of the loopback, where nothing answers, so that no peer asks a public
// one.
const ice = 'stun:127.0.0.1:9'
const app = 'webcpp-trystero-interop'
const freshRoom = () => 'room-' + Math.random().toString(36).slice(2, 10)
let relayParams

// The peer id 0.27.0 derives from the endpoint key whose scalar is the SHA-256 of `label`, as a C++ peer given
// that label as its `key` has; and a label whose id is below `id` (`below`) or above it.
const {getVerifiedPubKeyRecord} = await import(pathToFileURL(join(here, '../node_modules/@trystero-p2p/core/dist/identity.mjs')).href)
const peerIdOf = async label => {
    const ecdh = createECDH('prime256v1')
    ecdh.setPrivateKey(createHash('sha256').update(label).digest())
    const point = ecdh.getPublicKey()
    const jwk = {kty: 'EC', crv: 'P-256', x: point.subarray(1, 33).toString('base64url'), y: point.subarray(33).toString('base64url')}
    return (await getVerifiedPubKeyRecord(jwk)).peerId
}
const keyAround = async (id, below) => {
    for (let i = 0; ; ++i) {
        const label = `key-${i}-${Math.random().toString(36).slice(2, 10)}`
        if (((await peerIdOf(label)) < id) === below) return label
    }
}

// 0.27.0's topics of a room: the root, hashTopic of topicPath(Trystero, app, room), the base64url SHA-256 of the JSON
// array of its parts; and a peer's own, hashTopic of topicPath(root's path, peer id).
const rootPath = room => JSON.stringify(['Trystero', app, room])
const hashTopic = path => createHash('sha256').update(path).digest('base64url')
const rootTopicOf = room => hashTopic(rootPath(room))
const selfTopicOf = (room, peer) => hashTopic(JSON.stringify([rootPath(room), peer]))

// What the local relay forwarded of `peer`'s signals of kind `signal` ('announce', 'offer', 'answer', 'candidate'),
// on `topic` when given.
const relayed = (peer, signal, topic) =>
    left.relay.received.filter(e => e.peerId === peer && e.signal === signal && (topic === undefined || e.topic === topic))

// What anyone who reads and writes the relays can send, through a client of the local relay of its own, with 0.27.0's
// own code: Nostr events (createEvent, signed by a key of its own) and signals sealed under a room's key (genKey,
// encrypt). Each publish waits for the relay's OK, which the relay gives once every subscription has the event.
const {createEvent, createSigner} = await import(pathToFileURL(join(here, '../node_modules/@trystero-p2p/nostr/dist/protocol.mjs')).href)
const {encrypt, genKey} = await import(pathToFileURL(join(here, '../node_modules/@trystero-p2p/core/dist/crypto.mjs')).href)
const openPublisher = async () => {
    const socket = new WebSocket(left.relay.url)
    const acks = new Map()
    await new Promise((resolve, reject) => {
        socket.onopen = resolve
        socket.onerror = () => reject(new Error('the publisher could not reach the relay'))
    })
    socket.onmessage = ({data}) => {
        const [type, id, ok, why] = JSON.parse(data)
        if (type === 'OK') acks.get(id)?.(ok ? null : why)
    }
    socket.onclose = () => {
        for (const ack of acks.values()) ack('the relay closed the publisher')
    }
    const signer = createSigner()
    return {
        publish: async (topic, content) => {
            const text = await createEvent(topic, JSON.stringify(content), signer)
            const {id} = JSON.parse(text)[1]
            const refused = await new Promise(resolve => {
                acks.set(id, resolve)
                socket.send(text)
            })
            acks.delete(id)
            if (refused) throw new Error(`the relay refused an event: ${refused}`)
        },
        close: () => socket.close(),
    }
}

// Runs a scenario against the C++ peer of `cppKind`: 'cpp' (the browser's, in a page) or 'native' (a process). Its
// body gets `open(kind, params)`, which opens a peer, in the scenario's room unless `params.room` names another:
// 'cpp' (the C++ peer of the lane), 'js' or 'forger' (pages of Trystero 0.27.0).
const scenario = async (name, cppKind, body) => {
    const shown = `${name} (${cppKind === 'native' ? 'native' : 'browser'})`
    if (scenarioFilter && !shown.includes(scenarioFilter)) return
    const started = Date.now()
    deathsBefore = deaths
    const pages = []
    const room = freshRoom()
    const open = async (kind, params = {}) => {
        const all = {app, room, ...relayParams, ...(kind === 'cpp' ? {ice} : {}), ...params}
        const label = `${kind === 'cpp' ? cppKind : kind}#${pages.length}`
        const page = kind === 'cpp' && cppKind === 'native'
            ? openNative(all, label)
            : await openPage(`/${kind}.html?${new URLSearchParams({...all, program: basename(browserPeer ?? '')})}`, label)
        pages.push(page)
        return page
    }
    let failure = null
    try {
        failure = await body(open)
    } catch (error) {
        failure = String(error?.stack ?? error)
    }
    // The peers' problems are named even when the scenario failed on its own: a crash is the reason it did.
    const problems = pages.flatMap(p => p.problems)
    if (problems.length) failure = [failure, ...problems].filter(Boolean).join('\n')
    const seconds = ((Date.now() - started) / 1000).toFixed(1)
    results.push({name: shown, ok: !failure})
    console.log(`${failure ? 'FAIL' : 'pass'}  ${shown}  (${seconds} s)`)
    if (failure || verbose) {
        if (failure) console.log('  ' + failure)
        for (const p of pages) {
            console.log(`  ---- ${p.label} events: ${JSON.stringify(await p.events())}`)
            console.log((await p.log()).split('\n').map(l => '  | ' + l).join('\n'))
        }
    }
    for (const p of pages) await p.close()
}

// The peers of a room that joined `of` (its ids): none must have, checked once what must happen has, and at the end.
const joinedBy = async (pages, of) => {
    const joined = []
    for (const page of pages) {
        if (has(await page.events(), 'peer-join', e => e.peer === of.self)) joined.push(page.label)
    }
    return joined
}

// How many reports of one kind one peer may have made by now, at most one per peer and stage each 60 s (0.27.0's
// reportDecryptErr): one, and one more for each 60 s since `since`, however slow the machine is.
const reportsAllowed = since => 1 + Math.floor((Date.now() - since) / 60_000)

// The library's offer_ttl, read from its source: an unanswered offer holds its connection that long, and the room may
// make another max_connections_per_window connections and offer to another max_unconnected_peers peers once it passed.
const offerTtl = Number(/offer_ttl = std::chrono::milliseconds\(([\d']+)\)/.exec(
    readFileSync(join(here, '../../../include/webcpp/trystero/detail/signaling.hpp'), 'utf8'))[1].replaceAll("'", ''))
// How many windows of offer_ttl a run that began at `since` spans, at most: one, and one more for each offer_ttl.
const windowsSince = since => 1 + Math.floor((Date.now() - since) / offerTtl)

// 1. Each side sees the other's peer and user ids (the C++ peer's app::self_id and app::user_id, the page's
// getSelf()), and text, JSON and binary messages go both ways: with either side offering, as in 0.27.0 the lower id
// offers, so the C++ peer's id is put below the page's, then above it (its key, `keyAround`), and the local relay
// shows the offer of the lower. No candidate of either is refused (the malformed candidate scenario's control).
const idsAndMessages = cppKind => scenario('ids_and_messages_both_ways', cppKind, async open => {
    for (const cppLower of [true, false]) {
        const order = cppLower ? 'the C++ peer offering' : 'the page offering'
        const room = freshRoom()
        const js = await open('js', {room})
        const j = await idsOf(js)
        const cpp = await open('cpp', {room, key: await keyAround(j.self, cppLower)})
        const c = await idsOf(cpp)
        if (!/^[0-9A-Za-z]{20}$/.test(c.self) || !/^[0-9A-Za-z]{22}$/.test(c.user)) {
            return `the C++ peer's ids ${JSON.stringify(c)} are not 0.27.0's`
        }
        if ((c.self < j.self) !== cppLower) return `the C++ peer's id ${c.self} is not ${cppLower ? 'below' : 'above'} ${j.self}`
        const failed = await exchange(cpp, js)
        if (failed) return `${order}: ${failed}`
        if (!usePublic) {
            const [offerer, answerer] = cppLower ? [c, j] : [j, c]
            if (!relayed(offerer.self, 'offer', selfTopicOf(room, answerer.self)).length) return `${order}: the relay saw no offer of ${offerer.self}`
        }
        const seen = await js.value('window.peers()')
        if (seen[c.self] !== c.user) return `${order}: the page's getPeers() is ${JSON.stringify(seen)}, not ${c.self} of ${c.user}`
        if (!has(await cpp.events(), 'user-join', e => e.user === j.user && e.peer === j.self)) return `${order}: the C++ peer saw no user join`
        if (!has(await js.events(), 'user-join', e => e.user === c.user && e.peer === c.self)) return `${order}: the page saw no user join`
        if ((await cpp.log()).includes('dropped a candidate')) return `${order}: the C++ peer refused a candidate of the page`
    }
    return null
})

// 2. The page's onPeerJoin fires for the C++ peer only once the page verified the C++ peer's endpoint and user
// signatures (0.27.0's handshake), and the C++ peer's on_peer_join for the page only once it verified the page's: each
// joins the other, with the user the other presents, and neither reports an error.
const signatures = cppKind => scenario('js_verifies_cpp_signatures', cppKind, async open => {
    const cpp = await open('cpp')
    const js = await open('js')
    const c = await idsOf(cpp), j = await idsOf(js)
    const joined = async () => has(await js.events(), 'peer-join', e => e.peer === c.self && e.user === c.user) &&
        has(await cpp.events(), 'peer-join', e => e.peer === j.self && e.user === j.user)
    if (!(await until(joined))) return 'the peers did not join each other with their users'
    const errors = [...await cpp.events(), ...await js.events()].filter(e => e.kind === 'error')
    return errors.length ? `errors: ${JSON.stringify(errors)}` : null
})

// 3. 1 MiB each way, on the action `wide`, in 0.27.0's wide frames: the page sends and receives frames of 64 KiB
// (maxBulkFrameBytes, js.html's window.largest), where a link that fell back to the narrow chunks has 16 KiB ones.
const wideFrame = 64 * 1024
const wideTransfers = cppKind => scenario('wide_transfers_both_ways', cppKind, async open => {
    const cpp = await open('cpp', {wide: 1})
    const js = await open('js', {wide: 1})
    const c = await idsOf(cpp), j = await idsOf(js)
    const got = (events, from, payload) =>
        has(events, 'wide', e => e.from === from && e.size === payload.size && e.checksum === payload.checksum)
    const done = async () => got(await cpp.events(), j.self, wideFromJs) && got(await js.events(), c.self, wideFromCpp) &&
        has(await cpp.events(), 'sent', e => e.what === 'wide' && e.error === '') &&
        has(await js.events(), 'sent', e => e.what === 'wide' && e.error === '')
    if (!(await until(done))) return 'the 1 MiB transfers did not both complete'
    const largest = await js.value('window.largest')
    if (largest.sent !== wideFrame || largest.received !== wideFrame) {
        return `the page's largest frames are ${JSON.stringify(largest)}, not ${wideFrame} bytes each way`
    }
    return null
})

// 4. One password meets; another gives decrypt_failed on the C++ side and decrypt-failed on the page (0.27.0's code
// name both), and no join. Of two peers of different passwords the lower id offers and the higher cannot open the
// offer, as in 0.27.0: so the C++ peer's id is put below the page's, then above it (its key, `keyAround`), and the
// higher of the two reports the offer it could not open, naming the lower, once however often the lower offers (the
// local relay shows a second offer forwarded to it).
const passwords = cppKind => scenario('passwords', cppKind, async open => {
    const failed = await exchange(await open('cpp', {password: 'secret'}), await open('js', {password: 'secret'}))
    if (failed) return `with one password: ${failed}`
    for (const cppLower of [true, false]) {
        const room = freshRoom()
        const since = Date.now()
        const js = await open('js', {room, password: 'another'})
        const j = await idsOf(js)
        const cpp = await open('cpp', {room, password: 'secret', key: await keyAround(j.self, cppLower)})
        const c = await idsOf(cpp)
        if ((c.self < j.self) !== cppLower) return `the C++ peer's id ${c.self} is not ${cppLower ? 'below' : 'above'} ${j.self}`
        const [reporter, offerer, reporterId] = cppLower ? [js, c, j] : [cpp, j, c]
        const reports = async () => count(await reporter.events(), 'error', e => e.code === 'decrypt-failed' && e.peer === offerer.self)
        if (!(await until(async () => (await reports()) > 0))) {
            return `${reporter.label}, the higher id, did not report decrypt-failed for the offer of ${offerer.self}`
        }
        if (!usePublic && !(await until(() => relayed(offerer.self, 'offer', selfTopicOf(room, reporterId.self)).length >= 2))) {
            return `the relay forwarded no second offer of ${offerer.self}`
        }
        const reported = await reports()
        if (reported > reportsAllowed(since)) return `${reporter.label} reported decrypt-failed ${reported} times`
        const joined = [...await joinedBy([js], c), ...await joinedBy([cpp], j)]
        if (joined.length) return `peers with different passwords joined: ${joined.join(', ')}`
    }
    return null
})

// 5. The C++ peer imports a trystero_uk_ text a page exported (createUserIdentity, exportUserIdentity), and a page
// imports one a C++ peer exported (create_user_identity, export_user_identity): each presents the user it imported,
// and each sees the other's.
const identityOf = async page => {
    if (!(await until(async () => has(await page.events(), 'identity')))) throw new Error(`${page.label} exported no identity`)
    return (await page.events()).find(e => e.kind === 'identity')
}
const importedIdentities = cppKind => scenario('imported_identities', cppKind, async open => {
    const exporting = freshRoom()
    const fromPage = await identityOf(await open('js', {room: exporting, export: 1}))
    const fromCppPeer = await identityOf(await open('cpp', {room: exporting, export: 1}))
    for (const {text} of [fromPage, fromCppPeer]) {
        if (!/^trystero_uk_[A-Za-z0-9_-]+$/.test(text)) return `an exported text is not a user key: ${text}`
    }
    const cpp = await open('cpp', {identity: fromPage.text})
    const js = await open('js', {identity: fromCppPeer.text})
    const c = await idsOf(cpp), j = await idsOf(js)
    if (c.user !== fromPage.user) return `the C++ peer presents ${c.user}, not the page's ${fromPage.user}`
    if (j.user !== fromCppPeer.user) return `the page presents ${j.user}, not the C++ peer's ${fromCppPeer.user}`
    return exchange(cpp, js)
})

// 6. Two C++ apps importing one identity (a third C++ app exported it, in a room of its own), and a page: the page
// sees two peers and one user.
const oneUserTwoCppPeers = cppKind => scenario('one_user_two_cpp_peers', cppKind, async open => {
    const {text, user} = await identityOf(await open('cpp', {room: freshRoom(), export: 1}))
    const first = await open('cpp', {identity: text})
    const second = await open('cpp', {identity: text})
    const js = await open('js')
    const a = await idsOf(first), b = await idsOf(second)
    if (a.user !== user || b.user !== user) return `the C++ peers present ${a.user} and ${b.user}, not ${user}`
    const seen = async () => {
        const events = await js.events()
        return has(events, 'peer-join', e => e.peer === a.self && e.user === user) &&
            has(events, 'peer-join', e => e.peer === b.self && e.user === user)
    }
    if (!(await until(seen))) return 'the page did not see both C++ peers join'
    const users = await js.value('window.users()')
    if (Object.keys(users).length !== 1 || users[user]?.peerIds?.length !== 2) {
        return `the page's getUsers() is ${JSON.stringify(users)}, not one user of two peers`
    }
    const userJoins = count(await js.events(), 'user-join', e => e.user === user)
    if (userJoins !== 1) return `the page saw the user join ${userJoins} times`
    // The two C++ peers are one user: each joins the other, and neither sees a user join of its own user.
    if (has([...await first.events(), ...await second.events()], 'user-join', e => e.user === user)) {
        return 'a C++ peer saw its own user join'
    }
    return null
})

// 7. A page room with maxUsers 2 holding two users refuses the C++ peer as a third (room_full on the C++ side), and
// a C++ room with max_users 2 holding two refuses a page (room-full on the page). What must not happen, the third
// joining, is checked once the refusal came, and again at the end.
const maxUsers = cppKind => scenario('max_users_both_ways', cppKind, async open => {
    const refusedJoins = []
    {
        const full = await open('js', {maxUsers: 2})
        const other = await open('js')
        const f = await idsOf(full), o = await idsOf(other)
        if (!(await until(async () => has(await full.events(), 'peer-join', e => e.peer === o.self)))) {
            return 'the page of maxUsers 2 did not take its second user'
        }
        const cpp = await open('cpp')
        const c = await idsOf(cpp)
        const refused = async () => has(await cpp.events(), 'error', e => e.code === 'room-full' && e.peer === f.self)
        if (!(await until(refused))) return 'the C++ peer was not refused with room_full by the full page'
        refusedJoins.push([full, c], [cpp, f])
    }
    {
        const room = freshRoom()
        const cpp = await open('cpp', {room, maxUsers: 2})
        const first = await open('js', {room})
        const c = await idsOf(cpp), j = await idsOf(first)
        if (!(await until(async () => has(await cpp.events(), 'peer-join', e => e.peer === j.self)))) {
            return 'the C++ room of max_users 2 did not take its second user'
        }
        const third = await open('js', {room})
        const t = await idsOf(third)
        const refused = async () => has(await third.events(), 'error', e => e.code === 'room-full' && e.peer === c.self)
        if (!(await until(refused))) return 'the page was not refused with room-full by the full C++ room'
        refusedJoins.push([cpp, t], [third, c])
    }
    for (const [page, of] of refusedJoins) {
        if ((await joinedBy([page], of)).length) return `${page.label} joined ${of.self}, a third user`
    }
    return null
})

// 8. A request each way, answered, and a ping each way.
const requestsAndPings = cppKind => scenario('requests_and_pings', cppKind, async open => {
    const cpp = await open('cpp')
    const js = await open('js')
    const c = await idsOf(cpp), j = await idsOf(js)
    const answered = (events, peer, text) => has(events, 'response', e => e.peer === peer && e.error === '' && e.text === text)
    const pinged = (events, peer) => has(events, 'ping', e => e.peer === peer && e.error === '' && e.ms >= 0)
    const done = async () => answered(await cpp.events(), j.self, 'answer to: question from C++') &&
        answered(await js.events(), c.self, 'answer to: question from Trystero JS') &&
        pinged(await cpp.events(), j.self) && pinged(await js.events(), c.self)
    return (await until(done)) ? null : 'a request or a ping did not complete both ways'
})

// 9. A C++ leave is the page's onPeerLeave and onUserLeave, and the page's leave the C++ peer's on_peer_leave and
// on_user_leave.
const leaves = cppKind => scenario('leaves', cppKind, async open => {
    for (const leaver of ['cpp', 'js']) {
        const room = freshRoom()
        const cpp = await open('cpp', {room})
        const js = await open('js', {room})
        const failed = await exchange(cpp, js)
        if (failed) return `${leaver} leaving: ${failed}`
        const [goes, stays] = leaver === 'cpp' ? [cpp, js] : [js, cpp]
        const g = await idsOf(goes)
        await goes.leave()
        const seen = async () => {
            const events = await stays.events()
            return has(events, 'peer-leave', e => e.peer === g.self && e.user === g.user) &&
                has(events, 'user-leave', e => e.user === g.user && e.peer === g.self) &&
                has(await goes.events(), 'left', e => e.error === '')
        }
        if (!(await until(seen))) return `${goes.label} left, and ${stays.label} did not see its peer and its user leave`
    }
    return null
})

// 10. forger.html joins with a custom identity whose sign() passes 0.27.0's startup check and then signs with
// another key: the C++ peer reports handshake_failed for it, once per handshake run (one per connection the C++
// peer's log shows negotiated with it, by its offer ids), and never joins it.
const forgedUserSignature = cppKind => scenario('a_forged_user_signature', cppKind, async open => {
    const cpp = await open('cpp')
    const forger = await open('forger')
    const f = await idsOf(forger)
    const reports = async () => count(await cpp.events(), 'error', e => e.code === 'handshake-failed' && e.peer === f.self)
    if (!(await until(async () => (await reports()) > 0))) return 'the C++ peer did not report handshake_failed for the forger'
    if (count(await forger.events(), 'signed') < 2) return 'the forger signed nothing after its startup check'
    if ((await joinedBy([cpp], f)).length) return 'the C++ peer joined the forger'
    const reported = await reports()
    const log = await cpp.log()
    const runs = new Set([
        ...[...log.matchAll(new RegExp(`sending offer (\\S+) to ${f.self}`, 'g'))].map(m => m[1]),
        ...[...log.matchAll(new RegExp(`offer (\\S+) from ${f.self}`, 'g'))].map(m => m[1]),
    ]).size
    if (reported > runs) return `the C++ peer reported handshake_failed ${reported} times for ${runs} connections`
    return (await joinedBy([cpp], f)).length ? 'the C++ peer joined the forger' : null
})

// 11. Two pages and a C++ peer: each joins both others, and exchanges with each.
const mesh = cppKind => scenario('mesh', cppKind, async open => {
    const first = await open('js')
    const cpp = await open('cpp')
    const second = await open('js')
    for (const js of [first, second]) {
        const failed = await exchange(cpp, js)
        if (failed) return failed
    }
    const a = await idsOf(first), b = await idsOf(second)
    const met = async () => has(await first.events(), 'peer-join', e => e.peer === b.self) &&
        has(await second.events(), 'peer-join', e => e.peer === a.self)
    return (await until(met)) ? null : 'the two pages did not join each other'
})

// 12. Relay connectivity, each side in turn, as the local relay sees it. Two peers meet; one, the holder, goes
// offline: the relay has its subscriptions closed. The other leaves and a new peer joins in its place, whose
// announcement the relay forwards to no subscription of the holder, which does not meet it then; the holder goes
// online, subscribes again, and meets the new peer. Then a late peer of the holder's kind starts offline: once the
// relay forwarded an announcement of the new peer after the late one joined, the late one has neither subscribed nor
// sent anything; it goes online, subscribes, and meets the new peer.
const relayConnectivity = cppKind => scenario('relay_connectivity', cppKind, async open => {
    for (const offline of ['cpp', 'js']) {
        const room = freshRoom()
        const [mine, theirs] = offline === 'cpp' ? ['cpp', 'js'] : ['js', 'cpp']
        const pair = (a, b) => (offline === 'cpp' ? exchange(a, b) : exchange(b, a))
        const subscribed = page => left.relay.subscribed(selfTopicOf(room, page.self))
        const holder = await open(mine, {room})
        const leaver = await open(theirs, {room})
        const h = await idsOf(holder), l = await idsOf(leaver)
        const failed = await pair(holder, leaver)
        if (failed) return `${offline} offline: ${failed}`
        await holder.offline()
        if (!(await until(() => subscribed(h) === 0))) return `${holder.label} went offline, and the relay still has its subscription`
        await leaver.leave()
        if (!(await until(async () => has(await holder.events(), 'peer-leave', e => e.peer === l.self)))) {
            return `${holder.label} did not see ${leaver.label} leave`
        }
        const next = await open(theirs, {room})
        const n = await idsOf(next)
        if (!(await until(() => relayed(n.self, 'announce', rootTopicOf(room)).length > 0))) {
            return `the relay forwarded no announcement of ${next.label}`
        }
        if (subscribed(h) !== 0) return `${holder.label} subscribed again while offline`
        if ((await joinedBy([holder], n)).length) return `${holder.label} met ${next.label} while offline`
        await holder.online()
        if (!(await until(() => subscribed(h) > 0))) return `${holder.label} went online, and did not subscribe again`
        const again = await pair(holder, next)
        if (again) return `${offline} back online: ${again}`

        const late = await open(mine, {room, offline: 1})
        const t = await idsOf(late)
        const announced = relayed(n.self, 'announce', rootTopicOf(room)).length
        if (!(await until(() => relayed(n.self, 'announce', rootTopicOf(room)).length > announced))) {
            return `the relay forwarded no announcement of ${next.label} after ${late.label} joined`
        }
        if (subscribed(t) !== 0) return `${late.label} started offline, and subscribed`
        if (left.relay.received.some(e => e.peerId === t.self)) return `${late.label} started offline, and sent a signal`
        await late.online()
        if (!(await until(() => subscribed(t) > 0))) return `${late.label} went online, and did not subscribe`
        const met = await pair(late, next)
        if (met) return `${late.label} online: ${met}`
    }
    return null
})

// ---- adversarial peers on the relays ------------------------------------------------------------------------------
// A peer of the room sends the C++ peer candidates that are not candidates, sealed under the room's key for the C++
// peer's offer to the page in the page's name, as a broken or hostile peer of the room can: a text that is not JSON,
// and an RTCIceCandidateInit whose candidate WebRTC refuses (in a page, Chrome's addIceCandidate rejects it). The C++
// peer's id is below the page's, so that it offers; the relay holds the page's answer until the candidates are in, so
// that the C++ peer holds them until its offer is answered, and then adds them: it drops each, says so, throws
// nothing into WebAssembly, and the exchange completes. ids_and_messages_both_ways is the control: no candidate of a
// page is dropped there.
const malformedCandidates = ['not JSON at all', JSON.stringify({candidate: 'candidate:not a candidate', sdpMid: '0'})]
const malformedCandidateScenario = cppKind => scenario('a_malformed_candidate', cppKind, async open => {
    const room = freshRoom()
    const js = await open('js', {room})
    const j = await idsOf(js)
    left.relay.options.hold = (topic, content) => content?.peerId === j.self && typeof content?.answer === 'string'
    let cpp
    try {
        cpp = await open('cpp', {room, key: await keyAround(j.self, true)})
        const c = await idsOf(cpp)
        if (!(c.self < j.self)) return `the C++ peer's id ${c.self} is not below ${j.self}`
        if (!(await until(() => relayed(c.self, 'offer', selfTopicOf(room, j.self)).length > 0))) {
            return 'the relay forwarded no offer of the C++ peer to the page'
        }
        const {offerId} = relayed(c.self, 'offer', selfTopicOf(room, j.self))[0]
        const key = genKey('', app, room)
        const publisher = await openPublisher()
        try {
            for (const candidate of malformedCandidates) {
                await publisher.publish(selfTopicOf(room, c.self), {
                    peerId: j.self, offerId, candidate: await encrypt(key, candidate, `candidate|${j.self}|${offerId}`),
                })
            }
        } finally {
            publisher.close()
        }
    } finally {
        left.relay.release()
    }
    const failed = await exchange(cpp, js)
    if (failed) return failed
    const dropped = (await cpp.log()).split(`dropped a candidate from ${j.self}`).length - 1
    return dropped >= malformedCandidates.length
        ? null : `the C++ peer dropped ${dropped} candidates of the page, not the ${malformedCandidates.length} malformed ones`
})

// After a page joined the C++ peer, a flooder who reads and writes the relays, with no key, sends what no key is needed
// for, each from fresh ids: offers sealed by no key to the C++ peer, each twice, from ids below its own, and then
// announcements on the room's topic from ids above its own, which the C++ peer offers to. The C++ peer handles every
// one (its log names each id, or its refusal); reports each offer it cannot open as decrypt-failed at most once per
// peer and stage (0.27.0's reportDecryptErr); offers to at most max_unconnected_peers (32) of the announcing ids, makes
// at most max_connections_per_window (64) connections in the flood (all three bounds times the offer_ttl windows the
// run spans, one on a normal run), and every offer of the flood is reported (counted as the platform counts them: cpp.html's
// RTCPeerConnections, the native backend's made-connection events), and goes on with the page: no peer leaves, and a
// chat message after the flood is echoed. The page offers to the announcing ids too, as a 0.27.0 peer does.
const floodCount = 300
const unconnectedBound = 32
const connectionBound = 64
const floodScenario = cppKind => scenario('a_flood_of_signals_from_fresh_ids', cppKind, async open => {
    const room = freshRoom()
    const since = Date.now()
    const cpp = await open('cpp', {room})
    const js = await open('js', {room})
    const failed = await exchange(cpp, js)
    if (failed) return failed
    const c = await idsOf(cpp)
    const fresh = prefix => {
        const alphabet = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'
        return prefix + Array.from({length: 20 - prefix.length}, () => alphabet[Math.floor(Math.random() * 62)]).join('')
    }
    const offering = Array.from({length: floodCount}, () => fresh('0000000000'))
    const announcing = Array.from({length: floodCount}, () => fresh('zzzzzzzzzz'))
    if (!(offering.every(id => id < c.self) && announcing.every(id => id > c.self))) {
        return `the C++ peer's id ${c.self} is not between the flood's`
    }
    const connections = async () => cppKind === 'native'
        ? count(await cpp.events(), 'made-connection')
        : await cpp.value('window.madePeerConnections')
    const before = {connections: await connections(), cpp: (await cpp.events()).length, js: (await js.events()).length}
    const publisher = await openPublisher()
    try {
        for (const peerId of offering) {
            for (let i = 0; i < 2; ++i) {
                await publisher.publish(selfTopicOf(room, c.self), {peerId, offerId: 'forged', offer: 'not a sealed offer'})
            }
        }
        for (const peerId of announcing) await publisher.publish(rootTopicOf(room), {peerId})
    } finally {
        publisher.close()
    }
    const ids = async said => new Set([...(await cpp.log()).matchAll(new RegExp(`(?:${said}|refusing) (\\w{20})`, 'g'))].map(m => m[1]))
    const handled = async () => {
        const opened = await ids('could not open the offer from'), heard = await ids('announcement from')
        return offering.every(id => opened.has(id)) && announcing.every(id => heard.has(id))
    }
    if (!(await until(handled))) return 'the C++ peer did not handle every signal of the flood'
    const log = await cpp.log()
    const offered = new Set([...log.matchAll(/announcement from (\w{20}): our id is lower, so we offer/g)]
        .map(m => m[1]).filter(id => announcing.includes(id)))
    const made = (await connections()) - before.connections
    const reports = new Map()
    for (const e of (await cpp.events()).slice(before.cpp)) {
        if (e.kind === 'error' && e.code === 'decrypt-failed') reports.set(e.peer, (reports.get(e.peer) ?? 0) + 1)
    }
    const facts = `the C++ peer offered to ${offered.size} of ${floodCount} announcing ids, made ${made} connections ` +
        `in the flood, and reported decrypt-failed for ${reports.size} of ${floodCount} offering ids`
    console.log(`  ${facts}`)
    const windows = windowsSince(since)
    if (offered.size > unconnectedBound * windows) return `${facts}; it offered to more than ${unconnectedBound * windows}`
    if (made > connectionBound * windows) return `${facts}; more than ${connectionBound * windows} connections`
    if (reports.size !== floodCount) return `${facts}; not every offer of the flood was reported`
    const allowed = reportsAllowed(since)
    const often = [...reports].filter(([, n]) => n > allowed)
    if (often.length) return `${facts}; reported more than ${allowed} times: ${JSON.stringify(often)}`
    const text = 'still exchanging after the flood'
    await js.value(`window.chat(${JSON.stringify(text)}, ${JSON.stringify(c.self)})`)
    const echoed = async () => (await js.events()).slice(before.js).some(e => e.kind === 'chat' && e.text === `echo: ${text}`)
    if (!(await until(echoed))) return `${facts}; the page's chat message after the flood was not echoed`
    if ([...(await cpp.events()).slice(before.cpp), ...(await js.events()).slice(before.js)].some(e => e.kind === 'peer-leave')) {
        return `${facts}; a peer left during the flood`
    }
    return null
})

// ---- the C++ peer's own robustness on the relays ------------------------------------------------------------------
// The C++ peer's relays begin with a URL no WebSocket can be made to: that relay reports a close, as an unreachable
// one does, which the C++ peer's log says, and the room goes on over the next one.
const refusedRelay = 'ftp://relay.invalid'
const refusedRelayScenario = cppKind => scenario('a_refused_relay_url', cppKind, async open => {
    const cpp = await open('cpp', {relays: `${refusedRelay},${relayParams.relays}`})
    const failed = await exchange(cpp, await open('js'))
    if (failed) return failed
    return (await cpp.log()).includes(`could not reach ${refusedRelay}`) ? null : 'the C++ peer did not report the refused relay'
})

// The C++ peer is given a TURN server without credentials, which WebRTC refuses: the app drops it when it is made and
// reports it to app::on_error, and the exchange completes over the host candidates.
const unusableIceScenario = cppKind => scenario('an_unusable_ice_server', cppKind, async open => {
    const cpp = await open('cpp', {ice: 'turn:127.0.0.1:9'})
    const failed = await exchange(cpp, await open('js'))
    if (failed) return failed
    return has(await cpp.events(), 'error', e => e.code === 'the configuration is unusable')
        ? null : 'the C++ peer did not report the unusable ICE server'
})

// After the exchange, the relay sends every client a text longer than a relay's longest (1 MiB): the C++ peer closes
// that relay's connection rather than read it, says so, reconnects, and goes on with the peer it joined. Then the relay
// sends the text after every subscription: each reconnection is closed at once, and the C++ peer tries again within a
// bound that doubles every time, rather than every few seconds forever.
const oversizedRelayTextScenario = cppKind => scenario('a_relay_text_longer_than_1_mib', cppKind, async open => {
    const cpp = await open('cpp')
    const js = await open('js')
    const failed = await exchange(cpp, js)
    if (failed) return failed
    const cppStart = (await cpp.events()).length
    const jsStart = (await js.events()).length
    left.relay.broadcast(JSON.stringify(['NOTICE', 'x'.repeat(1024 * 1024 + 1)]))
    if (!(await until(async () => (await cpp.log()).includes(`lost ${left.relay.url}`)))) {
        return 'the C++ peer did not close the relay that sent the long text'
    }
    if (!(await until(async () => (await cpp.log()).includes(`reconnected to ${left.relay.url}`)))) {
        return 'the C++ peer did not reconnect to the relay'
    }
    const bounds = async () =>
        [...(await cpp.log()).matchAll(new RegExp(`lost ${left.relay.url}, next attempt within (\\d+) ms`, 'g'))].map(m => Number(m[1]))
    // The text closes the connection open now, and then each new one after its subscription.
    left.relay.options.afterSubscribe = JSON.stringify(['NOTICE', 'x'.repeat(1024 * 1024 + 1)])
    left.relay.broadcast(left.relay.options.afterSubscribe)
    const lostAgain = await until(async () => (await bounds()).length >= 3)
    left.relay.options.afterSubscribe = null
    const seen = await bounds()
    console.log(`  the C++ peer lost the relay ${seen.length} times, each time to try again within ${seen.join(', ')} ms`)
    if (!lostAgain) return 'the C++ peer was not closed three times'
    if (!seen.every((b, i) => i === 0 || b > seen[i - 1])) return `the bounds did not grow: ${seen.join(', ')}`
    const c = await idsOf(cpp)
    const text = `still connected after relay faults`
    await js.value(`window.chat(${JSON.stringify(text)}, ${JSON.stringify(c.self)})`)
    const echoed = async () => (await js.events()).slice(jsStart).some(e => e.kind === 'chat' && e.text === `echo: ${text}`)
    if (!(await until(echoed))) return 'the established data channel stopped exchanging after the relay faults'
    if ([...(await cpp.events()).slice(cppStart), ...(await js.events()).slice(jsStart)].some(e => e.kind === 'peer-leave')) {
        return 'a peer left after the relay fault'
    }
    return null
})

// A relay that sends a client a text longer than 1 MiB, or resets its connection, just as the client writes: a C++ peer
// announces 233 ms after it subscribes, and the relay strikes just before. A text that arrives 1 to 2 ms before the
// write fails the read while Beast holds the write back, so the write completes after the connection ended; measured
// against the code before the fix, a strike at 232 ms crashed 7 peers in 8, at 231 ms 5 in 8, at 228 or 234 ms none. A
// reset fails the read and the write at once. One peer at a time, each in a room of its own, which the relay strikes by
// its topic: each loses the relay once, survives, and reconnects.
const strikes = [232, 231, 232, 233, 232, 231, 232, 233, 232, 232, 232, 232].map((delay, i) => ({
    delay,
    action: i < 8 ? 'too-long' : 'reset',
}))
const strikeScenario = cppKind => scenario('a_relay_that_ends_the_connection_during_a_write', cppKind, async open => {
    const peers = []
    for (const {delay, action} of strikes) {
        const room = freshRoom()
        left.relay.options.strike = {action, delay, topic: rootTopicOf(room)}
        const peer = await open('cpp', {room})
        peers.push(peer)
        const lost = await until(async () => (await peer.log()).includes(`lost ${left.relay.url}`))
        left.relay.options.strike = null
        if (deaths !== deathsBefore) return `a C++ peer died after the relay struck it (${action} at ${delay} ms)`
        if (!lost) return `${peer.label} did not lose the relay that struck it (${action} at ${delay} ms)`
    }
    const back = async () => (await Promise.all(peers.map(p => p.log()))).every(log => log.includes(`reconnected to ${left.relay.url}`))
    return (await until(back)) ? null : 'not every C++ peer reconnected to the relay'
})

// The driver's own check: a native peer that dies by a signal, as a crash does, is named at once, and its scenario does
// not wait for anything it would have done. The signal is expected here, so this scenario takes it off the peer's
// problems once it is named.
const killedScenario = cppKind => scenario('a_native_peer_killed_by_a_signal_is_named', cppKind, async open => {
    const native = await open('cpp')
    await idsOf(native)
    await native.abort()
    const named = await until(async () => native.problems.includes(`${native.label}: killed by SIGABRT`))
    native.problems.length = 0
    return named ? null : 'the driver did not name the signal that killed the native peer'
})

// Every scenario the options ask for, in turn: each of the twelve of 0.27.0's interop against the lane's C++ peer (the
// twelfth sees the relay's side, so it needs the local relay), then, on the local relay, adversarial peers and the C++
// peer's own relay robustness.
const scenarios = async () => {
    for (const cppKind of [...(browserPeer ? ['cpp'] : []), ...(nativePeer ? ['native'] : [])]) {
        await idsAndMessages(cppKind)
        await signatures(cppKind)
        await wideTransfers(cppKind)
        await passwords(cppKind)
        await importedIdentities(cppKind)
        await oneUserTwoCppPeers(cppKind)
        await maxUsers(cppKind)
        await requestsAndPings(cppKind)
        await leaves(cppKind)
        await forgedUserSignature(cppKind)
        await mesh(cppKind)
        if (!usePublic) {
            await relayConnectivity(cppKind)
            await malformedCandidateScenario(cppKind)
            await floodScenario(cppKind)
            await refusedRelayScenario(cppKind)
            await unusableIceScenario(cppKind)
            await oversizedRelayTextScenario(cppKind)
            if (cppKind === 'native') {
                await strikeScenario(cppKind)
                await killedScenario(cppKind)
            }
        }
    }
}

let status = 1
try {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
    left.server = server
    base = `http://127.0.0.1:${server.address().port}`
    await startChrome()
    left.relay = await startRelay()
    relayParams = usePublic ? {} : {relays: left.relay.url}
    await scenarios()
    if (!results.length) throw new Error(`no scenario matched ${JSON.stringify(scenarioFilter)}`)
    console.log(`relay: ${left.relay.stats.events} events forwarded, ${left.relay.stats.rejected} rejected`)
    const failed = results.filter(r => !r.ok).length
    console.log(failed ? `${failed} of ${results.length} scenarios failed` : `all ${results.length} scenarios passed`)
    status = failed ? 1 : 0
} catch (error) {
    console.log(`the run failed outside every scenario: ${error?.stack ?? error}`)
} finally {
    await cleanUp()
}
process.exit(status)
