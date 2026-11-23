# trystero

trystero is a header-only port of [Trystero](https://github.com/dmotz/trystero)
0.27.0's serverless WebRTC rooms for C++20, one of the libraries of
[webcpp](https://github.com/webcpporg/webcpp). Peers find each other over
public Nostr relays and exchange messages directly, over WebRTC data channels.
The wire is Trystero's: once the port is whole, a room joined from C++ and the
same room joined from Trystero's JavaScript are one room.

**On the branch `port/trystero-0.27`.** This branch ports the library from
Trystero 0.26.0 to 0.27.0 (upstream commit `8d3f57e`): 0.27.0's topics, room
keys, sealing, ids, relays and action wire, each value proven against vectors
recorded from 0.27.0's own code; 0.27.0's identities, signed with ECDSA P-256,
and its handshake; and 0.27.0's room API, with listeners that return a
subscription, users, room errors, request actions and the relays'
connectivity. Its rooms meet Trystero 0.27.0's: the interop lane meets 0.27.0
pages in headless Chrome with the browser's peer and the native one. The
branch reaches `main` once, whole.

A room runs on two targets, native and emscripten, through one platform
backend:

| Header | Target | What it holds |
| --- | --- | --- |
| `<webcpp/trystero/platform/backend.hpp>` | native and emscripten | `platform::backend`: webcpp::webrtc for WebRTC, natively over libdatachannel and in a page over its `RTCPeerConnection`; webcpp::beast for the relays' WebSockets, natively Boost.Beast's and in a page its `WebSocket`, their URLs read with Boost.URL; webcpp::asio's timers; webcpp::crypto's `random_device` for randomness; and the PBKDF2 of a room's password, webcpp::hash's, natively on a thread of its own and in a page through `crypto.subtle` |
| `<webcpp/trystero/platform/page.hpp>` | emscripten | `platform::page_call` and `platform::js_string`, what a page's program uses beside the backend |

`<webcpp/trystero.hpp>` is the rooms themselves, which build on both targets
and leave the platform backend out. The backend's WebRTC is
[webcpp::webrtc](https://github.com/webcpporg/webrtc)'s, the same peer
connection and data channel on both targets, and trystero's Asio is
[webcpp::asio](https://github.com/webcpporg/asio), Boost.Asio itself
natively: a program runs its `io_context` from `main` on both targets, in
the browser under a work guard, where `run` suspends into the page's event
loop and the page's calls into C++ only post to it. Every name is in the
namespace `webcpp::trystero`. Messages, binary data, requests and pings are
ported; audio and video tracks are not yet. A room seals and opens its
signals in the call, with webcpp::crypto's AES-256-GCM, and hashes with
webcpp::hash: the backend has no sealer of its own.

trystero raises no exception of its own: an operation that can fail completes
with an error code. Every part of it, the platform backend included, compiles
and works without exceptions: webcpp::webrtc calls libdatachannel's C API,
where none crosses.

An `app` represents this program in one application: one peer id, drawn when
the app is made, and one connection per remote peer, shared by its rooms, with
one immutable password for the app, from which each room derives its keys off
the room's thread. Different passwords use separate apps with distinct peer
ids. Construct it with the executor, backend and moved `app_config`, then call
`join` and test its `boost::system::result<room>`. 0.27.0 removed the custom
peer id and the handshake timeout, and so did the branch: `app_config` has no
`self_id`, and `room_options` no `handshake_timeout`. A room's `on_*`
functions add listeners and return a `subscription`; `on_error` reports a
`room_error` with 0.27.0's code; a send to a target not connected completes
with `error::not_connected`. Rooms have no password override. A new password
requires a new app. Two peers in one process use two apps; rooms that must not
be linked use one app each.

`app_config::memory` receives the app's state, shared connections, received
frames and every log sentence. `room_options::memory` receives a room's
state, bindings, actions, relays, handlers and JSON; null uses the app's
resource. A received owning binary payload moves when its message buffer
and room resources match, and is copied once otherwise; received pmr text
is always copied once. Completion handlers use their own associated allocator,
which `webcpp::asio::bind_allocator` sets. The app's resource must outlive
its handle, all its rooms, retained values and pending operations; each
room's resource must outlive its values and operations too. Declare both
before the io_context and drain or destroy it first; in a browser keep them
for the page's life. Pools suit long-lived apps and rooms. webcpp::webrtc's
connections and channels use the app's resource; libdatachannel, OpenSSL,
natively the cipher context of each seal and open and the PBKDF2 thread, and
the browser retain their own internal allocations.

An app's password, `app_config::password`, seals the signals of each of its
rooms, as a Trystero JS room's password seals its own: a peer of another
password reads none of them, and the room reports its sealed offer or answer
as `decrypt_failed`. Each room negotiates its own connections with
Trystero 0.27.0's signed handshake, as a Trystero JS room does.

## Building inside the superproject

trystero is developed inside the webcpp superproject, as a Boost library is
developed inside Boost. It builds on webcpp::asio, webcpp::hash,
webcpp::crypto, webcpp::webrtc and, for the relays, webcpp::beast, five
libraries of the superproject; its TLS and WebRTC dependencies are built from
pinned sources, and so are webcpp::crypto's libsecp256k1 and BearSSL:

| Library | Needed by | Found where |
| --- | --- | --- |
| webcpp::hash | every room: the SHA-256 of its topics and keys, HMAC, HKDF, and the PBKDF2 of a password's master | `/webcpp/hash//hash` |
| webcpp::crypto | every room: the AES-256-GCM of its signals (natively OpenSSL's, on emscripten BearSSL's), its randomness, its secrets, and the BIP-340 signatures of its Nostr events, over libsecp256k1 0.8.0 with its `schnorrsig` and `extrakeys` modules | `/webcpp/crypto//crypto`, `//gcm-default` and `//secp256k1-default`; libsecp256k1 from `third_party/libs/secp256k1`, BearSSL from `third_party/libs/bearssl` |
| webcpp::asio | every room | `/webcpp/asio//asio` |
| webcpp::webrtc | the platform backend's WebRTC | `/webcpp/webrtc//webrtc` |
| webcpp::beast | the platform backend's relays: their WebSockets, and natively their TLS | natively `/webcpp/beast//tls`, Boost.Beast over the source-built OpenSSL; in a page `/webcpp/beast//browser`, over the page's `WebSocket` |
| Boost.URL | the platform backend's relays: their URLs | `/webcpp//boost_url`, built from Boost's sources once per variant |
| libdatachannel 0.24.6 | the platform backend natively, through webcpp::webrtc | `/webcpp/third_party//libdatachannel`, from pinned source submodules |
| OpenSSL 3.5.9 | natively: the platform backend's WebRTC and TLS, and webcpp::crypto's AES-GCM | `/webcpp/third_party//openssl`, also used by Asio and Beast |

The tables of libsecp256k1, which webcpp::crypto links, follow `webcpp-secp256k1-tables`: `auto` is window 15 and
comb 11x6 natively (a larger program, faster signing and verification) and window 2 and
comb 2x5 on emscripten (the same signing, verification a little slower than window 4, a
smaller program). A program that wants otherwise asks for `<webcpp-secp256k1-tables>small`
or `large`. With the pinned source submodules initialized,

    b2 libs/trystero/test libs/trystero/example
    b2 toolset=emscripten libs/trystero/test libs/trystero/example

build the tests natively and on emscripten, and the chat's program on
emscripten, with the emscripten toolset that `user-config.jam` registers
against emsdk; the quick start and the programs of the differences run in
the lane `relayed` (below). A b2 program uses the rooms and the platform
backend through `/webcpp/trystero//platform`, on both targets, as the
examples' Jamfiles do.
The target adds `/webcpp/trystero//trystero`, the rooms alone: trystero's
headers, Boost's, webcpp::asio, which links embind on emscripten,
webcpp::hash, webcpp::crypto with libsecp256k1, and webcpp::document with
Boost.JSON's and Boost.Charconv's definitions, compiled once per variant. Beside
them it adds webcpp::webrtc and Boost.URL on both targets; natively
libdatachannel and OpenSSL, through webcpp::webrtc, webcpp::beast's `//tls`
and webcpp::asio's `//tls`, whose embedded authorities a program may trust in
place of the system's; and in a page webcpp::beast's `//browser`, with no
OpenSSL. A native program declares `<threading>multi` itself; a program on
emscripten is built in webcpp::asio's suspension variant,
`webcpp-suspend=jspi` by default or `asyncify`. A project whose rooms run
over a backend of its own, as the tests' memory relays do, uses
`/webcpp/trystero//trystero` alone.

## The examples

| Example | What it is | Target |
| --- | --- | --- |
| `example/quick_start.cpp` | the quick start: two peers of one program meet in the room `lobby`, greet each other, print what they receive, and leave | native |
| `example/diff_*.cpp` | the programs of the page's appendix "Differences", each showing one difference from Trystero | native |
| `example/chat/` | a chat page whose room is C++ compiled to WebAssembly | emscripten |

The quick start and the programs of the differences run in the lane
`relayed`, each over a Nostr relay of its own on 127.0.0.1, whose URL
`example/run_over_relay.mjs` gives the program in `TRYSTERO_RELAY_URL`; each
passes when it ends by itself having printed its `.expected`. Each has its
twin, the same program on Trystero JS, under `test/oracle/twins/`, still on
Trystero 0.26.0 on the branch. The chat needs a browser, so the build links
its program and never runs it.

The quick start, whole: two apps of one program, `alice` and `bob`, each a
peer with an id it draws, join the room `lobby` of the app
`webcpp-trystero-quick-start`; alice greets bob when he joins, bob answers
from a coroutine, each prints what it received, after the other's name, and
both leave, which ends the program. Without `TRYSTERO_RELAY_URL` it meets
over Trystero's public relays and STUN servers, as a program that names
none does.

<!-- include::example/quick_start.cpp[tag=program] -->
```cpp
#include <webcpp/trystero.hpp>
#include <webcpp/trystero/platform/backend.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;

namespace {

/** What one peer's app is made with: the relay `relay` names, or the public ones. Each app draws
 * its own id, 20 random characters. */
trystero::app_config config_for(const char* relay) {
    trystero::app_config config;
    config.app_id = "webcpp-trystero-quick-start";
    if (relay != nullptr) {
        // The peers stay on this machine: the relay given, and a STUN server where nothing answers
        // in place of Trystero's public ones, beside a backend bound to the loopback.
        config.relay_urls = {relay};
        config.ice_servers = {{.urls = {"stun:127.0.0.1:9"}, .username = {}, .credential = {}}};
    }
    // Without a relay, both lists stay empty: Trystero's defaults, as a user's program has them,
    // the public relays its app id selects and Trystero's public STUN servers.
    return config;
}


/** bob's part: waits for a message, prints it after alice's name, answers its sender, and leaves
 * the room. */
asio::awaitable<void> answer(trystero::room& lobby, trystero::action<std::string>& chat) {
    // as_tuple hands the error over as a value: a room left first ends the wait without throwing.
    auto [ec, text, info] = co_await chat.async_receive(asio::as_tuple(asio::use_awaitable));
    if (ec) {
        co_return;
    }
    // The sender's id is drawn on each run: its name prints the same every time.
    std::cout << "alice: " << text << '\n';
    auto [failed] = co_await chat.async_send("hello back", {.targets = {info.from}, .metadata = {}},
                                             asio::as_tuple(asio::use_awaitable));
    if (failed) {
        std::cerr << "not delivered: " << failed.message() << '\n';
    }
    co_await lobby.async_leave(asio::as_tuple(asio::use_awaitable));
}


}  // namespace

int main() {
    // The relay the lane starts on 127.0.0.1, or none: Trystero's public relays.
    const char* const relay = std::getenv("TRYSTERO_RELAY_URL");
    asio::io_context io;
    // One backend for both apps, whose connections stay on the loopback beside a local relay.
    trystero::platform::backend_config local;
    if (relay != nullptr) {
        local.bind_address = "127.0.0.1";
    }
    trystero::platform::backend backend(io.get_executor(), local);

    // Two apps, two peers, each with an id of its own, in the same room.
    trystero::app alice(io.get_executor(), backend, config_for(relay));
    trystero::app bob(io.get_executor(), backend, config_for(relay));
    auto alice_lobby = alice.join("lobby");
    auto bob_lobby = bob.join("lobby");
    if (!alice_lobby || !bob_lobby) {
        std::cerr << "not joined\n";
        return 1;
    }
    auto alice_chat = alice_lobby->make_action<std::string>("chat");
    auto bob_chat = bob_lobby->make_action<std::string>("chat");

    // alice greets each peer once its handshake completes, prints the answer, and leaves.
    alice_lobby->on_peer_join([&](const trystero::peer_event& joined) {
        alice_chat.async_send("hello",
                              {.targets = {trystero::peer_id(joined.peer_id)}, .metadata = {}},
                              [](boost::system::error_code ec) {
                                  if (ec) {
                                      std::cerr << "not delivered: " << ec.message() << '\n';
                                  }
                              });
    });
    alice_chat.on_message([&](const std::string& text, const trystero::message_info& /*info*/) {
        std::cout << "bob: " << text << '\n';
        alice_lobby->async_leave([](boost::system::error_code) {});
    });
    asio::co_spawn(io, answer(*bob_lobby, bob_chat), asio::detached);

    io.run();  // lint-run: the program's network I/O; libdatachannel's threads post to it
}
```

It prints bob's line, then alice's:

<!-- include::example/quick_start.expected[] -->
```
alice: hello
bob: hello back
```

`example/Jamfile` declares it natively, built with `<threading>multi`, and
run by its driver, `run_over_relay.mjs`, over a relay of its own:

<!-- include::example/Jamfile[tag=quick-start] -->
```
webcpp.drive quick_start : quick_start.cpp
  : <library>/webcpp/trystero//platform <threading>multi : run_over_relay.mjs : : native ;
```

`b2 toolset=emscripten libs/trystero/example/chat//site` copies the chat page
and its program into `site/` beside the program, under the build directory
and out of the tree, one per variant, and prints that directory. On another
toolset it stops, saying that the page is built on emscripten alone.

## From an empty directory to the quick start and the chat

These steps go from an empty directory to the quick start's two peers
meeting over the public Nostr relays, and to two tabs of the chat page
talking to each other there, on macOS with Homebrew. Every command runs in
the workspace directory of step 2 unless it says otherwise.

### 1. Install the tools

git, CMake 3.21 or newer, clang (the Xcode command line tools), Python 3,
Node, Perl, Make, Boost 1.92 and b2:

```sh
brew install cmake boost boost-build node
```

### 2. Create a workspace

```sh
mkdir trystero-workspace && cd trystero-workspace
```

### 3. Install Emscripten 6.0.11

webcpp pins Emscripten 6.0.11, installed through emsdk:

```sh
git clone https://github.com/emscripten-core/emsdk.git
./emsdk/emsdk install 6.0.11
./emsdk/emsdk activate 6.0.11
```

### 4. Get webcpp

```sh
git clone --recurse-submodules https://github.com/webcpporg/webcpp.git
```

### 5. Initialize the native dependencies

The recursive checkout already includes pinned OpenSSL, libdatachannel,
libjuice, usrsctp, plog and libsrtp sources. For an existing checkout:

```sh
git -C webcpp submodule update --init --recursive third_party/libs/openssl third_party/libs/libdatachannel third_party/libs/secp256k1 third_party/libs/bearssl
```

B2 builds them with the selected compiler, runtime and C++ ABI when the
platform backend is first needed natively. Each upstream checkout remains
unchanged. The generated headers and static archives are kept together by
build configuration. Asio, Beast and Trystero share one OpenSSL provider in a
combined application; no system OpenSSL or installed libdatachannel is used.
Native Trystero targets declare `<threading>multi` because WebRTC runs
worker threads. The supplied examples and tests declare it already.

### 6. Tell b2 its compilers and Boost

```sh
cat > user-config.jam <<EOF
using clang ;
using boost : 1.92 : <include>$(brew --prefix boost)/include <library>$(brew --prefix boost)/lib ;
using emscripten : : $PWD/emsdk/upstream/emscripten/em++
  : <archiver>$PWD/emsdk/upstream/emscripten/emar ;
EOF
```

### 7. Build the quick start and the chat page

```sh
cd webcpp
b2 --user-config=../user-config.jam libs/trystero/example//quick_start
b2 --user-config=../user-config.jam toolset=emscripten libs/trystero/example/chat//site
```

The first command builds the quick start and runs it once over a Nostr relay
of its own on 127.0.0.1, which Node runs with the packages
`libs/trystero/test/oracle` pins, installed by npm at the first run; it
passes when the program prints its `.expected`. The first build also
compiles OpenSSL, libdatachannel, and Boost.JSON's and Boost.Charconv's
definitions, which webcpp::document uses, and the first emscripten build
Emscripten's system libraries, which takes a few minutes.
The second command ends by printing the chat site's directory,
`bin/libs/trystero/example/chat/emscripten-6.0.11/debug/address-model-32/cxxstd-20-iso/link-static/target-os-none/site`,
which holds `index.html`, `chat-program.js` and its WebAssembly,
`chat-program-60-d-jspi.wasm`, named after webcpp::asio's suspension
variant: JSPI, which Chrome 137 and later run; `webcpp-suspend=asyncify`
builds one that every browser runs.

### 8. Run them

In a terminal, from `webcpp`, run the quick start. Without
`TRYSTERO_RELAY_URL` its two peers meet over Trystero's public relays;
within a few seconds it prints what each received, and ends:

```sh
`find bin/libs/trystero/example/quick_start.test -name quick_start -type f`
```

<!-- include::example/quick_start.expected[] -->
```
alice: hello
bob: hello back
```

Then serve the chat page:

```sh
python3 -m http.server 8000 -d `find bin/libs/trystero/example/chat -name site -type d`
```

Open <http://localhost:8000> in two tabs, and press Join in each, with the
page's app, `webcpp-trystero-chat`, and room, `lobby`. Within a few seconds
each tab shows the other's id, its first six characters, "joined", and
counts it as a peer; what you send from one tab appears in the other, under
the sender's id. Every tab that joins the same app and room joins the same
chat. On `main` so does any Trystero JS 0.26.0 page; on the branch any
Trystero 0.27.0 page does.

A native peer and a page meet too. The lane `interop` of `test/oracle` meets
the native peer, and the browser's, with Trystero JS pages in headless Chrome
(The tests, below).

### When something goes wrong

- **b2 prints `node: bad option: --experimental-wasm-threads`** when it
  configures the emscripten toolset: b2's toolset probes node with a flag that
  recent Node removed. It is harmless.
- **The page loads but never joins a room with a password**: it must be
  opened at `localhost` (or `127.0.0.1`), not at a network address or from a
  file. The browser gives the PBKDF2 a password room needs only to secure
  pages, and `localhost` counts as one.
- **No peer appears**: both sides must use exactly the same app and room. The
  peers meet over public relays, so a network that blocks WebSockets to them
  keeps them apart.

## The tests

`test/` compiles every public header alone, on each target that builds it:
the rooms, the protocol and their details, the platform backend and the
backend's WebRTC on both targets, natively against webcpp::webrtc's
libdatachannel and OpenSSL, and `platform/page.hpp` on emscripten. It also
checks that `<webcpp/trystero/config.hpp>` defines
`WEBCPP_TRYSTERO_NO_EXCEPTIONS` exactly when webcpp has no exceptions, and
when a program defines `WEBCPP_NO_EXCEPTIONS`.
`test/support/memory_backend.hpp` is a backend in memory, whose relays,
peer connections and data channels run whole rooms in one process.

`crypto_layer` checks the protocol of 0.27.0 against
`test/fixtures/expected/trystero-0.27.0.json`, the vectors
`test/oracle/record_0_27.mjs` records from 0.27.0's own code
(`node test/oracle/record_0_27.mjs <checkout>`, against a checkout of
`8d3f57e`).

Two lanes of trystero's own, declared in `test/oracle/Jamfile`, check it
against Trystero itself, pinned at 0.27.0 by `test/oracle/package.json` and
`package-lock.json`, with Node:

    b2 libs/trystero/test/oracle//oracle
    b2 libs/trystero/test/oracle//interop
    b2 toolset=emscripten libs/trystero/test/oracle//interop

`oracle` runs Trystero's own functions over the inputs of
`test/fixtures/cases/protocol.json` and checks that they still write
`test/fixtures/oracle/expected.json`, the values the protocol tests
reproduce; `b2 libs/trystero/test/oracle//update-expected` writes it again.
`interop` meets each C++ peer, the native one and the browser's, with
Trystero JS peers in headless Chrome, over a Nostr relay of its own on
127.0.0.1, natively and on emscripten; every peer gathers its ICE candidates
on the machine alone. Chrome is the one `CHROME` names, else on macOS
Playwright's `chrome-headless-shell`.

## What derives from Trystero

trystero ports Trystero 0.27.0, and on `main` 0.26.0, Copyright (c) 2021 Dan
Motzenbecker, distributed under the MIT License. These parts of this repository derive from
Trystero:

- the wire protocol, ported from Trystero's code into
  `include/webcpp/trystero`: the Nostr strategy's topics, events and signals,
  the room keys, their derivation and the sealing of signals, the room's
  handshake and its password check, and the action wire with its requests;
- its constants: the domain tags, the topic prefixes, the id alphabet and
  lengths, the announcement cadence, the event kinds, the relay health and
  the backpressure threshold;
- its message layouts: the signals, the action frames and their envelope;
- its room API: listeners and their options, users, room errors and their
  codes, and the relays' connectivity;
- its default relays (`protocol/relays.hpp`), with their order and the
  shuffle that derives an app's relays from its id, and its default STUN
  servers.

Trystero's MIT licence and its copyright notice are in
[LICENSE-TRYSTERO.txt](LICENSE-TRYSTERO.txt), exactly as Trystero 0.26.0
and 0.27.0 ship them. That adds no licence to trystero's own code, which is under the
Boost Software License alone.

## Documentation

trystero's page, with its API reference, is published at
<https://webcpporg.github.io/libs/trystero/>.

## License

Distributed under the [Boost Software License, Version 1.0](LICENSE_1_0.txt).
