// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Runs one twin over a Nostr relay of its own on 127.0.0.1, interop/relay.mjs, as webcpp.twins
// runs every twin of trystero's examples (Jamfile), the twin's path last:
//
//   node --conditions=development test/oracle/run_twin.mjs test/oracle/twins/<name>.mjs
//
// The relay's URL is in the environment, TRYSTERO_RELAY_URL, which the twin reads as its C++
// program reads it (example/run_difference.mjs starts the program's relay). The run ends once the
// twin's module has evaluated, its top-level awaits included, and what it printed has been
// written: Trystero JS keeps its relay sockets open after its rooms have left, as a page does,
// so the process would not end by itself. A twin that throws, or whose module fails to load,
// ends the run with status 1.
import {resolve} from 'node:path'
import {pathToFileURL} from 'node:url'
import {startRelay} from './interop/relay.mjs'

const twin = process.argv.at(-1)
const relay = await startRelay()
process.env.TRYSTERO_RELAY_URL = relay.url
let status = 0
try {
    await import(pathToFileURL(resolve(twin)).href)
} catch (error) {
    console.error(error)
    status = 1
}
// Every line the twin printed is written before the process ends: a pipe's writes are
// asynchronous on macOS, and a write's callback runs once those before it are done.
await new Promise(done => process.stdout.write('', done))
process.exit(status)
