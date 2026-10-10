# Working on trystero

webcpp's rules apply here: read the superproject's
[AGENTS.md](../../AGENTS.md)
(<https://github.com/webcpporg/webcpp/blob/main/AGENTS.md>) first. This file
holds only what is specific to trystero.

- **The original, and its wire.** trystero ports Trystero 0.26.0
  (<https://github.com/dmotz/trystero>, MIT). The wire is Trystero's and never
  changes: what a Trystero JS peer or a page sees is kept byte for byte, the
  topic prefixes `"Trystero@"` and `"Trystero:"`, the handshake's
  `"__trystero_pw"`, every protocol constant and message layout under
  `include/webcpp/trystero/protocol/`, the default relays in their order, and
  the names embind exports, `trystero_dispatch` and the chat's `join` and
  `send`. A rename never replaces the word `trystero`:
  it uses an exact C++ pattern (`namespace webcpp::trystero`,
  `<webcpp/trystero/`, `WEBCPP_TRYSTERO_`), and a string literal is changed
  only when it is not on the wire, as the error category `webcpp.trystero`
  and the examples' app ids `webcpp-trystero-quick-start` and
  `webcpp-trystero-chat`. `LICENSE-TRYSTERO.txt` is Trystero's notice,
  unchanged, and the README names what derives from Trystero.
- **The core and the two backends.** `<webcpp/trystero.hpp>`, the headers
  directly under `include/webcpp/trystero/`, `protocol/` and `detail/` are
  the core: the rooms over the backend interface of `backend.hpp`, which
  build on both targets, native and emscripten.
  `meta/include-boundaries.json` keeps them free of the two backends, of
  libdatachannel, OpenSSL and Emscripten, and the lint fails an include that
  crosses it. `browser/` is the browser backend, over the page's WebRTC
  through embind, which builds only on emscripten; `browser/src.hpp` holds
  the embind registration of `trystero_dispatch`, which a program includes
  once. `native/` is the native backend, over libdatachannel, Boost.Beast
  and OpenSSL, which builds only natively. `test/Jamfile` compiles each
  header alone on the targets that build it, in four calls.
- **The native backend's exceptions.** libdatachannel reports errors by
  throwing, and `native/data_channel.hpp` and `native/peer_connection.hpp`
  catch them, so they, and `native::backend`, which makes their objects, sit
  behind `#ifndef WEBCPP_TRYSTERO_NO_EXCEPTIONS` and do not exist without
  exceptions: code that uses them does not compile there. The native timer,
  sealer and WebSockets, and everything else of trystero, compile without
  exceptions. Every test and program that uses the native backend's WebRTC
  declares `<exception-handling>on`, with the reason in a comment, and so
  does the headers-alone call of `data_channel.hpp` and
  `peer_connection.hpp`, which declare nothing without exceptions:
  `/webcpp/trystero//native` cannot, since b2 ignores a non-free feature among
  usage requirements, so a user's `exception-handling=off` would reach them.
  The lint names those two headers as left out of its analysis without
  exceptions; `backend.hpp`, `sealer.hpp` and `websocket.hpp` are compiled
  alone in a call of their own, without it, so the lint analyses the timer,
  the sealer and the WebSockets without exceptions too. trystero raises
  nothing of its own; a raise would go through `boost::throw_exception`.
- **The external dependencies.** libsecp256k1 (every room), libdatachannel
  and OpenSSL (the native backend) are not part of Boost. `build.jam` finds
  them with webcpp's `webcpp.external` and `webcpp.external-found`
  (`tools/webcpp.jam`): where `-sSECP256K1_ROOT=<dir>`,
  `-sSECP256K1_EMSCRIPTEN_ROOT=<dir>`, `-sLIBDATACHANNEL_ROOT=<dir>` and
  `-sOPENSSL_ROOT=<dir>` on b2's command line say, a relative one from b2's
  working directory, never the environment,
  else in the superproject's `.local/secp256k1-native`,
  `.local/secp256k1-emscripten`, `.local/libdatachannel` and
  `.local/openssl` when it holds the library's header, where webcpp's CI
  actions install them, until webcpp builds them from `third_party/`; so the
  reference and the lint need no `-s`. A library found in neither is looked
  for, natively outside Windows, in `/usr/include` or `/usr/local/include`,
  where a system install is legitimate. A `-s`
  that names a directory without its library's header stops the build,
  naming it. A library that no candidate holds leaves trystero out, its
  targets skipped, with one message that names the library and every place
  it looked, so that `b2 test` builds every other library; with
  `webcpp-require-external=on`, as the CI builds, it stops the build
  instead. With MSVC the names are CMake's and Shining
  Light's: `libsecp256k1`, linked with `SECP256K1_STATIC` defined, which a
  static libsecp256k1 needs on Windows, and `libssl` and `libcrypto`, also
  searched in `lib/VC/x64/MD`. libdatachannel is a
  C++ library: it must be built with the same standard library as the
  program that links it (libc++ for clang on macOS, libstdc++ for GCC), or
  the program does not link, though the native headers still compile alone.
- **embind.** Every program on emscripten links `-lembind`, which
  `/webcpp/trystero//trystero` adds as a linker flag: `<embind>on`, b2's own
  feature, is not free, and b2 ignores it among usage requirements.
- **No JavaScript exception enters WebAssembly.** The browser backend makes
  every call into JavaScript that can throw (a constructor, `JSON.parse`, a
  `send`, a WebRTC or Web Crypto method) through `browser/js.hpp`, whose
  JavaScript, defined once in `browser/src.hpp` with `EM_JS`, runs it inside
  `try` and returns a value or the exception as a value, which the C++ turns
  into the error its interface has. Every event of the page enters C++
  through a function of the same file that catches what still escapes,
  puts WebAssembly's stack pointer and the loop's depth back as they were
  before the call, reports it and lets the loop poll again. With
  exceptions, `browser::dispatch` catches a C++ exception first, which
  frees it: one that reached JavaScript would keep its object for good. A
  message that is not a frame is dropped before it is read. The browser
  tests run under node with exceptions and without them
  (`*_no_exceptions`), and the interop lane passes both ways.
- **Bounded signaling.** Announcements are not sealed, so anyone who knows
  the app and room ids can announce any number of peer ids, and anyone who
  reads the relays can send any signal in any peer's name.
  - Nothing a peer sends changes a negotiation before it opens with the
    room key: an offer, an answer or a candidate that does not open is
    counted, reported once per negotiation, and changes nothing else. A
    flood of fresh ids makes each a negotiation, so the reports are paced:
    the first of a quiet 10 s goes to `on_error` at once, with its peer,
    and the others of those 10 s in one report, with no peer and their
    number.
  - A sealed offer or answer holds neither its offer id nor a peer id
    (Trystero's wire), so one recorded from the relays still opens when
    anyone resends it in its sender's name, and so does the room's own, sent
    back to it. A room remembers, by the SHA-256 of their opened text, the
    last 1024 offers and answers it made or acted on
    (`detail::signaling::remembered_descriptions`), and refuses one that
    comes to it as a peer's: it changes nothing and is logged. It makes or
    acts on at most two per connection it makes, so each is remembered for
    at least an offer's lifetime (57 s), and in a quiet room for its whole
    life.
  - What remains: a sealed offer or answer the room neither made nor
    acted on still opens and is acted on. Such texts are not scarce:
    every peer seals a fresh offer for any id announced above its own, and
    announcements need no key. So anyone who reads and publishes on the
    relays, without the key, can keep any two peers of a room apart from
    their first meeting for as long as it keeps at it, each attempt
    costing the other peer a connection of its window. It cannot read or
    alter anything the peers exchange, join the room, or touch peers
    already joined; a joined peer that drops is a new peer again. The
    room refuses only what it made or already acted on. Only a wire that
    seals the offer id and both peer ids closes this, which Trystero would
    have to share (reported upstream).
  - A room holds at most `detail::signaling::max_unconnected_peers` (32)
    negotiations with peers it is not connected to. A negotiation whose
    offer and answer expire without a connection is dropped, an idle one
    gives its place to a new peer, and a new peer beyond the bound is
    refused and logged once.
  - A room makes at most `max_connections_per_window` (64) connections in
    any 57 s, however soon they end, and a page at most
    `browser::backend::max_peer_connections` (256), counted from their
    construction until the garbage collector frees them, as Chromium counts
    them against its 500 (measured with headless Chrome: the 501st throws
    "Cannot create so many PeerConnections", and a closed one counts until
    a collection). The native backend makes at most
    `backend_config::max_peer_connections_per_minute` (256).
  - A signal's fields are bounded in `protocol/signal.hpp`: the peer id
    (64 bytes; Trystero's are 20), the offer id, a sealed offer or answer
    (64 KiB) and a sealed candidate (16 KiB, above the 8192 bytes of
    plaintext Trystero takes).
  - A room asks a peer below it to offer at most once in 5.3 s per relay,
    Trystero's announce interval, so a flood of low ids does not have the
    relays rate-limit the room.
  - A relay is a third party too. A text longer than 1 MiB closes its
    connection on both backends. A relay that stops reading is dropped once
    the text held unsent for it would pass 4 MiB
    (`protocol::max_queued_relay_bytes`): the native queue on native
    (`native::websocket_limits::max_queued_bytes`), the page WebSocket's
    `bufferedAmount` in the browser. On native only, a relay that stops
    answering is dropped once `idle_timeout` (30 s) passes with its
    keep-alive ping unanswered (`backend_config::relay_limits`); the idle
    timeout counts bytes received, and a ping waits for the write in
    flight. A page's `WebSocket` cannot ping, so the browser has no idle
    timeout. Each drop reports one close, and the room reconnects with its
    backoff.
  - What a flood still does: joined peers, memory and connections stay
    safe, but a flooder who keeps sending new ids takes each place, or each
    connection of the window, as it frees, so new peers can be kept out for
    as long as the flood lasts. A joined peer whose connection drops comes
    back as a new peer, with the same id, and is kept out the same way:
    the room has no renegotiation that would keep its place. Trystero JS
    bounds none of this.
- **The memory relays.** `test/support/memory_backend.hpp`, in
  `webcpp::trystero::test`, is a backend in memory: Nostr relays that forward
  ephemeral events to matching subscriptions, peer connections that pair
  through their descriptions, data channels that deliver in order, and a
  sealer that opens only what was sealed under the same key, each event
  posted to the `io_context`. A test of a whole room runs over it, on both
  targets, without the network.
- **The oracle and the interop tests, trystero's own lanes.**
  `test/oracle/` pins Trystero 0.26.0 (`trystero`, `@trystero-p2p/core` and
  `@trystero-p2p/nostr`, with `ws` 8.22.0 and `@noble/secp256k1` 3.2.0 for
  the local relay, which imports both) in its `package.json` and
  `package-lock.json`, which `webcpp.original node` installs. Its `Jamfile`
  declares two own lanes. `oracle`, which names no target: `oracle.mjs` runs
  Trystero's own functions over the inputs of
  `test/fixtures/cases/protocol.json` and writes `expected.json`, which must
  equal `test/fixtures/expected/expected.json` byte for byte, the file the
  `protocol` and `native_sealer` tests read; `update-expected` is its only
  writer. `interop`, on native and emscripten (`native.trystero.interop` and
  `emscripten.trystero.interop` in the report): two driven tests,
  `interop_native` and `interop_browser`, whose driver `interop/run.mjs`
  meets the C++ peer, the native one as a process or the browser's in
  headless Chrome, with Trystero JS peers in Chrome, over the in-memory
  Nostr relay `interop/relay.mjs` on 127.0.0.1, in sixteen scenarios each:
  C++ offering, Trystero JS offering with a password, a Trystero JS peer
  that also sends malformed candidates (`js.html`'s
  `malformed-candidate-to`), a first relay whose URL no WebSocket can be
  made to, a Trystero JS peer that also sends text messages on its data
  channel (`text-messages`), a C++ peer given a TURN server without
  credentials, a watcher without the key that answers every offer with
  garbage in the other peer's name (`forger.html`), the same watcher
  resending, after the Trystero JS peer's connections drop
  (`window.drop`), the sealed answer and then the sealed offer it recorded
  of that peer (`mode=replay`), which the C++ peer must refuse, the same
  watcher sending the C++ peer its own sealed offer back as the other
  peer's answer, and as its offer by way of a decoy id (`mode=reflect`),
  which the C++ peer must refuse too, a flood of announcements and of
  unsealed offers from fresh peer ids (`window.flood`,
  `window.floodOffers`) whose offers the watcher answers too, a relay text
  longer than 1 MiB, once and then after every subscription
  (`relay.broadcast`, `relay.options.afterSubscribe`), the C++ peer and a
  Trystero JS page each in two rooms of one app (`room2`), every C++ room
  with its own peer id, since Trystero keeps one connection per app and
  peer id, which its rooms share, and drops a second one, a wrong password,
  and a mesh of three; and, for the native peer, three
  more: a room moved over the joined one, which must leave it as
  destroying it does; a relay that ends the connection just as the peer
  writes, with a text longer than 1 MiB or a reset
  (`relay.options.strike`), which twelve peers in turn must survive and
  reconnect after; and the driver's own check that a native peer killed
  by a signal (`abort` on its standard input) is named at once. The
  native peer and `native_websocket` are built with the standard
  library's own checks (libc++'s extensive hardening, libstdc++'s
  assertions), so that a completion that touches a cleared container
  fails at once. Both lanes are of the kind `original`, so the CI gives
  them its Node.
  - Every peer gathers its ICE candidates on the machine alone: the C++
    peers get `stun:127.0.0.1:9`, where nothing answers, in place of
    Trystero's public STUN servers, the native peer binds its connections to
    127.0.0.1 (`native::backend_config::bind_address`), and the Trystero JS
    peers have `rtcConfig: {iceServers: []}`.
  - Chrome is `--chrome`, `CHROME`, or on macOS Playwright's
    `chrome-headless-shell` from its cache; elsewhere `CHROME` is required,
    and the driver fails naming it. A full Chrome serves as well as the
    headless shell: the driver starts either with `--headless=new`, and with
    the flags that stop Chrome's own requests, `--no-first-run`,
    `--no-default-browser-check`, `--disable-component-update`,
    `--disable-background-networking` and `--disable-sync`. Those five do
    not stop a full Chrome for Testing 156 from asking Google for accounts,
    update checks and component downloads (measured), so the driver also
    resolves no host but 127.0.0.1 (`--host-resolver-rules`); the headless
    shell asks nothing with or without any of them. The driver removes
    Chrome's profile once Chrome has exited, on every way the run ends but a
    SIGKILL, whose profile the next run removes. Chrome offers host
    candidates only on the interface of its default route, so in a container
    whose only interface is the loopback its peers gather none and never
    connect: the lanes were measured passing under `--network none` with one
    interface that holds the default route and leads nowhere, a dummy
    device, to which nothing but the kernel's IPv6 router solicitations was
    sent.
  - Each driver is bounded by 900 s (`<webcpp-drive-timeout>`), beyond its
    scenarios' own limits, so that a scenario that does not complete fails by
    name with what each peer saw. A native peer that dies by a signal the
    driver did not send, as a crash does, is named as soon as it dies
    (`native#0: killed by SIGSEGV`), and every wait of its scenario ends. A
    wire string changed in the C++ library (`"__trystero_pw"`) fails the
    password scenario of both.

  ```
  b2 -a libs/trystero/test/oracle//oracle
  b2 -a --dump-tests --out-xml=native.trystero.interop.xml libs/trystero/test/oracle//interop
  b2 -a --dump-tests --out-xml=emscripten.trystero.interop.xml toolset=emscripten libs/trystero/test/oracle//interop
  ```

- **Allocation.** trystero is born with webcpp's allocation rule, through
  `std::pmr`, header-only. The page's section "Allocation" is written from
  these rules.
  - **One resource per room.** `room_config::memory`, a
    `std::pmr::memory_resource*` that defaults to
    `std::pmr::get_default_resource()`, is the resource every allocation of
    the room goes through, from its construction to its destruction: the
    room itself, the copy of its configuration, its peers, actions, queues,
    frames, transfers, negotiations and relays, its handlers, the text it
    composes for its log, the objects its backend makes for it, the context
    libsecp256k1 signs with (`secp256k1_context_preallocated_create`, in the
    resource), and the JSON it parses and writes. trystero touches it only
    from the room's executor, so it need not be thread-safe when that
    executor runs on one thread. A null `memory` is a programming error,
    which `BOOST_ASSERT` catches when the room is constructed.
  - **The resource's lifetime.** The resource must outlive the room, every
    value the room handed out (a `std::pmr::string`, a `pmr_bytes` or a
    `boost::json::value` a handler kept), and every operation the room left
    pending: destroying a room closes everything, but what it closed still
    has operations queued that free into the resource. On Asio, the resource
    lives until the io_context of the room's executor has run out of the
    room's work or has been destroyed, so it is declared before the
    io_context; in the browser, until the room's pending promises have
    settled (a promise's registration with the page's `event_loop` keeps its
    handler, in the room's resource), in practice for the page's life. The
    order that holds:

    ```cpp
    std::pmr::unsynchronized_pool_resource memory;  // first: destroyed last
    boost::asio::io_context io;                     // destroyed before memory
    {
        trystero::room room(io.get_executor(), backend, {.memory = &memory}, "lobby");
        // ...
    }
    io.run();  // or destroy io: the room's pending operations free into memory
    ```

    The test measures it: once two rooms are destroyed, their resources
    still have memory out, and none once the io_context has drained. No
    check in debug builds catches the misuse: trystero holds a raw pointer
    and cannot see the resource end, and the use comes after the room is
    gone, from an operation of Asio's or the browser's; a resource that
    asserts nothing is out when it is destroyed, as the test's counts, is
    the check, and it is the user's.
  - **Which resource.** A room allocates and frees on every event (a frame,
    a parsed text, a post, a log line), so a resource that reuses what is
    freed, a pool, suits a long-lived room; a
    `std::pmr::monotonic_buffer_resource` grows with every event and suits
    a short-lived one, or a test.
  - **Containers.** Every container trystero declares is a `std::pmr` one,
    but `trystero::bytes` (a `std::vector<std::uint8_t>`) and
    `trystero::payload` (a variant of `std::string`, `boost::json::value` and
    `bytes`), the user's choice of `std::allocator`:
    `std::pmr::string` (and `peer_id`), `std::pmr::vector` (and
    `backend::bytes`, `protocol::bytes` and `pmr_bytes`), `std::pmr::deque`,
    `std::pmr::map`, `std::pmr::set`, `std::pmr::unordered_map` and
    `std::pmr::unordered_set`, built over the room's resource. A struct that
    holds them takes the room's `std::pmr::polymorphic_allocator<>` in its
    constructor, and a container of such structs is given it when it makes
    one (`try_emplace(key, allocator)`). A `std::pmr` container is never
    copied without an allocator: its copy constructor takes the default
    resource, so a copy names the resource
    (`std::pmr::string(text, allocator)`). Moving keeps the resource.
  - **Shared objects** are made with `std::allocate_shared` over
    `std::pmr::polymorphic_allocator<>`, never with `std::make_shared`,
    `std::make_unique` or `new`; a timer is a `std::shared_ptr` for that
    reason.
  - **Handlers.** `webcpp::trystero::handler<R(Args...)>` (`handler.hpp`) is
    a move-only callable that allocates through a
    `std::pmr::polymorphic_allocator<>`: constructed with
    `handler(std::allocator_arg_t, allocator, f)`, it holds `f` inline when
    `f` fits in three pointers and is nothrow-movable, and in storage of the
    allocator otherwise. It replaces every `std::function` of `backend.hpp`
    (the handlers of `timer`, `websocket`, `data_channel`,
    `peer_connection` and `sealer`), of the room and of
    `detail::logger::sink`, since `std::function` takes no allocator since
    C++17. Constructed from `f` alone it uses the default resource; the room
    moves a handler it is given (`room_config::log`) into its own resource
    with the allocator-extended move, which makes `room_config` move-only:
    a room is given its configuration by value, moved
    (`room(ex, factory, std::move(config), id)`). A move assignment carries
    the source's allocator with its target. Where a handler must survive its
    own replacement during its call, its owner keeps it in a
    `std::shared_ptr` made in the resource and calls it through a copy of
    that pointer.
  - **The backend.** Each method of `backend::factory` that makes an object
    takes the room's `std::pmr::memory_resource*` as its last parameter, and
    the backend allocates its objects, their handlers and the frames and
    texts it hands to the room through it.
  - **Asio.** Every `boost::asio::post`, `dispatch`, timer wait and
    `any_completion_handler` the room makes carries
    `boost::asio::bind_allocator(std::pmr::polymorphic_allocator<>(memory),
    ...)`. A post through `boost::asio::any_io_executor` is wrapped by Asio in
    a function object allocated with `std::allocator`, from Asio's
    per-thread recycling cache, which calls `std::aligned_alloc` on a miss
    (measured: two per post, in every round of five posts, natively and on
    emscripten). So `detail::post` posts through the executor of an
    `io_context` when the room's executor is one, where Asio allocates the
    operation as the handler's allocator says; on another executor the
    wrapper stays Asio's. A user's completion handler is allocated as its
    own associated allocator says: the room never binds its allocator over
    it, and the page shows `bind_allocator` with the room's resource. When
    the room erases a completion handler's type
    (`detail::completion`, an `any_completion_handler`), it records whether
    the handler's associated executor is the room's, which only the concrete
    type tells without allocating; such a completion runs from an operation
    the room posts to its executor, allocated with the handler's allocator,
    and one bound to another executor is handed to it by Asio, whose
    type-erased executor wraps it with `std::allocator` (measured: telling
    the executor apart through the erased type cost two `operator new` calls
    a completion, and Asio's wrapper a `std::aligned_alloc`).
  - **Boost.JSON** allocates through `json_resource` (`json_resource.hpp`),
    a `boost::container::pmr::memory_resource` that forwards to a
    `std::pmr::memory_resource`, header-only: `boost::container::pmr`'s own
    global resources are compiled (`BOOST_CONTAINER_DECL`), and the CI
    installs Boost's headers alone. The room's JSON storage is
    `boost::json::make_shared_resource<json_resource>(memory)`, made once when
    the room is constructed, since a value a handler keeps may outlive the
    room. Text is parsed by a `boost::json::parser` over that storage, never
    by `boost::json::parse`, whose temporaries beyond a stack buffer come
    from the default resource; JSON is written by a
    `boost::json::serializer` whose stack is the resource.
  - **Payloads.** `detail::payload_traits` has `std::pmr::string` and
    `std::pmr::vector<std::uint8_t>` (`webcpp::trystero::pmr_bytes`) beside
    the standard types, and `decode(incoming& message,
    std::pmr::memory_resource* memory)` takes the resource for every type:
    the `std::pmr` types are built in it, and `boost::json::value` in the
    room's JSON storage, which the message carries. `std::string`, `bytes`
    and `payload` stay as they are, with `std::allocator`: the user's
    choice, and a received `bytes`, or the binary of a `payload`, is a copy
    of the frame, which is in the room's resource. `encode`
    takes the resource too, for what keeps the payload alive.
  - **The protocol** (`protocol/`) takes a `std::pmr::memory_resource*` as
    the last parameter of every function that returns what it allocates,
    with `std::pmr::get_default_resource()` as its default, and returns
    `std::pmr` types; `protocol::nostr::verify` uses
    `secp256k1_context_static` and allocates nothing.
  - **Outside the resource**, stated on the page: libdatachannel's and
    OpenSSL's own allocations, with the `std::string` and `rtc::Description`
    libdatachannel's API takes and returns; `emscripten::val`'s JavaScript
    heap; the JSON storage's counter, which Boost.JSON allocates with
    `operator new` once per room; Asio's own once-per-context allocations
    (its services and the scheduler's first operation); the wrapper Asio puts
    around a post through an executor that is not an `io_context`'s; the
    native backend's posts from libdatachannel's threads, which carry Asio's
    default allocator, since the resource is touched only on the room's
    executor; and Boost.Beast's and Asio's state inside the native WebSocket
    (its stream, its resolver's results, its coroutines' frames); the
    payload types `std::string`, `bytes` and `payload`, with
    `std::allocator`; and, on Emscripten, the C library's `select()`, which
    Asio's reactor calls on every wait of an io_context with a timer or a
    socket, and which callocs an array on every call (the test replaces it,
    below). The browser's `event_loop` and the test's `memory_network` are
    shared by every room of a page or of a test, and allocate the entries
    of their registrations and their relays through the resource they are
    constructed with; a registered callback stays in its own resource.
  - **The proof** is `test/allocation_test.cpp`, on both targets, which
    counts the global `operator new`, replaced, and the C library's
    allocation functions, where the platform lets a program see them: glibc's
    functions are replaced and forward to `__libc_*`, Emscripten's dlmalloc
    has weak definitions the test replaces and forwards to
    `emscripten_builtin_*`, macOS reports each to `malloc_logger`, and MSVC's
    are not counted, nor any under AddressSanitizer, whose own allocator
    reports to none of these. Two rooms over the memory relays exchange a
    payload, a `std::pmr::string`, a `pmr_bytes` and a `boost::json::value`
    in turn, while both counts see none and the counting resources of the two
    configurations see the rooms'; and two rooms with a password, a log sink
    and the network's state in a resource of its own are made, join, exchange
    a payload that travels in chunks, measure a round trip, one says goodbye,
    both are destroyed and the io_context is drained, while `operator new`
    sees exactly the two rooms' JSON storage counters and the C library
    nothing, and the rooms' resources have nothing out once the io_context
    has drained. The counting resources also count a return of memory they
    never gave, a pmr object freed through the wrong resource, which must be
    none. Only the thread `main` runs on is counted: the C library reports
    allocations from every thread, and a system thread's are not the rooms'.
    Asio's once-per-context allocations are made first (its timer service,
    its reactor, the scheduler's first operation, and room in its timer queue
    for 256 waits); without them the count is 8, not 2. The replaced
    `operator new` raises an alignment below `sizeof(void*)` and rounds the
    size to a multiple of the alignment, since `std::aligned_alloc` refuses
    both (measured: macOS refuses the alignments 1, 2 and 4, Emscripten 1 and
    2, and both a size that is not a multiple of the alignment), throws
    `std::bad_alloc` where there are exceptions, uses `_aligned_malloc` on
    MSVC, and checks itself for every alignment from 1 to 64. On Emscripten
    the test also replaces `select()`, which Emscripten implements over
    `poll()` with an array it callocs on every call, and which Asio's reactor
    calls on every wait of the io_context the test drives (measured:
    thousands of calls while two rooms exchange one payload): the replacement
    keeps the array on the stack, so that the count sees the rooms'
    allocations rather than the C library's wait.
- **No twins.** Every example needs the public relays or a browser, so each
  is a `webcpp.link`, linked and never run, and has no `.expected` and no
  twin in Trystero's language. The chat page's site is written by
  `b2 toolset=emscripten libs/trystero/example/chat//site` under the build
  directory, never in the tree.
