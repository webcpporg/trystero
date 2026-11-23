// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Another page for a twin whose room must meet a peer. Trystero JS 0.27.0 has one endpoint key, so
// one peer id, per app and page (its module's client id), so two rooms of one twin's module would
// be the same peer; a C++ program makes a second app instead. The other peer runs in a worker thread, with module instances of its own,
// as a second page would:
//
//   import {openPage} from '../page.mjs'
//   const other = openPage(async ({joinRoom, RTCPeerConnection, data, post, next}) => { ... }, data)
//
// The function is the page's program, written in the twin and run in the worker from its source,
// so it reaches nothing of the twin's but what it is given: Trystero's joinRoom,
// werift's RTCPeerConnection (the WebRTC Trystero's README names for Node, as rtcPolyfill), the
// data given to openPage, which must be cloneable, post(message), which sends the twin a message,
// and next(), which resolves to the next message the twin sends it. The twin's side holds
// next() and post(message) the other way round, `done`, which settles when the page's function
// does, and close(), which ends the worker. What a page prints goes to the twin's standard output
// out of order with the twin's own lines: a page posts what the twin is to print.
import {isMainThread, parentPort, Worker, workerData} from 'node:worker_threads'

/** A queue of the messages one side receives, read by next(). */
const inbox = port => {
    const waiting = []
    const ready = []
    port.on('message', message => (waiting.length ? waiting.shift()(message) : ready.push(message)))
    return () => (ready.length ? Promise.resolve(ready.shift()) : new Promise(take => waiting.push(take)))
}

export const openPage = (program, data) => {
    const worker = new Worker(new URL(import.meta.url), {workerData: {source: program.toString(), data}})
    const next = inbox(worker)
    const done = new Promise((resolve, reject) => {
        worker.on('error', reject)
        worker.on('exit', resolve)
    })
    return {next, post: message => worker.postMessage(message), done, close: () => worker.terminate()}
}

if (!isMainThread) {
    const {joinRoom} = await import('trystero')
    const {RTCPeerConnection} = await import('werift')
    const program = (0, eval)(`(${workerData.source})`)
    await program({
        joinRoom,
        RTCPeerConnection,
        data: workerData.data,
        post: message => parentPort.postMessage(message),
        next: inbox(parentPort),
    })
    process.exit(0)
}
