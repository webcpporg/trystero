// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Runs one example over a Nostr relay of its own on 127.0.0.1, test/oracle/interop/relay.mjs, and
// checks what it prints: the driver of the quick start and of each diff_<topic> of the appendix
// "Differences", which the Jamfile declares with webcpp.drive, which gives it the native program
// last:
//
//   node --conditions=development example/run_over_relay.mjs <program>
//
// The relay's URL is in the program's environment, TRYSTERO_RELAY_URL, as in its twin's
// (test/oracle/run_twin.mjs). The run passes when the program exits with 0 having printed
// exactly <name>.expected beside this script, every carriage return removed, as webcpp.example
// compares an example's output; otherwise it names the status, or shows the difference and what
// the program said on its standard error. webcpp.drive bounds the run.
import {spawn} from 'node:child_process'
import {readFileSync} from 'node:fs'
import {basename, dirname, extname, join} from 'node:path'
import {fileURLToPath} from 'node:url'
import {startRelay} from '../test/oracle/interop/relay.mjs'

const program = process.argv.at(-1)
const name = basename(program, extname(program) === '.exe' ? '.exe' : '')
const expected = readFileSync(join(dirname(fileURLToPath(import.meta.url)), `${name}.expected`), 'utf8')
    .replaceAll('\r', '')

const relay = await startRelay()
const child = spawn(program, [], {
    env: {...process.env, TRYSTERO_RELAY_URL: relay.url},
    stdio: ['ignore', 'pipe', 'pipe'],
})
const output = []
const errors = []
child.stdout.on('data', data => output.push(data))
child.stderr.on('data', data => errors.push(data))
const [status, signal] = await new Promise((done, fail) => {
    child.on('error', fail)
    child.on('close', (code, killed) => done([code, killed]))
})
relay.close()

const printed = Buffer.concat(output).toString('utf8').replaceAll('\r', '')
const said = Buffer.concat(errors).toString('utf8')
const lines = text => text.split('\n').map(line => `  ${line}`).join('\n')
if (status !== 0) {
    console.error(`${name} ended with ${signal ? `the signal ${signal}` : `status ${status}`}, having printed:\n` +
        `${lines(printed)}\nand said:\n${lines(said)}`)
    process.exit(1)
}
if (printed !== expected) {
    console.error(`${name} printed:\n${lines(printed)}\nwhere ${name}.expected holds:\n${lines(expected)}\n` +
        `and said:\n${lines(said)}`)
    process.exit(1)
}
console.log(`${name}: printed ${name}.expected over ${relay.url}`)
