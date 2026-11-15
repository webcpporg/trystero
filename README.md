# trystero

trystero is a header-only port of [Trystero](https://github.com/dmotz/trystero)
0.26.0's serverless WebRTC rooms for C++20, one of the libraries of
[webcpp](https://github.com/webcpporg/webcpp). Peers find each other over
public Nostr relays and exchange messages directly, over WebRTC data channels.
The wire is Trystero's: a room joined from C++ and the same room joined from
Trystero's JavaScript are one room.

A room runs on two targets, native and emscripten, through one platform
backend:

| Header | Target | What it holds |
| --- | --- | --- |
| `<webcpp/trystero/platform/backend.hpp>` | native and emscripten | `platform::backend`: webcpp::webrtc for WebRTC, natively over libdatachannel and in a page over its `RTCPeerConnection`; webcpp::beast for the relays' WebSockets, natively Boost.Beast's and in a page its `WebSocket`, their URLs read with Boost.URL; webcpp::asio's timers; and per target the sealer and the randomness, natively OpenSSL's AES-GCM and `RAND_bytes`, in a page `crypto.subtle` and `getentropy` |
| `<webcpp/trystero/platform/page.hpp>` | emscripten | `platform::page_call` and `platform::js_string`, what a page's program uses beside the backend |

`<webcpp/trystero.hpp>` is the rooms themselves, which build on both targets
and leave the platform backend out. The backend's WebRTC is
[webcpp::webrtc](https://github.com/webcpporg/webrtc)'s, the same peer
connection and data channel on both targets, and trystero's Asio is
[webcpp::asio](https://github.com/webcpporg/asio), Boost.Asio itself
natively: a program runs its `io_context` from `main` on both targets, in
the browser under a work guard, where `run` suspends into the page's event
loop and the page's calls into C++ only post to it. Every name is in the
namespace `webcpp::trystero`. Messages, binary data and pings are ported;
audio and video tracks are not yet.

trystero raises no exception of its own: an operation that can fail completes
with an error code. Every part of it, the platform backend included, compiles
and works without exceptions: webcpp::webrtc calls libdatachannel's C API,
where none crosses.

An `app` represents this program in one application: one peer id and one
connection per remote peer, shared by its rooms, with one immutable password
for the app. Different passwords use separate apps with distinct self ids.
Construct it with the
executor, backend and moved `app_config`, then call `join` and test its
`boost::system::result<room>`. The standalone room constructor and
`room_config` are removed: shared configuration, including the password,
moves to `app_config`; handshake timeout and receive limit move to
`room_options`. Rooms have no password override. A new password requires a
new app. Two peers
in one process use two apps; rooms that must not be linked use one app each.

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
connections and channels use the app's resource; libdatachannel, OpenSSL
and the browser retain their own internal allocations.

A passwordless app's rooms are open to peers already connected through any
of its rooms: presence reveals their tokens. A protected app uses a separate
identity and connection; the initial sealed signaling authenticates its
password before its rooms can share that connection. The Trystero JS 0.26.0
wire remains unchanged, including its one-way room handshake: a JS page
that mixes room passwords still exposes its own rooms. Configure each JS
app with one password too. A peer that knows a C++ app's password can still
send a wrong room proof, reported by the lower id only.

## Building inside the superproject

trystero is developed inside the webcpp superproject, as a Boost library is
developed inside Boost. It builds on webcpp::asio, webcpp::webrtc and, for
the relays, webcpp::beast, three libraries of the superproject; its
TLS and WebRTC dependencies are built from pinned sources; only libsecp256k1
needs an external installation:

| Library | Needed by | Found where |
| --- | --- | --- |
| libsecp256k1 0.8.0, with its `schnorrsig` and `extrakeys` modules | every room: it signs the Nostr events peers meet through | `-sSECP256K1_ROOT=<dir>` natively, `-sSECP256K1_EMSCRIPTEN_ROOT=<dir>` for its emscripten build, else `.local/secp256k1-native` and `.local/secp256k1-emscripten` |
| webcpp::asio | every room | `/webcpp/asio//asio` |
| webcpp::webrtc | the platform backend's WebRTC | `/webcpp/webrtc//webrtc` |
| webcpp::beast | the platform backend's relays: their WebSockets, and natively their TLS | natively `/webcpp/beast//tls`, Boost.Beast over the source-built OpenSSL; in a page `/webcpp/beast//browser`, over the page's `WebSocket` |
| Boost.URL | the platform backend's relays: their URLs | `/webcpp//boost_url`, built from Boost's sources once per variant |
| libdatachannel 0.24.6 | the platform backend natively, through webcpp::webrtc | `/webcpp/third_party//libdatachannel`, from pinned source submodules |
| OpenSSL 3.5.9 | the platform backend natively: TLS, AES-GCM and randomness | `/webcpp/third_party//openssl`, also used by Asio and Beast |

The libsecp256k1 `-s` option, on b2's command line, names a directory with the library's
`include/` and `lib/`; the environment is never read. Without it, the library
is looked for in the superproject's `.local/` directory the table names,
where webcpp's CI installs it, and else, natively outside Windows, in
`/usr/include` or `/usr/local/include`, where a system install is
legitimate; a relative `-s` is read from b2's working directory. A `-s` that names a directory
without the library's header stops the build, naming it. A library found
nowhere leaves trystero out: its tests and examples are skipped, b2 says so
once, naming the library and where it looked, and every other library
builds; `webcpp-require-external=on`, which the CI gives, stops the build
instead. With them found,

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
libsecp256k1 and webcpp::document with Boost.JSON's and Boost.Charconv's
definitions, compiled once per variant. Beside
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
twin, the same program on Trystero JS, under `test/oracle/twins/`. The chat
needs a browser, so the build links its program and never runs it.

The quick start, whole: two apps of one program, `alice` and `bob`, each a
peer with the id the program gives it, join the room `lobby` of the app
`webcpp-trystero-quick-start`; alice greets bob when he joins, bob answers
from a coroutine, each prints what it received, after its sender's id, and
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
#include <string_view>

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;

namespace {

/** What one peer's app is made with: its id, and the relay `relay` names, or the public ones. */
trystero::app_config config_for(std::string_view self_id, const char* relay) {
    trystero::app_config config;
    config.app_id = "webcpp-trystero-quick-start";
    // An id of the program's own, where an app would draw 20 random characters, so that what each
    // peer prints of the other is the same on every run.
    config.self_id = self_id;
    if (relay != nullptr) {
        // The peers stay on this machine: the relay given, and a STUN server where nothing answers
        // in place of Trystero's public ones, beside a backend bound to the loopback.
        config.relay_urls = {relay};
        config.ice_servers = {{.urls = {"stun:127.0.0.1:9"}, .username = {}, .credential = {}}};
    }
    // Without a relay, both lists stay empty: Trystero's defaults, as a user's program has them,
    // the public relays its app id selects and Trystero's public STUN servers.
    // A connection closes as soon as no room uses it, so that the program ends once both leave.
    config.idle_timeout = std::chrono::milliseconds{0};
    return config;
}


/** bob's part: waits for a message, prints it, answers its sender, and leaves the room. */
asio::awaitable<void> answer(trystero::room& lobby, trystero::action<std::string>& chat) {
    // as_tuple hands the error over as a value: a room left first ends the wait without throwing.
    auto [ec, text, info] = co_await chat.async_receive(asio::as_tuple(asio::use_awaitable));
    if (ec) {
        co_return;
    }
    std::cout << info.from << ": " << text << '\n';
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

    // Two apps, two peers, each with its own id, in the same room.
    trystero::app alice(io.get_executor(), backend, config_for("alice", relay));
    trystero::app bob(io.get_executor(), backend, config_for("bob", relay));
    auto alice_lobby = alice.join("lobby");
    auto bob_lobby = bob.join("lobby");
    if (!alice_lobby || !bob_lobby) {
        std::cerr << "not joined\n";
        return 1;
    }
    auto alice_chat = alice_lobby->make_action<std::string>("chat");
    auto bob_chat = bob_lobby->make_action<std::string>("chat");

    // alice greets each peer once its handshake completes, prints the answer, and leaves.
    alice_lobby->on_peer_join([&](std::string_view peer) {
        alice_chat.async_send("hello", {.targets = {trystero::peer_id(peer)}, .metadata = {}},
                              [](boost::system::error_code ec) {
                                  if (ec) {
                                      std::cerr << "not delivered: " << ec.message() << '\n';
                                  }
                              });
    });
    alice_chat.on_message([&](const std::string& text, const trystero::message_info& info) {
        std::cout << info.from << ": " << text << '\n';
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

### 5. Build libsecp256k1

libsecp256k1 is built twice: natively, and with Emscripten for the browser.

```sh
git clone --branch v0.8.0 https://github.com/bitcoin-core/secp256k1.git
options="-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DSECP256K1_ENABLE_MODULE_SCHNORRSIG=ON -DSECP256K1_ENABLE_MODULE_EXTRAKEYS=ON \
  -DSECP256K1_ECMULT_WINDOW_SIZE=4 -DSECP256K1_ECMULT_GEN_KB=2 \
  -DSECP256K1_BUILD_TESTS=OFF -DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF \
  -DSECP256K1_BUILD_BENCHMARK=OFF"

cmake -S secp256k1 -B build-secp256k1-native $options \
  -DCMAKE_INSTALL_PREFIX=$PWD/webcpp/.local/secp256k1-native
cmake --build build-secp256k1-native && cmake --install build-secp256k1-native

./emsdk/upstream/emscripten/emcmake cmake -S secp256k1 -B build-secp256k1-emscripten $options \
  -DCMAKE_INSTALL_PREFIX=$PWD/webcpp/.local/secp256k1-emscripten
cmake --build build-secp256k1-emscripten && cmake --install build-secp256k1-emscripten
```

### 6. Initialize the native dependencies

The recursive checkout already includes pinned OpenSSL, libdatachannel,
libjuice, usrsctp, plog and libsrtp sources. For an existing checkout:

```sh
git -C webcpp submodule update --init --recursive third_party/libs/openssl third_party/libs/libdatachannel
```

B2 builds them with the selected compiler, runtime and C++ ABI when the
platform backend is first needed natively. Each upstream checkout remains
unchanged. The generated headers and static archives are kept together by
build configuration. Asio, Beast and Trystero share one OpenSSL provider in a
combined application; no system OpenSSL or installed libdatachannel is used.
Native Trystero targets declare `<threading>multi` because WebRTC runs
worker threads. The supplied examples and tests declare it already.

### 7. Tell b2 its compilers and Boost

```sh
cat > user-config.jam <<EOF
using clang ;
using boost : 1.92 : <include>$(brew --prefix boost)/include <library>$(brew --prefix boost)/lib ;
using emscripten : : $PWD/emsdk/upstream/emscripten/em++
  : <archiver>$PWD/emsdk/upstream/emscripten/emar ;
EOF
```

### 8. Build the quick start and the chat page

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

### 9. Run them

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
chat, and so does any Trystero JS page.

A native peer and a page meet too: the lane `interop` of `test/oracle`
meets the native peer, and the browser's, with Trystero JS pages in headless
Chrome (The tests, below).

### When something goes wrong

- **b2 says trystero is left out**: a library is not in the directory of
  `webcpp/.local/` it names; install it there as steps 5 and 6 do, or give
  b2 `-s<VARIABLE>=<dir>` with the directory that `cmake --install` wrote.
- **b2 stops naming a `-s`**: it names a directory without the library's
  header; give the directory that `cmake --install` wrote.
- **b2 prints `node: bad option: --experimental-wasm-threads`** when it
  configures the emscripten toolset: b2's toolset probes node with a flag that
  recent Node removed. It is harmless.
- **The page loads but never joins**: it must be opened at `localhost` (or
  `127.0.0.1`), not at a network address or from a file. The browser gives
  the cryptography the room needs only to secure pages, and `localhost` counts
  as one.
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

Two lanes of trystero's own, declared in `test/oracle/Jamfile`, check it
against Trystero itself, pinned at 0.26.0 by `test/oracle/package.json` and
`package-lock.json`, with Node:

    b2 libs/trystero/test/oracle//oracle
    b2 libs/trystero/test/oracle//interop
    b2 toolset=emscripten libs/trystero/test/oracle//interop

`oracle` runs Trystero's own functions over the inputs of
`test/fixtures/cases/protocol.json` and checks that they still write
`test/fixtures/expected/expected.json`, the values the protocol tests
reproduce; `b2 libs/trystero/test/oracle//update-expected` writes it again.
`interop` meets each C++ peer, the native one and the browser's, with
Trystero JS peers in headless Chrome, over a Nostr relay of its own on
127.0.0.1, natively and on emscripten; every peer gathers its ICE candidates
on the machine alone. Chrome is the one `CHROME` names, else on macOS
Playwright's `chrome-headless-shell`.

## What derives from Trystero

trystero ports Trystero 0.26.0, Copyright (c) 2021 Dan Motzenbecker,
distributed under the MIT License. These parts of this repository derive from
Trystero:

- the wire protocol, ported from Trystero's code into
  `include/webcpp/trystero`: the Nostr strategy's topics, events and signals,
  the room's handshake and its password check, and the action wire;
- its constants: the topic prefixes, the id alphabet and lengths, the
  announcement cadence, the event kinds and the backpressure threshold;
- its message layouts: the signals, the action frames and their envelope;
- its default relays (`protocol/relays.hpp`), with their order and the
  shuffle that derives an app's relays from its id, and its default STUN
  servers.

Trystero's MIT licence and its copyright notice are in
[LICENSE-TRYSTERO.txt](LICENSE-TRYSTERO.txt), exactly as Trystero 0.26.0
ships them. That adds no licence to trystero's own code, which is under the
Boost Software License alone.

## Documentation

trystero's page, with its API reference, is published at
<https://webcpporg.github.io/libs/trystero/>.

## License

Distributed under the [Boost Software License, Version 1.0](LICENSE_1_0.txt).
