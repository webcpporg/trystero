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
// cache; elsewhere CHROME is required.
import {spawn} from 'node:child_process'
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
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
const base = `http://127.0.0.1:${server.address().port}`

// ---- Chrome over the DevTools protocol -----------------------------------------------------------------------------
const profile = mkdtempSync(join(tmpdir(), 'trystero-interop-'))
const chrome = spawn(findChrome(), [
    // The port Chrome chooses, which it writes into the profile's DevToolsActivePort: no two runs collide.
    '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--no-first-run',
    // Host candidates as IP addresses: headless Chrome cannot resolve its own mDNS names.
    '--disable-features=WebRtcHideLocalIpsWithMdns',
    // Chrome's sandbox refuses to start as root, as in a container.
    ...(process.getuid?.() === 0 ? ['--no-sandbox'] : []),
    'about:blank',
], {stdio: 'ignore'})
let devtools
for (let i = 0; ; i++) {
    const active = join(profile, 'DevToolsActivePort')
    const port = existsSync(active) ? readFileSync(active, 'utf8').split('\n')[0] : ''
    if (/^[0-9]+$/.test(port)) {
        devtools = `http://127.0.0.1:${port}`
        try {
            await fetch(`${devtools}/json/version`)
            break
        } catch {}
    }
    if (i > 100) throw new Error('Chrome did not start')
    await new Promise(r => setTimeout(r, 100))
}

const openPage = async (path, label) => {
    const target = await (await fetch(`${devtools}/json/new?about:blank`, {method: 'PUT'})).json()
    const ws = new WebSocket(target.webSocketDebuggerUrl)
    let id = 0
    const pending = new Map()
    const problems = []
    // Every call settles within 10 s: a closing tab may never answer.
    const send = (method, params = {}) => new Promise(resolve => {
        const i = ++id
        const timer = setTimeout(() => {
            pending.delete(i)
            resolve(undefined)
        }, 10_000)
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
    const evaluate = async expression => (await send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true})).result?.value
    return {
        label,
        problems,
        events: async () => (await evaluate('window.events ?? []')) ?? [],
        log: async () => (await evaluate("document.getElementById('log')?.innerText ?? ''")) ?? '',
        leave: () => evaluate('window.leave()'),
        close: async () => {
            ws.close()
            await fetch(`${devtools}/json/close/${target.id}`).catch(() => {})
        },
    }
}

// ---- the native peer: a process with the interface of a page -------------------------------------------------------
const openNative = (params, label) => {
    const args = Object.entries(params).flatMap(([k, v]) => [`--${k}`, String(v)])
    const child = spawn(nativePeer, args, {stdio: ['pipe', 'pipe', 'pipe']})
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
    child.on('exit', code => code && code !== 143 && code !== null && problems.push(`${label}: exited with ${code}`))
    return {
        label,
        problems,
        events: async () => events,
        log: async () => lines.join('\n'),
        leave: async () => child.stdin.write('leave\n'),
        close: async () => {
            child.kill()
            await new Promise(resolve => (child.exitCode !== null ? resolve() : child.on('exit', resolve)))
        },
    }
}

// ---- scenarios ---------------------------------------------------------------------------------------------------
const checksum = bytes => bytes.reduce((sum, b, i) => (sum + b * (i + 1)) >>> 0, 0)
const fromCpp = {size: 100_000, checksum: checksum(new Uint8Array(100_000).map((_, i) => (i * 13) % 256)), metadata: '{"name":"from-cpp.bin"}'}
const fromJs = {size: 100_000, checksum: checksum(new Uint8Array(100_000).map((_, i) => (i * 7) % 256)), metadata: '{"name":"from-js.bin"}'}

const until = async (condition, ms) => {
    const end = Date.now() + ms
    while (Date.now() < end) {
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
// The C++ peers' STUN server: the discard port of the loopback, where nothing answers, so that no peer asks a public one.
const ice = 'stun:127.0.0.1:9'

const scenario = async (name, body) => {
    const started = Date.now()
    const pages = []
    const open = async (kind, params) => {
        const all = {app: 'webcpp-trystero-interop', ice, ...params}
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
    const problems = pages.flatMap(p => p.problems)
    if (!failure && problems.length) failure = problems.join('\n')
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

const relay = await startRelay()
const room = () => 'room-' + Math.random().toString(36).slice(2, 10)
const relayParams = usePublic ? {} : {relays: relay.url}

const pairScenario = (name, {cppKind = 'cpp', cppSelf, password, leaver}) =>
    scenario(name, async open => {
        const params = {room: room(), ...relayParams, ...(password ? {password} : {})}
        const cpp = await open(cppKind, {...params, self: cppSelf})
        const js = await open('js', params)
        const ok = await until(async () => exchangeComplete(await cpp.events(), false) && exchangeComplete(await js.events(), true), 60_000)
        if (!ok) return 'the exchange did not complete in 60 s'
        const [stays, goes] = leaver === 'cpp' ? [js, cpp] : [cpp, js]
        await goes.leave()
        const left = await until(async () => (await stays.events()).some(e => e.kind === 'peer-leave') && (await goes.events()).some(e => e.kind === 'left'), 10_000)
        return left ? null : `the ${leaver} peer left but the other did not see it`
    })

if (browserPeer) {
await pairScenario('C++ offers (lower id), Trystero JS answers; C++ leaves', {cppSelf: '00000000000000000000', leaver: 'cpp'})
await pairScenario('Trystero JS offers, C++ answers (higher id), with a password; JS leaves', {cppSelf: 'zzzzzzzzzzzzzzzzzzzz', password: 'secret', leaver: 'js'})
}

if (nativePeer) {
    await pairScenario('Native C++ offers (lower id), Trystero JS answers; native leaves', {cppKind: 'native', cppSelf: '00000000000000000000', leaver: 'cpp'})
    await pairScenario('Trystero JS offers, native C++ answers (higher id), with a password; JS leaves', {cppKind: 'native', cppSelf: 'zzzzzzzzzzzzzzzzzzzz', password: 'secret', leaver: 'js'})
    if (!usePublic) {
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
            }, 60_000)
            return ok ? null : 'not every peer joined both others and received both files in 60 s'
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
        }, 60_000)
        return ok ? null : 'not every peer joined both others and received both files in 60 s'
    })
}

console.log(`relay: ${relay.stats.events} events forwarded, ${relay.stats.rejected} rejected`)
relay.close()
server.close()
chrome.kill()
rmSync(profile, {recursive: true, force: true})
const failed = results.filter(r => !r.ok).length
console.log(failed ? `${failed} of ${results.length} scenarios failed` : `all ${results.length} scenarios passed`)
process.exit(failed ? 1 : 0)
