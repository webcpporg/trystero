// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The interop test: C++ peers built with Emscripten (peer.cpp) and Trystero JS 0.26 peers in the same rooms, in
// headless Chrome, over a local Nostr relay (relay.mjs) or, with --public, Trystero's public relays.
//
//   node interop/run.mjs [--public] [--browser <interop_browser.js>] [--native <native_peer>] [--chrome <path>] [--verbose]
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
import {createHash} from 'node:crypto'
import {createInterface} from 'node:readline'
import {existsSync, mkdtempSync, readFileSync, readdirSync, rmSync} from 'node:fs'
import {createServer} from 'node:http'
import {homedir, tmpdir} from 'node:os'
import {basename, dirname, extname, join, normalize, resolve} from 'node:path'
import {fileURLToPath} from 'node:url'
import {startRelay} from './relay.mjs'

const here = dirname(fileURLToPath(import.meta.url))
const args = process.argv.slice(2)
const option = name => (args.includes(name) ? args[args.indexOf(name) + 1] : undefined)
const usePublic = args.includes('--public')
const verbose = args.includes('--verbose')
const browserPeer = option('--browser')
const nativePeer = option('--native')
if (!browserPeer && !nativePeer) throw new Error('pass --browser <interop_browser.js> or --native <native_peer>')
const buildDir = browserPeer && dirname(resolve(browserPeer))

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
    for (let i = 0; ; i++) {
        const active = join(left.profile, 'DevToolsActivePort')
        const port = existsSync(active) ? readFileSync(active, 'utf8').split('\n')[0] : ''
        if (/^[0-9]+$/.test(port)) {
            devtools = `http://127.0.0.1:${port}`
            try {
                await fetch(`${devtools}/json/version`)
                return
            } catch {}
        }
        if (i > 100 || exited(chrome)) throw new Error(`Chrome did not start; it said:\n${said}`)
        await new Promise(r => setTimeout(r, 100))
    }
}

const openPage = async (path, label) => {
    const target = await (await fetch(`${devtools}/json/new?about:blank`, {method: 'PUT'})).json()
    const ws = new WebSocket(target.webSocketDebuggerUrl)
    let id = 0
    const pending = new Map()
    const problems = []
    // Every call settles within ms, 10 s unless it says: a closing tab may never answer.
    const send = (method, params = {}, ms = 10_000) => new Promise(resolve => {
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
    const evaluate = async (expression, ms = 10_000) => {
        const answer = await send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true}, ms)
        if (answer === undefined) throw new Error(`${label}: ${expression} did not answer in ${ms / 1000} s`)
        return answer.result?.value
    }
    return {
        label,
        problems,
        events: async () => (await evaluate('window.events ?? []')) ?? [],
        log: async () => (await evaluate("document.getElementById('log')?.innerText ?? ''")) ?? '',
        leave: () => evaluate('window.leave()'),
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
        replace: async () => child.stdin.write('replace\n'),
        abort: async () => child.stdin.write('abort\n'),
        close: async () => {
            child.kill()
            await exit(child)
        },
    }
}

// ---- scenarios ---------------------------------------------------------------------------------------------------
const checksum = bytes => bytes.reduce((sum, b, i) => (sum + b * (i + 1)) >>> 0, 0)
const fromCpp = {size: 100_000, checksum: checksum(new Uint8Array(100_000).map((_, i) => (i * 13) % 256)), metadata: '{"name":"from-cpp.bin"}'}
const fromJs = {size: 100_000, checksum: checksum(new Uint8Array(100_000).map((_, i) => (i * 7) % 256)), metadata: '{"name":"from-js.bin"}'}

// Waits for `condition` for at most `ms`, and no longer once a peer of the scenario died by a signal: what it waits for
// may never come, and the death is the failure to name.
const until = async (condition, ms) => {
    const end = Date.now() + ms
    while (Date.now() < end && deaths === deathsBefore) {
        if (await condition()) return true
        await new Promise(r => setTimeout(r, 250))
    }
    return condition()
}

/** What a peer must have seen of a partner: the join, both messages, the echo of its own, the file, a ping, its sends. */
const exchangeComplete = (events, partnerIsCpp) => {
    const has = (kind, f = () => true) => events.some(e => e.kind === kind && f(e))
    const file = partnerIsCpp ? fromCpp : fromJs
    return has('peer-join') &&
        has('chat', e => e.text === (partnerIsCpp ? 'hello from C++' : 'hello from Trystero JS')) &&
        has('chat', e => e.text === (partnerIsCpp ? 'echo: hello from Trystero JS' : 'echo: hello from C++')) &&
        has('drink', e => e.value?.drink === (partnerIsCpp ? 'cafezinho' : 'negroni')) &&
        has('file', e => e.size === file.size && e.checksum === file.checksum && e.metadata === file.metadata) &&
        has('ping', e => e.error === '') &&
        ['chat', 'drink', 'file'].every(what => has('sent', e => e.what === what && e.error === ''))
}

const results = []
// The C++ peers' STUN server: the discard port of the loopback, where nothing answers, so that no peer asks a public
// one.
const ice = 'stun:127.0.0.1:9'

const scenario = async (name, body) => {
    const started = Date.now()
    deathsBefore = deaths
    const pages = []
    const open = async (kind, params) => {
        const all = {app, ice, ...params}
        const page = kind === 'native'
            ? openNative(all, `native#${pages.length}`)
            : await openPage(`/${kind}.html?${new URLSearchParams({...all, program: basename(browserPeer ?? '')})}`,
                `${kind}#${pages.length}`)
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
    results.push({name, ok: !failure})
    console.log(`${failure ? 'FAIL' : 'pass'}  ${name}  (${seconds} s)`)
    if (failure || verbose) {
        if (failure) console.log('  ' + failure)
        for (const p of pages) {
            console.log(`  ---- ${p.label} events: ${JSON.stringify(await p.events())}`)
            console.log((await p.log()).split('\n').map(l => '  | ' + l).join('\n'))
        }
    }
    for (const p of pages) await p.close()
}

const app = 'webcpp-trystero-interop'
const room = () => 'room-' + Math.random().toString(36).slice(2, 10)
let relayParams

const pairScenario = (name, {cppKind = 'cpp', cppSelf, password, leaver}) =>
    scenario(name, async open => {
        const params = {room: room(), ...relayParams, ...(password ? {password} : {})}
        const cpp = await open(cppKind, {...params, self: cppSelf})
        const js = await open('js', params)
        const ok = await until(async () => exchangeComplete(await cpp.events(), false) && exchangeComplete(await js.events(), true), 30_000)
        if (!ok) return 'the exchange did not complete in 30 s'
        const [stays, goes] = leaver === 'cpp' ? [js, cpp] : [cpp, js]
        await goes.leave()
        const left = await until(async () => (await stays.events()).some(e => e.kind === 'peer-leave') && (await goes.events()).some(e => e.kind === 'left'), 10_000)
        return left ? null : `the ${leaver} peer left but the other did not see it`
    })

// A Trystero JS peer that also sends the C++ peer malformed candidates (js.html, malformed-candidate-to): the C++
// peer drops at least one, which its log says, and the exchange completes as it does without them.
const malformedCandidateScenario = (name, cppKind) =>
    scenario(name, async open => {
        const cppSelf = '00000000000000000000'
        const params = {room: room(), ...relayParams}
        const cpp = await open(cppKind, {...params, self: cppSelf})
        const js = await open('js', {...params, 'malformed-candidate-to': cppSelf})
        const ok = await until(async () => exchangeComplete(await cpp.events(), false) && exchangeComplete(await js.events(), true), 30_000)
        if (!ok) return 'the exchange did not complete in 30 s'
        const jsSelf = (await js.events()).find(e => e.kind === 'joined')?.self
        return (await cpp.log()).includes(`dropped a candidate from ${jsSelf}`) ? null : 'no malformed candidate reached the C++ peer'
    })

// The C++ peer's relays begin with a URL no WebSocket can be made to: that relay reports a close, as an unreachable
// one does, which the C++ peer's log says, and the room goes on over the next one.
const refusedRelay = 'ftp://relay.invalid'
const refusedRelayScenario = (name, cppKind) =>
    scenario(name, async open => {
        const params = {room: room(), ...relayParams}
        const cpp = await open(cppKind, {...params, relays: `${refusedRelay},${relayParams.relays}`})
        const js = await open('js', params)
        const ok = await until(async () => exchangeComplete(await cpp.events(), false) && exchangeComplete(await js.events(), true), 30_000)
        if (!ok) return 'the exchange did not complete in 30 s'
        return (await cpp.log()).includes(`could not reach ${refusedRelay}`) ? null : 'the C++ peer did not report the refused relay'
    })

// A Trystero JS peer that also sends text messages on its data channel (js.html, text-messages), then a chat message:
// the C++ peer drops the text messages before reading them, and receives the chat message; no peer leaves.
const textMessagesScenario = (name, cppKind) =>
    scenario(name, async open => {
        const params = {room: room(), ...relayParams}
        const cpp = await open(cppKind, {...params, self: 'zzzzzzzzzzzzzzzzzzzz'})
        const js = await open('js', {...params, 'text-messages': '1'})
        const ok = await until(async () => exchangeComplete(await cpp.events(), false) && exchangeComplete(await js.events(), true) &&
            (await cpp.events()).some(e => e.kind === 'chat' && e.text === 'after text'), 30_000)
        if (!ok) return 'the exchange, or the chat message after the text messages, did not complete in 30 s'
        if (!(await js.events()).some(e => e.kind === 'text-sent' && e.channels > 0)) return 'no text message was sent'
        if ([...await cpp.events(), ...await js.events()].some(e => e.kind === 'peer-leave')) return 'a peer left'
        return (await cpp.log()).includes('invalid or incompatible room frame') ? 'the C++ peer took a text message for a frame' : null
    })

// The C++ peer is given a TURN server without credentials, which WebRTC refuses: the room drops it when it joins and
// reports it, and the exchange completes over the host candidates.
const unusableIceScenario = (name, cppKind) =>
    scenario(name, async open => {
        const params = {room: room(), ...relayParams}
        const cpp = await open(cppKind, {...params, ice: 'turn:127.0.0.1:9'})
        const js = await open('js', params)
        const ok = await until(async () => exchangeComplete(await cpp.events(), false) && exchangeComplete(await js.events(), true), 30_000)
        if (!ok) return 'the exchange did not complete in 30 s'
        const reported = (await cpp.events()).some(e => e.kind === 'error' && e.error === "the room's configuration is unusable")
        return reported ? null : 'the C++ peer did not report the unusable ICE server'
    })

// After a Trystero JS peer joined, it floods the C++ peer with what no key is needed for (js.html, window.flood and
// window.floodOffers): announcements from fresh ids above the C++ peer's, which a watcher (forger.html) answers with
// garbage in the flooders' names as soon as the C++ peer offers to them, and offers that no key sealed, from fresh ids.
// The C++ peer offers to at most the room's bound of them at once, makes no connection for a forged offer or answer,
// logs each id it refuses once, makes at most the room's bound of connections in the window (counted as Chromium
// counts them, every one made), reports the forged signals once, not once per id, and goes on with the peer that
// joined. The flood is judged by what happens once it begins: before it, the two peers meet as in any scenario, and a
// first meeting whose handshake times out (Trystero's 10 s) and meets again is that meeting's, not the flood's.
const floodCount = 300
const unconnectedBound = 32
const floodCallMs = 20_000
const floodScenario = (name, cppKind) =>
    scenario(name, async open => {
        const params = {room: room(), ...relayParams}
        const self = '00000000000000000000'
        const forger = await open('forger', {...params, target: self})
        if (!(await until(async () => (await forger.events()).some(e => e.kind === 'watching'), 10_000))) return 'the forger never watched'
        const opened = Date.now()
        const cpp = await open(cppKind, {...params, self})
        const js = await open('js', params)
        const ok = await until(async () => exchangeComplete(await cpp.events(), false) && exchangeComplete(await js.events(), true), 30_000)
        if (!ok) return 'the exchange did not complete in 30 s'
        const connections = async () => cppKind === 'native'
            ? (await cpp.events()).filter(e => e.kind === 'made-connection').length
            : await cpp.value('window.madePeerConnections')
        const before = {connections: await connections(), cpp: (await cpp.events()).length, js: (await js.events()).length}
        // Each call answers once its flood is sent, which on a busy machine takes the page more than 10 s.
        await js.value(`window.flood(${floodCount}, '0000000000')`, floodCallMs)
        await js.value(`window.floodOffers(${floodCount}, '1111111111', '${self}')`, floodCallMs)
        const ids = async (prefix, said) =>
            new Set([...(await cpp.log()).matchAll(new RegExp(`(?:${said}|refusing) (${prefix}\\w{10})`, 'g'))].map(m => m[1]))
        const floodIds = () => ids('0000000000', 'announcement from')
        const offerIds = () => ids('1111111111', 'could not open the offer from')
        const seen = await until(async () => (await floodIds()).size >= floodCount && (await offerIds()).size >= floodCount, 30_000)
        const log = await cpp.log()
        const offered = new Set([...log.matchAll(/announcement from (0000000000\w{10}): our id is lower, so we offer/g)].map(m => m[1]))
        const refused = new Set([...log.matchAll(/refusing ([01]{10}\w{10})/g)].map(m => m[1]))
        const forged = (await forger.events()).filter(e => e.kind === 'forged').length
        const made = await connections() - before.connections
        const peak = await cpp.value('window.peakPeerConnections')
        const reports = (await cpp.events()).filter(e => e.kind === 'error' && e.error === 'the peer uses another room password').length
        const facts = `the C++ peer logged ${(await floodIds()).size} of ${floodCount} announcing ids and ` +
            `${(await offerIds()).size} of ${floodCount} offering ids, offered to ${offered.size}, refused ${refused.size}, ` +
            `made ${made} connections in the flood, ${before.connections} before it` +
            (peak === undefined ? '' : `, held at most ${peak}`) +
            `, reported ${reports} wrong passwords; the watcher forged ${forged} answers`
        console.log(`  ${facts}`)
        if (!seen) return `${facts}; not every flooding signal was handled in 30 s`
        if (offered.size > unconnectedBound) return `${facts}; offered to more than ${unconnectedBound}`
        if (forged < 1) return `${facts}; the watcher forged no answer`
        // The flooders' offers alone: the peer that joined is joined before the flood begins.
        if (made > unconnectedBound) return `${facts}; more than ${unconnectedBound} connections in the flood`
        // The forged signals are reported once per 10 s, not once per fresh id: the first at once, the rest together.
        // So at most one report, and one more for each 10 s the C++ peer has been open, however slow the machine is.
        const reportBound = 1 + Math.floor((Date.now() - opened) / 10_000)
        if (reports > reportBound) return `${facts}; more than ${reportBound} reports of signals that did not open`
        const during = [...(await cpp.events()).slice(before.cpp), ...(await js.events()).slice(before.js)]
        if (during.some(e => e.kind === 'peer-leave')) return `${facts}; a peer left during the flood`
        return null
    })

// After the exchange, the relay sends every client a text longer than a relay's longest (1 MiB): the C++ peer closes
// that relay's connection rather than read it, says so, reconnects, and goes on with the peer it joined. Then the relay
// sends the text after every subscription: each reconnection is closed at once, and the C++ peer tries again within a
// bound that doubles every time, rather than every few seconds forever.
const oversizedRelayTextScenario = (name, cppKind) =>
    scenario(name, async open => {
        const params = {room: room(), ...relayParams}
        const cpp = await open(cppKind, {...params, self: '00000000000000000000'})
        const js = await open('js', params)
        const ok = await until(async () => exchangeComplete(await cpp.events(), false) && exchangeComplete(await js.events(), true), 30_000)
        if (!ok) return 'the exchange did not complete in 30 s'
        left.relay.broadcast(JSON.stringify(['NOTICE', 'x'.repeat(1024 * 1024 + 1)]))
        const lost = await until(async () => (await cpp.log()).includes(`lost ${left.relay.url}`), 10_000)
        if (!lost) return 'the C++ peer did not close the relay that sent the long text'
        const back = await until(async () => (await cpp.log()).includes(`reconnected to ${left.relay.url}`), 15_000)
        if (!back) return 'the C++ peer did not reconnect to the relay'
        const bounds = async () =>
            [...(await cpp.log()).matchAll(new RegExp(`lost ${left.relay.url}, next attempt within (\\d+) ms`, 'g'))].map(m => Number(m[1]))
        // The text closes the connection open now, and then each new one after its subscription.
        left.relay.options.afterSubscribe = JSON.stringify(['NOTICE', 'x'.repeat(1024 * 1024 + 1)])
        left.relay.broadcast(left.relay.options.afterSubscribe)
        const lostAgain = await until(async () => (await bounds()).length >= 3, 30_000)
        left.relay.options.afterSubscribe = null
        const seen = await bounds()
        console.log(`  the C++ peer lost the relay ${seen.length} times, each time to try again within ${seen.join(', ')} ms`)
        if (!lostAgain) return 'the C++ peer was not closed three times in 30 s'
        if (!seen.every((bound, i) => i === 0 || bound > seen[i - 1])) return `the bounds did not grow: ${seen.join(', ')}`
        if ([...await cpp.events(), ...await js.events()].some(e => e.kind === 'peer-leave')) return 'a peer left'
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
const topicOf = text => Array.from(createHash('sha1').update(text).digest(), b => b.toString(36)).join('')
const strikeScenario = (name, cppKind) =>
    scenario(name, async open => {
        const peers = []
        for (const {delay, action} of strikes) {
            const params = {room: room(), ...relayParams}
            left.relay.options.strike = {action, delay, topic: topicOf(`Trystero@${app}@${params.room}`)}
            const peer = await open(cppKind, params)
            peers.push(peer)
            const lost = await until(async () => (await peer.log()).includes(`lost ${left.relay.url}`), 5_000)
            left.relay.options.strike = null
            if (deaths !== deathsBefore) return `a C++ peer died after the relay struck it (${action} at ${delay} ms)`
            if (!lost) return `${peer.label} did not lose the relay that struck it (${action} at ${delay} ms)`
        }
        const back = await until(async () =>
            (await Promise.all(peers.map(p => p.log()))).every(log => log.includes(`reconnected to ${left.relay.url}`)), 10_000)
        return back ? null : 'not every C++ peer reconnected to the relay'
    })

// A watcher without the room key (forger.html) answers each offer of the C++ peer with garbage in the name of the peer
// the offer is for, before that peer's own answer (whose sends js.html holds 1 s): the C++ peer opens an answer before
// acting on it, so the forged one changes nothing, and the real one joins them.
const forgedAnswerScenario = (name, cppKind) =>
    scenario(name, async open => {
        const cppSelf = '00000000000000000000'
        const params = {room: room(), ...relayParams}
        const forger = await open('forger', {...params, target: cppSelf})
        if (!(await until(async () => (await forger.events()).some(e => e.kind === 'watching'), 10_000))) {
            return 'the forger did not start watching'
        }
        const cpp = await open(cppKind, {...params, self: cppSelf})
        const js = await open('js', {...params, 'hold-sends': '1'})
        const ok = await until(async () => exchangeComplete(await cpp.events(), false) && exchangeComplete(await js.events(), true), 30_000)
        const forged = (await forger.events()).filter(e => e.kind === 'forged').length
        if (!forged) return 'the forger never answered an offer, so nothing was tested'
        if (!ok) return `the exchange did not complete in 30 s; the forger answered ${forged} offers`
        return null
    })

// A watcher without the room key (forger.html, mode=replay) records the sealed answer, or the sealed offer, that a
// Trystero JS peer sends the C++ peer. Then the JS peer's connections drop (js.html's window.drop), as on a network
// drop, and Trystero meets the C++ peer again with the same id, its sends held 1 s (hold-sends), so that the watcher
// resends the recording in its name first: with each new offer of the C++ peer (side 'answer': the C++ peer's id is
// lower, so it offers), or as soon as the JS peer announces itself or is asked to offer (side 'offer': the C++ peer's
// id is higher). The recording opens with the room key, but the C++ peer used it before: it refuses it, and the JS
// peer joins it again.
const replayScenario = (name, cppKind, side) =>
    scenario(name, async open => {
        const cppSelf = side === 'answer' ? '00000000000000000000' : 'zzzzzzzzzzzzzzzzzzzz'
        const params = {room: room(), ...relayParams}
        const forger = await open('forger', {...params, target: cppSelf, mode: 'replay'})
        if (!(await until(async () => (await forger.events()).some(e => e.kind === 'watching'), 10_000))) {
            return 'the forger did not start watching'
        }
        const cpp = await open(cppKind, {...params, self: cppSelf})
        const js = await open('js', {...params, 'hold-sends': '1'})
        const ok = await until(async () => exchangeComplete(await cpp.events(), false) && exchangeComplete(await js.events(), true), 30_000)
        if (!ok) return 'the exchange did not complete in 30 s'
        if (!(await forger.events()).some(e => e.kind === 'recorded' && e.what === side)) return `the forger recorded no ${side}`
        await js.value('window.drop()')
        const back = await until(async () => (await cpp.events()).filter(e => e.kind === 'peer-join').length === 2, 30_000)
        const replayed = (await forger.events()).filter(e => e.kind === 'replayed' && e.what === side).length
        if (!replayed) return `the forger resent no ${side}, so nothing was tested`
        if (!back) return `the Trystero JS peer did not join again in 30 s; the forger resent ${replayed} ${side}s`
        return (await cpp.log()).includes(`refused the ${side} from`) ? null : `the C++ peer refused no ${side}`
    })

// A watcher without the room key (forger.html, mode=reflect) sends the C++ peer its own sealed texts back, in the Trystero
// JS peer's name: side 'answer' (the C++ peer's id is lower, so it offers), each offer of the C++ peer to the JS peer
// comes back at once as the JS peer's answer; side 'offer' (its id is higher), the watcher announces a decoy id above
// it, and the C++ peer's sealed offer to the decoy comes back as the JS peer's offer whenever the JS peer announces,
// ahead of its own (hold-sends). Each opens with the room key, but the C++ peer made it: it refuses it, and the two
// peers join.
const reflectScenario = (name, cppKind, side) =>
    scenario(name, async open => {
        // Offer side: the highest id below the decoy's, so that the Trystero JS peer's random id is below it and offers.
        const cppSelf = side === 'answer' ? '00000000000000000000' : 'zzzzzzzzzzzzzzzzzzzy'
        const params = {room: room(), ...relayParams}
        const decoy = side === 'offer' ? {decoy: 'zzzzzzzzzzzzzzzzzzzz'} : {}
        const forger = await open('forger', {...params, target: cppSelf, mode: 'reflect', ...decoy})
        if (!(await until(async () => (await forger.events()).some(e => e.kind === 'watching'), 10_000))) {
            return 'the forger did not start watching'
        }
        const cpp = await open(cppKind, {...params, self: cppSelf})
        if (side === 'offer' && !(await until(async () => (await forger.events()).some(e => e.kind === 'recorded'), 10_000))) {
            return 'the C++ peer made no offer to the decoy'
        }
        const js = await open('js', {...params, 'hold-sends': '1'})
        const ok = await until(async () => exchangeComplete(await cpp.events(), false) && exchangeComplete(await js.events(), true), 30_000)
        const reflected = (await forger.events()).filter(e => e.kind === 'reflected' && e.what === side).length
        if (!reflected) return `the forger reflected no ${side}, so nothing was tested`
        if (!ok) return `the exchange did not complete in 30 s; the forger reflected ${reflected} ${side}s`
        return (await cpp.log()).includes(`refused the ${side} from`) ? null : `the C++ peer refused no ${side}`
    })

// Every scenario the options ask for, in turn.
// A C++ peer and a Trystero JS page, each in two rooms of one app (room2 of peer.cpp, native_peer.cpp and js.html).
// Trystero keeps one connection per app and peer id, which the page's two rooms share, and drops a second one to the
// same id; a C++ room makes its own connections, so each C++ room needs a peer id of its own, which a room draws when
// its configuration names none. With them both rooms meet, as these check: the first room's whole exchange, and the
// second room's join, chat message and 100 KB file, both ways. Given one self_id for both rooms, the second room
// never joins: the page drops its connection and sends its frames over the first room's, which the C++ room drops.
const twoRoomsScenario = (name, cppKind) =>
    scenario(name, async open => {
        const params = {room: room(), room2: room(), ...relayParams}
        const cpp = await open(cppKind, params)
        const js = await open('js', params)
        const first = events => events.filter(e => e.room === undefined)
        const second = (events, file) => events.some(e => e.room === 'second' && e.kind === 'peer-join') &&
            events.some(e => e.room === 'second' && e.kind === 'chat') &&
            events.some(e => e.room === 'second' && e.kind === 'file' && e.size === file.size && e.checksum === file.checksum)
        const ok = await until(async () => {
            const c = await cpp.events(), j = await js.events()
            return exchangeComplete(first(c), false) && exchangeComplete(first(j), true) && second(c, fromJs) && second(j, fromCpp)
        }, 45_000)
        if (ok) return null
        const c = await cpp.events(), j = await js.events()
        return `the two rooms did not both complete in 45 s: first room ${exchangeComplete(first(c), false)} for C++, ` +
            `${exchangeComplete(first(j), true)} for JS; second room ${second(c, fromJs)} for C++, ${second(j, fromCpp)} for JS`
    })

const scenarios = async () => {
    if (browserPeer) {
        await pairScenario('C++ offers (lower id), Trystero JS answers; C++ leaves', {cppSelf: '00000000000000000000', leaver: 'cpp'})
        await pairScenario('Trystero JS offers, C++ answers (higher id), with a password; JS leaves', {cppSelf: 'zzzzzzzzzzzzzzzzzzzz', password: 'secret', leaver: 'js'})
        if (!usePublic) {
            await malformedCandidateScenario('A malformed ICE candidate from a peer does not stop C++', 'cpp')
            await refusedRelayScenario('A relay URL the browser refuses reports a close; C++ goes on', 'cpp')
            await textMessagesScenario('Text messages on the data channel do not reach or stop C++', 'cpp')
            await unusableIceScenario('An ICE server WebRTC refuses is reported and dropped; C++ goes on', 'cpp')
            await forgedAnswerScenario('A forged answer from a watcher without the key changes nothing; C++ joins', 'cpp')
            await replayScenario('A resent answer from a watcher without the key is refused; C++ joins again', 'cpp', 'answer')
            await replayScenario('A resent offer from a watcher without the key is refused; C++ joins again', 'cpp', 'offer')
            await reflectScenario('Its own offer sent back as an answer is refused; C++ joins', 'cpp', 'answer')
            await reflectScenario('Its own offer sent back as an offer is refused; C++ joins', 'cpp', 'offer')
            await floodScenario('A flood of announcements from fresh ids is bounded; C++ goes on', 'cpp')
            await oversizedRelayTextScenario('A relay text longer than 1 MiB is not read; C++ reconnects', 'cpp')
            await twoRoomsScenario('C++ and Trystero JS meet in two rooms of one app, each C++ room with its own id', 'cpp')
        }
    }

    if (nativePeer) {
        await pairScenario('Native C++ offers (lower id), Trystero JS answers; native leaves', {cppKind: 'native', cppSelf: '00000000000000000000', leaver: 'cpp'})
        await pairScenario('Trystero JS offers, native C++ answers (higher id), with a password; JS leaves', {cppKind: 'native', cppSelf: 'zzzzzzzzzzzzzzzzzzzz', password: 'secret', leaver: 'js'})
        if (!usePublic) {
            await malformedCandidateScenario('A malformed ICE candidate from a peer does not stop native C++', 'native')
            await refusedRelayScenario('A relay URL of another scheme reports a close; native C++ goes on', 'native')
            await textMessagesScenario('Text messages on the data channel do not reach or stop native C++', 'native')
            await unusableIceScenario('An ICE server WebRTC refuses is reported and dropped; native C++ goes on', 'native')
            await forgedAnswerScenario('A forged answer from a watcher without the key changes nothing; native C++ joins', 'native')
            await replayScenario('A resent answer from a watcher without the key is refused; native C++ joins again', 'native',
                'answer')
            await replayScenario('A resent offer from a watcher without the key is refused; native C++ joins again', 'native',
                'offer')
            await reflectScenario('Its own offer sent back as an answer is refused; native C++ joins', 'native', 'answer')
            await reflectScenario('Its own offer sent back as an offer is refused; native C++ joins', 'native', 'offer')
            await floodScenario('A flood of announcements from fresh ids is bounded; native C++ goes on', 'native')
            await oversizedRelayTextScenario('A relay text longer than 1 MiB is not read; native C++ reconnects', 'native')
            await twoRoomsScenario('Native C++ and Trystero JS meet in two rooms of one app, each C++ room with its own id',
                'native')
            await strikeScenario('A relay that ends the connection during a write: native C++ survives and reconnects', 'native')
            await scenario('A room moved over a joined native C++ one leaves it, as destroying it does', async open => {
                const params = {room: room(), ...relayParams}
                const native = await open('native', params)
                const js = await open('js', params)
                const ok = await until(async () => exchangeComplete(await native.events(), false) && exchangeComplete(await js.events(), true), 30_000)
                if (!ok) return 'the exchange did not complete in 30 s'
                await native.replace()
                const left = await until(async () => (await js.events()).some(e => e.kind === 'peer-leave') &&
                    (await native.events()).some(e => e.kind === 'pending' && e.error === 'room left'), 10_000)
                return left ? null : 'the replaced room did not say goodbye, or its pending receive did not complete with room left'
            })
            // The driver's own check: a native peer that dies by a signal, as a crash does, is named at once, and its
            // scenario does not wait for anything it would have done. The signal is expected here, so this scenario
            // takes it off the peer's problems once it is named.
            await scenario('A native C++ peer killed by a signal is named at once', async open => {
                const native = await open('native', {room: room(), ...relayParams})
                if (!(await until(async () => (await native.events()).some(e => e.kind === 'joined'), 10_000))) {
                    return 'the native peer did not start'
                }
                const started = Date.now()
                await native.abort()
                const named = await until(async () => native.problems.includes(`${native.label}: killed by SIGABRT`), 10_000)
                if (!named) return 'the driver did not name the signal that killed the native peer'
                const seconds = (Date.now() - started) / 1000
                native.problems.length = 0
                return seconds < 5 ? null : `the driver named the signal only after ${seconds} s`
            })
            await scenario('A wrong password keeps native C++ and Trystero JS apart', async open => {
                const params = {room: room(), ...relayParams}
                const native = await open('native', {...params, password: 'secret'})
                const js = await open('js', {...params, password: 'another'})
                await new Promise(r => setTimeout(r, 8000))
                const all = [...(await native.events()), ...(await js.events())]
                if (all.some(e => e.kind === 'peer-join')) return 'peers with different passwords joined'
                return all.some(e => e.kind === 'error') ? null : 'neither peer reported the wrong password'
            })
            await scenario('Two Trystero JS peers and a native C++ peer form a mesh', async open => {
                const params = {room: room(), ...relayParams}
                const peers = [await open('js', params), await open('native', params), await open('js', params)]
                const ok = await until(async () => {
                    for (const p of peers) {
                        const events = await p.events()
                        if (events.filter(e => e.kind === 'peer-join').length !== 2) return false
                        if (events.filter(e => e.kind === 'file').length !== 2) return false
                    }
                    return true
                }, 30_000)
                return ok ? null : 'not every peer joined both others and received both files in 30 s'
            })
        }
    }

    if (!usePublic && browserPeer) {
        await scenario('A wrong password keeps the peers apart', async open => {
            const params = {room: room(), ...relayParams}
            const cpp = await open('cpp', {...params, password: 'secret'})
            const js = await open('js', {...params, password: 'another'})
            await new Promise(r => setTimeout(r, 8000))
            const joined = [...(await cpp.events()), ...(await js.events())].filter(e => e.kind === 'peer-join')
            const errors = [...(await cpp.events()), ...(await js.events())].filter(e => e.kind === 'error')
            if (joined.length) return 'peers with different passwords joined'
            if (!errors.length) return 'neither peer reported the wrong password'
            return null
        })

        await scenario('Two Trystero JS peers and a C++ peer form a mesh', async open => {
            const params = {room: room(), ...relayParams}
            const peers = [await open('js', params), await open('cpp', params), await open('js', params)]
            const ok = await until(async () => {
                for (const p of peers) {
                    const events = await p.events()
                    if (events.filter(e => e.kind === 'peer-join').length !== 2) return false
                    if (events.filter(e => e.kind === 'file').length !== 2) return false
                }
                return true
            }, 30_000)
            return ok ? null : 'not every peer joined both others and received both files in 30 s'
        })
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
