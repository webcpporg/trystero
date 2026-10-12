# trystero

trystero is a header-only port of [Trystero](https://github.com/dmotz/trystero)
0.26.0's serverless WebRTC rooms for C++20, one of the libraries of
[webcpp](https://github.com/webcpporg/webcpp). Peers find each other over
public Nostr relays and exchange messages directly, over WebRTC data channels.
The wire is Trystero's: a room joined from C++ and the same room joined from
Trystero's JavaScript are one room.

A room runs on two targets, through a backend of each:

| Backend | Header | Target | Built on |
| --- | --- | --- | --- |
| the browser's | `<webcpp/trystero/browser/backend.hpp>` | emscripten | the page's `WebSocket`, `RTCPeerConnection` and `crypto.subtle`, through embind |
| the native one | `<webcpp/trystero/native/backend.hpp>` | native | libdatachannel for WebRTC, Boost.Beast for the relays' WebSockets, OpenSSL for TLS and AES-GCM |

`<webcpp/trystero.hpp>` is the rooms themselves, which build on both targets
and include neither backend. Every name is in the namespace
`webcpp::trystero`. Messages, binary data and pings are ported; audio and
video tracks are not yet.

trystero raises no exception of its own: an operation that can fail completes
with an error code. The rooms, the protocol and the browser backend compile
without exceptions. The native backend's WebRTC needs them, since
libdatachannel reports errors by throwing: without exceptions, or with
`WEBCPP_NO_EXCEPTIONS` or `WEBCPP_TRYSTERO_NO_EXCEPTIONS` defined,
`native::data_channel`, `native::peer_connection` and `native::backend` do
not exist, and the native timer, sealer and WebSockets remain.

Every allocation of a room goes through the `std::pmr::memory_resource` its
`room_config::memory` names, the default resource unless it names another:
its state, its handlers, its JSON, the objects its backend makes for it, and
a message an action's handler receives as a `std::pmr::string`, a
`trystero::pmr_bytes` or a `boost::json::value`. A completion handler is
allocated as its own associated allocator says, which
`boost::asio::bind_allocator` sets. What libdatachannel, OpenSSL and the
browser's JavaScript allocate stays theirs. The resource must outlive the
room, every value it handed out, and the operations it leaves pending when
it is destroyed: declare it before the io_context, which then drains or is
destroyed first; in the browser, keep it for the page's life. A room
allocates and frees on every event, so a pool suits a long-lived room better
than a monotonic buffer. `room_config` is move-only: pass it moved.

## Building inside the superproject

trystero is developed inside the webcpp superproject, as a Boost library is
developed inside Boost. Besides Boost, it needs three libraries that are not
part of it, until webcpp builds them from `third_party/`:

| Library | Needed by | Found where |
| --- | --- | --- |
| libsecp256k1 0.8.0, with its `schnorrsig` and `extrakeys` modules | every room: it signs the Nostr events peers meet through | `-sSECP256K1_ROOT=<dir>` natively, `-sSECP256K1_EMSCRIPTEN_ROOT=<dir>` for its emscripten build, else `.local/secp256k1-native` and `.local/secp256k1-emscripten` |
| libdatachannel 0.24.6 | the native backend | `-sLIBDATACHANNEL_ROOT=<dir>`, else `.local/libdatachannel` |
| OpenSSL 3 | the native backend | `-sOPENSSL_ROOT=<dir>`, else `.local/openssl` |

Each `-s`, on b2's command line, names a directory with the library's
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

build the tests and the examples natively and on emscripten, with the
emscripten toolset that `user-config.jam` registers against emsdk. A b2
project uses the rooms and the browser backend through
`/webcpp/trystero//trystero`, which adds trystero's headers, Boost's,
libsecp256k1 and Boost.JSON's definitions, compiled once per variant, and on
emscripten embind and `BOOST_ASIO_DISABLE_THREADS`. A native program uses
`/webcpp/trystero//native`, which adds libdatachannel and OpenSSL, and
declares `<exception-handling>on` itself, as `example/Jamfile` does: b2 lets
no usage requirement override a build's `exception-handling=off`.

## The examples

Three examples come with the library. Each needs the public relays or a
browser, so the build links them and never runs them.

| Example | What it is | Target |
| --- | --- | --- |
| `example/native_quick_start.cpp` | a native program that joins the room `lobby`, greets every peer and prints what it receives | native |
| `example/quick_start.cpp` | the browser quick start, a `.js` and `.wasm` to embed in a page | emscripten |
| `example/chat/` | a chat page whose room is C++ compiled to WebAssembly | emscripten |

The native quick start, whole:

<!-- include::example/native_quick_start.cpp[tag=program] -->
```cpp
#include <webcpp/trystero/native/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>

#include <iostream>
#include <string>

namespace trystero = webcpp::trystero;

int main() {
    boost::asio::io_context io;
    trystero::native::backend backend(io.get_executor());
    trystero::room room(io.get_executor(), backend, {.app_id = "webcpp-trystero-quick-start"},
                        "lobby");
    auto chat = room.make_action<std::string>("chat");

    room.on_peer_join([&](std::string_view peer) {
        trystero::send_options to_peer{.targets = {trystero::peer_id(peer)}, .metadata = {}};
        chat.async_send("hello from a native peer", std::move(to_peer),
                        [](boost::system::error_code) {});
    });
    chat.on_message([](const std::string& text, const trystero::message_info& info) {
        // flushed: into a pipe too, since the program runs until it is stopped
        std::cout << info.from << ": " << text << '\n' << std::flush;
    });

    io.run();  // lint-run: the program's network I/O; libdatachannel's threads post to it
}
```

The browser quick start's room, which a page keeps alive as long as the tab:

<!-- include::example/quick_start.cpp[tag=application] -->
```cpp
/** What the page keeps alive: event loop, backend, room and actions. */
struct application {
    asio::io_context io;
    trystero::browser::event_loop loop{io};
    trystero::browser::backend backend{loop};
    trystero::room room{
        loop.get_executor(), backend, {.app_id = "webcpp-trystero-quick-start"}, "lobby"};
    trystero::action<std::string> chat = room.make_action<std::string>("chat");
    trystero::action<std::string> echo = room.make_action<std::string>("echo");
};
```

`example/Jamfile` declares each program on its own target:

<!-- include::example/Jamfile[tag=native] -->
```
webcpp.link native_quick_start : native_quick_start.cpp
  : <library>/webcpp/trystero//native <exception-handling>on : native ;
```

<!-- include::example/Jamfile[tag=browser] -->
```
webcpp.link quick_start : quick_start.cpp
  : <linkflags>-sENVIRONMENT=web <linkflags>-sALLOW_MEMORY_GROWTH : emscripten ;
```

`b2 toolset=emscripten libs/trystero/example/chat//site` copies the chat page
and its program into `site/` beside the program, under the build directory
and out of the tree, one per variant, and prints that directory. On another
toolset it stops, saying that the page is built on emscripten alone.

## From an empty directory to a native peer and the chat

These steps go from an empty directory to the native quick start and the
chat page talking to each other over the public Nostr relays, on macOS with
Homebrew. Every command runs in the workspace directory of step 2 unless it
says otherwise.

### 1. Install the tools

git, CMake, clang (the Xcode command line tools), Python 3, Node, OpenSSL 3,
Boost 1.92 and b2:

```sh
brew install cmake openssl@3 boost boost-build node
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

### 6. Build libdatachannel

libdatachannel's CMake finds OpenSSL where `OPENSSL_ROOT_DIR` says, which
Homebrew's OpenSSL needs, and the build where `webcpp/.local/openssl` links. It is built with the compiler that builds the
programs, clang: a program built with GCC and libstdc++ does not link against
a libdatachannel built with libc++.

```sh
openssl=$(brew --prefix openssl@3)
ln -s "$openssl" webcpp/.local/openssl

git clone --recurse-submodules --branch v0.24.6 \
  https://github.com/paullouisageneau/libdatachannel.git
cmake -S libdatachannel -B build-libdatachannel -DCMAKE_BUILD_TYPE=Release \
  -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON -DOPENSSL_ROOT_DIR=$openssl \
  -DCMAKE_INSTALL_PREFIX=$PWD/webcpp/.local/libdatachannel
cmake --build build-libdatachannel && cmake --install build-libdatachannel
```

The three libraries are now where the build looks for them, in
`webcpp/.local/`, which git ignores: no variable is needed, in this terminal
or another.

### 7. Tell b2 its compilers and Boost

```sh
cat > user-config.jam <<EOF
using clang ;
using boost : 1.92 : <include>$(brew --prefix boost)/include <library>$(brew --prefix boost)/lib ;
using emscripten : : $PWD/emsdk/upstream/emscripten/em++
  : <archiver>$PWD/emsdk/upstream/emscripten/emar ;
EOF
```

### 8. Build the native quick start and the chat page

```sh
cd webcpp
b2 --user-config=../user-config.jam libs/trystero/example//native_quick_start-program
b2 --user-config=../user-config.jam toolset=emscripten libs/trystero/example/chat//site
```

The first build also compiles Boost.JSON's definitions, and the first
emscripten build Emscripten's system libraries, which takes a few minutes.
The second command ends by printing the chat site's directory,
`bin/libs/trystero/example/chat/emscripten-6.0.11/debug/address-model-32/cxxstd-20-iso/link-static/target-os-none/site`,
which holds `index.html`, `chat-program.js` and `chat-program.wasm`.

### 9. Run them

In a terminal, from `webcpp`, start the native quick start. It runs until you
stop it with Ctrl-C:

```sh
`find bin/libs/trystero/example -name native_quick_start-program -type f`
```

In a second terminal, from the workspace, serve the chat page:

```sh
cd webcpp
python3 -m http.server 8000 -d `find bin/libs/trystero/example/chat -name site -type d`
```

Open <http://localhost:8000>, write `webcpp-trystero-quick-start` as the app
and `lobby` as the room, and press Join. Within a few seconds the native quick
start joins as a peer: the page shows "1 peer", the native peer's id "joined",
and its greeting, "hello from a native peer". What you send from the page is
printed in the native quick start's terminal, after the page's peer id:

```
eXfaPSb9aXpUL1KNqF8M: hello from the page
```

Every browser tab that joins the same app and room joins the same chat.

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
the rooms, the protocol and their details on both targets, the native
backend natively, against libdatachannel and OpenSSL and with exceptions
whatever the build asks, and the browser backend on emscripten. It also
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
