# Working on trystero

webcpp's rules apply here: read the superproject's
[CODING-GUIDELINES.md](../../CODING-GUIDELINES.md)
(<https://github.com/webcpporg/webcpp/blob/main/CODING-GUIDELINES.md>), whose
chapter 15 gives trystero's design decisions with their reasons, and its
[AGENTS.md](../../AGENTS.md)
(<https://github.com/webcpporg/webcpp/blob/main/AGENTS.md>) first. This file
holds only what is specific to trystero.

- **The original, and its wire.** trystero ports Trystero 0.26.0
  (<https://github.com/dmotz/trystero>, MIT). The wire is Trystero's and never
  changes: what a Trystero JS peer or a page sees is kept byte for byte, the
  topic prefixes `"Trystero@"` and `"Trystero:"`, the handshake's
  `"__trystero_pw"`, every protocol constant and message layout under
  `include/webcpp/trystero/protocol/`, the default relays in their order, and
  the names the chat's page calls, `joinRoom` and `sendText`, and the interop
  page's `window.leave`. A rename never replaces the word `trystero`:
  it uses an exact C++ pattern (`namespace webcpp::trystero`,
  `<webcpp/trystero/`, `WEBCPP_TRYSTERO_`), and a string literal is changed
  only when it is not on the wire, as the error category `webcpp.trystero`
  and the examples' app ids `webcpp-trystero-quick-start` and
  `webcpp-trystero-chat`. `LICENSE-TRYSTERO.txt` is Trystero's notice,
  unchanged, and the README names what derives from Trystero. Comments cite
  Trystero's code as `dist/<file>:<lines>`, the files of
  `@trystero-p2p/core` 0.26.0 as published.
- **The core and the platform.** `<webcpp/trystero.hpp>`, the headers
  directly under `include/webcpp/trystero/`, `protocol/` and `detail/` are
  the core: the rooms over the backend interface of `backend.hpp`, which
  build on both targets, native and emscripten.
  `meta/include-boundaries.json` keeps them free of the platform backend, of
  its WebRTC and webcpp::webrtc, of libdatachannel, OpenSSL and Emscripten,
  and the lint fails an include that crosses it. `platform/` is the platform
  backend, `platform::backend` (`platform/backend.hpp`): one class that
  implements the backend interface on both targets, never one per target.
  `webrtc/` is its WebRTC, over webcpp::webrtc, on both targets: one peer
  connection and one data channel, which hold
  `webcpp::webrtc::basic_peer_connection` and `basic_data_channel` over a
  `std::pmr::polymorphic_allocator` on the app's resource and keep
  trystero's policies over them. Its relays are one session of webcpp::beast
  on both targets (`platform/websocket.hpp`), natively over Boost.Beast's
  own stream and in a page over webcpp::beast's page `WebSocket`. Its timer
  is the core's `detail::timer` over webcpp::asio's `steady_timer`, the one
  timer on both targets. Its clocks are the standard library's on both
  targets, `std::chrono::system_clock` floored to whole seconds
  (`unix_now`) and `std::chrono::steady_clock` (`steady_now`), which
  Emscripten reads from the page's `Date.now()` and `performance.now()`.
  Two parts are the target's own, chosen by `WEBCPP_TARGET_EMSCRIPTEN`,
  config's (neither the library nor its tests test `__EMSCRIPTEN__`): the
  sealer (`platform/sealer.hpp`), natively OpenSSL's AES-256-GCM and in a
  page `crypto.subtle` through webcpp::asio's `browser::async_await`; and
  the platform's one source of randomness,
  `platform::detail::fill_random` (`platform/entropy.hpp`), whose result is
  checked: natively OpenSSL's `RAND_bytes`, in a page `getentropy`, which
  Emscripten draws from `crypto.getRandomValues`. Without randomness
  `random()` aborts the program and a seal fails with
  `error::backend_failure`. `platform/page.hpp`, which builds only on
  emscripten (an `#error` elsewhere), holds what a page's program uses beside
  the backend, `platform::page_call` and `platform::js_string`; a program
  includes no source file of trystero's: `/webcpp/asio//asio` links the
  browser runtime. `test/Jamfile` compiles each header alone on the targets
  that build it, in three calls: the core; `platform/backend.hpp`,
  `entropy.hpp`, `sealer.hpp`, `websocket.hpp` and `webrtc/` on both
  targets, with `/webcpp/trystero//platform`; and `platform/page.hpp` on
  emscripten.
- **The backend interface stays.** The rooms hold the backend's peer
  connections and data channels through `backend::peer_connection` and
  `backend::data_channel`, abstract classes, as chapter 15 decided: the
  memory relays (`test/support/memory_backend.hpp`) implement them for the
  rooms of the tests, on both targets, where node has no WebRTC, and the
  interop's native peer wraps the platform backend's (`delayed_channel`,
  `observed_connection`). Their one implementation over a platform is
  `webrtc/`, which webcpp::webrtc's table "trystero's backend, part by
  part" maps part by part. The interface's clocks are std::chrono's, so
  that no unit is converted at the seam: `factory::steady_now` returns the
  `std::chrono::steady_clock::time_point` the core keeps
  (`detail::time_point`), from an origin of the backend's, and
  `factory::unix_now` a `std::chrono::sys_seconds`, which a Nostr event's
  `created_at` and a subscription's `since` write.
- **Exceptions.** trystero's WebRTC is webcpp::webrtc's, which natively calls
  libdatachannel's C API, where no exception crosses: every part of trystero,
  the platform backend included, works with exceptions and without them on
  both targets, and no program or test asks for `<exception-handling>on`.
  Every `platform_*` test but `platform_websocket` also runs with
  `exception-handling=off`, its `_no_exceptions` twin, as the evidence.
  trystero raises nothing of its own; a raise would go through
  `boost::throw_exception`; natively the platform backend keeps a TLS
  context OpenSSL cannot make as an error value (`tls_context()`), and its
  `wss://` relays report a close.
- **webcpp::asio.** trystero names Asio as `webcpp::asio` wherever it builds
  on emscripten (the core, the platform backend, the tests over the memory
  relays, the examples and the browser's interop peer): natively it is
  Boost.Asio itself, and on emscripten the backend webcpp::asio ships, a
  service of whose `io_context` webcpp::webrtc's browser objects deliver
  through, so the room's executor must be webcpp::asio's. The platform
  backend names it everywhere, on both targets, its relays' WebSockets
  included, which are webcpp::beast's over webcpp::asio. The guide's
  reference reads every header for Emscripten's target, where webcpp::asio
  and webcpp::beast are their browser implementations and
  `platform/page.hpp` builds, with `/webcpp/trystero//platform` built
  natively (`<threading>multi`) beside `/webcpp//emscripten-reference`
  (`doc/Jamfile`). `/webcpp/trystero//trystero` brings `/webcpp/asio//asio`:
  on emscripten every program is built in a suspension variant
  (`webcpp-suspend=jspi` by default, or `asyncify`) and named after it
  (the chat's `chat-program-60-d-jspi.js`), and the suites run in both.
  A page's program runs its io_context from `main` under a work guard, as
  webcpp::asio's model asks (the owner's decision of 2026-11-14): the page's
  events post to it through the bridge, and the page calls C++ only through
  `platform::page_call` (`platform/page.hpp`), a bridge function that posts,
  never through embind. Under Asyncify an embind function called while
  `main` is suspended returns a promise that never settles, and its second
  call aborts a debug build. A page_call's handler lets no JavaScript error
  out, webcpp::asio's rule for every handler, since one leaves `run` for
  good: it checks the page's value before converting it, and the chat and
  `platform_page` do (`platform_page` passes a number and `undefined`). Its
  resource holds each call until the io_context runs it or is destroyed, so
  it outlives the io_context (`platform_page_call`).
  Every emscripten test that runs an io_context runs it from `main`, and
  ends with `webcpp::test::exit_status`, since under Asyncify a `main` that
  suspended loses what it returns; `platform_page` proves the program's
  shape. There is one timer, the core's `detail::timer` over webcpp::asio's
  `steady_timer`, on both targets, which `timer` tests on both.
- **The dependencies.** libsecp256k1 (every room) remains an external
  installation, found through `webcpp.external` and `webcpp.external-found`
  at `-sSECP256K1_ROOT=<dir>` or `-sSECP256K1_EMSCRIPTEN_ROOT=<dir>`, then
  `.local/secp256k1-native` or `.local/secp256k1-emscripten`. Native system
  fallback outside Windows is allowed only for this dependency. A missing
  installation leaves trystero out unless `webcpp-require-external=on`.
  MSVC links its static `libsecp256k1.lib` with `SECP256K1_STATIC`.
  `/webcpp/trystero//trystero` (`build.jam`'s `tag::trystero[]`) is the
  rooms alone, the core: its headers, Boost, webcpp::document
  (`/webcpp/document//document`, with Boost.JSON's and Boost.Charconv's
  definitions), config, webcpp::asio and libsecp256k1; on no target does it
  bring webcpp::webrtc, and the tests over the memory relays use it alone.
  `/webcpp/trystero//platform` (`tag::platform[]`) is the platform backend's
  target on both targets, alternatives by `<toolset>emscripten`, which a
  native program and a page's program both name. Its WebRTC is
  webcpp::webrtc's, `/webcpp/webrtc//webrtc`, on both targets; natively it
  names `/webcpp/third_party//libdatachannel`, which builds pinned
  libdatachannel 0.24.6, libjuice, usrsctp and libsrtp as separate static
  archives against `/webcpp/third_party//openssl` and its matching
  generated headers. These are the same OpenSSL target used by Asio and
  Beast, so a combined application has one provider. For the relays
  `/webcpp/trystero//platform` also names, natively, `/webcpp/beast//tls`,
  `/webcpp//boost_url` and `/webcpp/asio//tls`, whose embedded authorities
  a program may trust in place of the system's, and on emscripten
  `/webcpp/beast//browser` and `/webcpp//boost_url`, with no OpenSSL. No
  `LIBDATACHANNEL_ROOT`, `OPENSSL_ROOT` or installed binary is consulted.
  Initialize their sources with `git submodule update --init --recursive
  third_party/libs/libdatachannel third_party/libs/openssl`. Native builds
  need CMake 3.21 or newer in addition to OpenSSL's Perl and Make/nmake.
  B2 forwards the configured compiler, C++ ABI, runtime and optimization
  flags, and isolates artifacts by their effective configuration. The
  upstream source trees stay unchanged; verified staged copies are built.
- **Native threading.** libdatachannel always uses worker threads, so every
  native consumer of `/webcpp/trystero//platform` must declare
  `<threading>multi`, including native headers-alone and reference targets.
  An explicit single-threaded native build of the platform backend fails
  with an actionable B2 diagnostic; a page has no threads, and the target
  asks for none on emscripten. Asio and Beast still support their own
  single-threaded OpenSSL builds.
- **The runtime.** webcpp::webrtc's `runtime` caps the connections alive at
  once and, natively, holds libdatachannel's process-wide state: at most one
  exists in a process at a time. `platform::backend` makes one of its own,
  in `backend_config::memory`, with `backend_config::runtime` (its
  `max_peer_connections` 256 by default), or takes the program's
  (`backend(ex, runtime, config)`, or `backend(ex, tls, runtime, config)`),
  on both targets, and every connection and channel of the backend's WebRTC
  keeps it alive through a `std::shared_ptr`, so that one a queued handler
  holds after the backend is gone never outlives it. In a page
  webcpp::webrtc's census of the page counts every runtime's connections.
- **The relays.** One session (`platform/websocket.hpp`) serves both
  targets: resolve, connect, TLS, the handshake, a read loop and one write at
  a time, on the room's executor, natively over Boost.Beast's own stream and
  in a page over webcpp::beast's page `WebSocket` (`/webcpp/beast//browser`).
  On both targets a write that fails ends the session at once, reporting one
  close, unless it is already closing, when its read ends it. A relay's URL
  is read with Boost.URL on both targets, by the same rules, which the
  guide's "The relays" lists: an IPv6 address in brackets reaches the
  resolver without them, the scheme in any case, a host decoded and in
  lower case, an escaped dotted quad an address; userinfo, a fragment, a
  port beyond 65535, an IPvFuture host and `http://` and `https://` (which a
  page's `WebSocket` alone would take as `ws://` and `wss://`) are refused,
  and what a page's URL parser reads otherwise fails closed: IPv4 shorthands
  (`127.1`) and any host whose last label is a number, international names
  (no A-labels: webcpp::idna is not used yet), tabs, newlines and
  backslashes. Each refusal reports a close. Natively, over TLS the
  connection sets `verify_peer` on its stream whatever the program's context
  says, sends the host (SNI), through `webcpp::beast::set_server_name`, only
  when it is a name once decoded, and checks the certificate against the
  URL's host, a name or an address, with Asio's `host_name_verification`; in
  a page the browser does all three. On Windows the system's authorities are
  none unless `SSL_CERT_FILE` or `SSL_CERT_DIR` names some. A
  backend trusts the system's authorities by default
  (`platform::make_tls_context()`, which in a page records only the policy
  the page's stream asks for), or the program's context through the
  constructors that take a `boost::system::result`; `tls_context()` returns
  that result, and a backend whose context holds an error reports a close
  for every `wss://` relay. The test's TLS server uses an authority and a
  certificate for `localhost` embedded in `test/platform_websocket_test.cpp`
  (EC P-256, valid from 2026-01-01 to 2126-01-01), which nothing else
  trusts; its `embedded-roots` region is what the guide's page includes.
- **The backend's WebRTC policies** (`webrtc/`), on the executor, on both
  targets: a text message, or a binary one larger than a room frame, is
  dropped after `async_receive` (webcpp::webrtc reads every message into a
  vector of the app's resource, at most `max_message_size`, 256 KiB); the
  connection's state and its ICE state merge into one, as a page sees them;
  a local candidate leaves as the JSON of an `RTCIceCandidateInit` and a
  remote one is read from it; `buffered_amount_low` is reported when the
  amount falls to 64 KiB after a send left it above. Natively a URL
  libdatachannel cannot parse (`stuns:`) is left out by trying each URL
  alone; in a page a refused server refuses the connection. The per-minute
  window (`backend_config::max_peer_connections_per_minute`, 256) is
  `platform::backend`'s, a `detail::construction_window` in
  `backend_config::memory`, on both targets. `backend_config::bind_address`
  is passed to webcpp::webrtc on both targets; a page leaves it empty, since
  webcpp::webrtc refuses to bind there.
- **Channel handoff.** Valid frames received before a message handler is
  installed wait in an app-resource FIFO, at most 64 frames for 10 s from the
  first, on both targets now. Overflow or expiry closes the channel; close
  clears the queue. The timer is the factory's (`make_timer` with the app's
  resource), whose waits a restart or a cancel invalidates. Installing a
  receiver posts a resource-allocated flush, never an inline call. Each frame
  checks the current handler and open channel, and a message already read
  ahead of the flush drains older frames when a receiver exists. A transport
  closed during adoption, before binding, forgets its negotiation and
  restarts announcements. `platform_peer_connection` tests real queued events
  (the offerer made on an executor the test runs), FIFO, replacement, close,
  limits and resource release (`webrtc::data_channel::pending_frames`);
  `shared` tests closure during adoption, and interop controls native open
  delivery while a JS peer sends both rooms' presence first.
- **embind.** Every program on emscripten links `-lembind`, which
  `/webcpp/asio//asio` adds as a linker flag, and `/webcpp/trystero//trystero`
  brings: `<embind>on`, b2's own feature, is not free, and b2 ignores it
  among usage requirements.
- **No JavaScript exception enters WebAssembly.** In a page the platform
  backend's calls that can throw are caught in JavaScript and return what
  failed as a value, which the C++ turns into the error its interface has:
  a `WebSocket` constructor or `send` in webcpp::beast's page stream, which
  turns a socket that cannot be made into an error, so that its relay
  reports a close; a Web Crypto method in an `EM_ASM` block of an inline
  function of `platform/sealer.hpp`, made inside an `async` function, so
  that what it throws rejects its promise. Every event and every page call
  enters C++ through webcpp::asio's bridge, which restores WebAssembly's
  stack pointer, catches a C++ exception in C++, which frees it (one that
  reached JavaScript would keep its object for good), and only posts.
  webcpp::webrtc's JavaScript catches the page's WebRTC exceptions the same
  way, and its events come through the same bridge, which posts to the
  io_context. A message that is not a frame is dropped on the executor,
  after webcpp::webrtc read it: a text as the few bytes it holds, a binary
  one at most 256 KiB, past which the channel closes. The platform's tests
  on emscripten (`platform_backend`, `platform_sealer`,
  `platform_websocket_unreachable` and every `platform_page*`) run under
  node with exceptions and without them (`*_no_exceptions`), and the
  interop lane passes both ways.
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
    any 57 s, however soon they end. The platform backend makes at most
    `backend_config::max_peer_connections_per_minute` (256) in any minute,
    for every app and room it serves, on both targets, and holds at most its
    runtime's `max_peer_connections` (`backend_config::runtime`, 256) at
    once: natively those alive, and in a page those webcpp::webrtc's census
    of the page counts, from their construction until the garbage collector
    frees them, as Chromium counts them against its 500 (measured with
    headless Chrome: the 501st throws "Cannot create so many
    PeerConnections", and a closed one counts until a collection).
  - A signal's fields are bounded in `protocol/signal.hpp`: the peer id
    (64 bytes; Trystero's are 20), the offer id, a sealed offer or answer
    (64 KiB) and a sealed candidate (16 KiB, above the 8192 bytes of
    plaintext Trystero takes).
  - A room asks a peer below it to offer at most once in 5.3 s per relay,
    Trystero's announce interval, so a flood of low ids does not have the
    relays rate-limit the room.
  - A strategy remembers the newest 4096 event ids
    (`detail::nostr_strategy::remembered_events`) in a
    `boost::unordered_flat_set` hashed with Boost.Hash2's `siphash_64` under
    a key drawn from the backend's randomness when it is made, since a relay
    chooses the ids; `room`'s `colliding_event_ids_stay_fast` feeds it 2048
    ids found to share one bucket of `std::unordered_set` and reads
    Boost.Unordered's statistics (`BOOST_UNORDERED_ENABLE_STATS`, defined by
    that test only).
  - A relay is a third party too. A text longer than 1 MiB closes its
    connection on both targets, in a page before it is copied into
    WebAssembly (webcpp::beast's `read_message_max`). A relay that stops
    reading is dropped once the text held unsent for it would pass 4 MiB
    (`protocol::max_queued_relay_bytes`): the session's own queue on both
    targets (`platform::websocket_limits::max_queued_bytes`), and in a page
    also the `WebSocket`'s `bufferedAmount`, which webcpp::beast's
    `queue_limits::send` bounds to the same, past which the write fails.
    In a page the texts a relay sent that wait for the executor are bounded
    by webcpp::beast's `queue_limits::receive` (4 MiB), past which the
    socket closes. Natively only, a relay that stops answering is dropped
    once `idle_timeout` (30 s) passes with its keep-alive ping unanswered
    (`backend_config::relay_limits`); the idle timeout counts bytes
    received, and a ping waits for the write in flight. A page's
    `WebSocket` cannot ping, and webcpp::beast's page stream refuses
    keep-alive pings, so a page has no idle timeout. Each drop reports one
    close, and the room reconnects with its backoff.
  - What a flood still does: joined peers, memory and connections stay
    safe, but a flooder who keeps sending new ids takes each place, or each
    connection of the window, as it frees, so new peers can be kept out for
    as long as the flood lasts. A joined peer whose connection drops comes
    back as a new peer, with the same id, and is kept out the same way:
    the room has no renegotiation that would keep its place. Trystero JS
    bounds none of this.
- **The app and shared admission.** An `app` scopes one self id, one
  immutable password and one connection per remote peer, with no static
  registry. `app_config::password`, after `self_id`, is copied into the
  app's resource; every room reads it there. `room_options` has no password.
  Different passwords require separate apps with distinct self ids; their
  connections, presence and tokens are never reused. The initial encrypted
  signaling authenticates a protected app's password before any room can
  reuse that connection. `join` returns a result, refusing duplicates
  (`already_joined`) and a 65th room (`too_many_rooms`), counting farewells
  until withdrawal. A changed domain password requires a new app.
  Configuration errors go to `app::on_error`; room errors name peer
  problems. A passwordless app's connected peers can echo another room's
  token and join it. The Trystero JS 0.26.0 wire is unchanged: a JS page
  that mixes room passwords retains its one-way handshake exposure,
  including admission to its higher-id rooms without their password and
  offline-guess material. C++ app domains protect the C++ app's admission;
  they do not repair a remote JS page. A peer that already knows the domain
  password can still send a bad room proof, kicked and paced by rebinding.
- **Shared connection bounds.** A connection accepts 64 advertised tokens
  and at most 64 kept frames for local unbound rooms, expiring 10 s after
  the first, and drops unknown room tokens. A malformed envelope ends all
  bindings. A kick ends only its binding; if the last binding was kicked,
  close at once. After departures, idle expiry is `app_config::idle_timeout`,
  123,333 ms by default, 0 closes immediately. A remote must advertise the token
  itself before any binding; forged relay announcements bind nothing.
  Rebinding after a kick or peer leave uses the larger of handshake timeout
  and 10 s plus 1 s, doubling to 5 minutes until a completed handshake or
  connection end. The lower id refuses bindings during its own-kick
  backoff and keeps frames; the higher drops frames after its own kick
  until it sends presence and accepts the lower's presence immediately.
  Reuse is a first-meeting path; later relay signals cannot shorten that
  backoff. The initiator accepts correct answers to its last eight
  challenges since the last completed handshake. Only the lower id reports
  an invalid shared room proof. One room's departure does not close another's
  transport; waiting sends take turns by binding at each drain.
- **The memory relays.** `test/support/memory_backend.hpp`, in
  `webcpp::trystero::test`, is a backend in memory: Nostr relays that forward
  ephemeral events to matching subscriptions, peer connections that pair
  through their descriptions, data channels that deliver in order, and a
  sealer that opens only what was sealed under the same key, each event
  posted to the `io_context`. A test of a whole room runs over it, on both
  targets, without the network.
  Its optional fault hooks use `std::size_t` connection ids, `on_made`
  (with ICE servers) and `peer_of`, a deliver/drop/hold filter, ordered
  `release`, deferred one-end `open`, `inject` and `fail`. A held frame
  holds every later frame from its sending end behind it. Buffered channels
  drain up to `drain_per_post` bytes per post, delivering only complete
  frames and reporting exact unsent bytes through `buffered`. Set
  `virtual_clock` before making timers: `advance`, `step` and `poll_ready` run due
  timers and ready work deterministically, in due-time and registration
  order. The virtual clock is a `std::chrono::steady_clock::time_point`
  from the clock's epoch, which `steady_now` returns, and `unix_now` reads
  it as 2025-10-05T21:33:20Z plus its whole seconds; tests compare its
  durations exactly. Optional hook storage uses the default resource only
  when enabled; allocation tests enable none. `connection`, `memory_backend`, `shared`,
  `shared_no_exceptions` and `shared_hardened` cover these paths, with
  sanitizers and library hardening checking resource lifetimes.
- **The oracle and the interop tests, trystero's own lanes.**
  `test/oracle/` pins Trystero 0.26.0 (`trystero`, `@trystero-p2p/core` and
  `@trystero-p2p/nostr`, with `ws` 8.22.0 and `@noble/secp256k1` 3.2.0 for
  the local relay, which imports both, and werift 0.24.4, the WebRTC the
  twins give Trystero JS in Node as `rtcPolyfill`, which Trystero's README
  names for Node) in its `package.json` and `package-lock.json`, which
  `webcpp.original node --conditions=development` installs. Its `Jamfile`
  declares two own lanes. `oracle`, which names no target: `oracle.mjs` runs
  Trystero's own functions over the inputs of
  `test/fixtures/cases/protocol.json` and writes `expected.json`, which must
  equal `test/fixtures/expected/expected.json` byte for byte, the file the
  `protocol` and `platform_sealer` tests read, and `webcpp.twins` runs every
  twin of `example/` through `run_twin.mjs` (below); `update-expected` is the
  only writer of `expected.json` and of each divergent twin's output. `interop`, on native and emscripten (`native.trystero.interop` and
  `emscripten.trystero.interop` in the report): two driven tests,
  `interop_native` and `interop_browser`, whose driver `interop/run.mjs`
  meets the C++ peer, the native one as a process or the browser's in
  headless Chrome, with Trystero JS peers in Chrome, over the in-memory
  Nostr relay `interop/relay.mjs` on 127.0.0.1, with the existing scenarios and the shared scenarios below:
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
  Trystero JS page each in two rooms of one app (`room2`), sharing one
  connection and one self id per app, a wrong password,
  and a mesh of three; and, for the native peer, three
  more: a room moved over the joined one, which must leave it as
  destroying it does; a relay that ends the connection just as the peer
  writes, with a text longer than 1 MiB or a reset
  (`relay.options.strike`), which twelve peers in turn must survive and
  reconnect after; and the driver's own check that a native peer killed
  by a signal (`abort` on its standard input) is named at once. The
  native peer and `platform_websocket` are built with the standard
  library's own checks (libc++'s extensive hardening, libstdc++'s
  assertions), so that a completion that touches a cleared container
  fails at once. Both lanes are of the kind `original`, so the CI gives
  them its Node.
  - Shared scenarios on both targets cover: (1) two rooms on one
    connection, with chat and 100 KB files both ways; (2) a third room
    joining later by presence; (3) the page's rooms joined before the C++
    app's; (4) admission to a passwordless room by token and refusal by a
    protected app with another password and self id, in both id orders;
    (5) either side leaving one room while another keeps exchanging without a new connection; (6) departure from every room,
    followed by idle expiry; (7) corrupted room proofs after authenticated
    connection establishment, in both id orders, with
    paced reports and the other room unaffected; (8) a 5,000-message flood
    while another room pings; (9) a held handshake frame recovering in both
    id orders with no false password report. Crossed adoption races allow
    retries but fail by connection count above four constructions per side.
  - Recovered-peer relay scenarios wait for fresh, currently joined peers on
    both sides within the existing 30 s setup bound, then require a fresh
    round-trip message before injecting a relay fault. A join reported by one
    side alone does not prove the other's handshake completed. The controlled
    `delayNextRecovery` fixture delays one outbound direction of the first
    recovered channel for 12 s, beyond the C++ handshake deadline; a subsequent
    connection must complete the setup. A departure after the relay fault
    still fails. Console errors remain fatal, with RTCError fields recorded
    alongside peer events so a failure is not reduced to `OperationError`.
  - Every peer gathers its ICE candidates on the machine alone: the C++
    peers get `stun:127.0.0.1:9`, where nothing answers, in place of
    Trystero's public STUN servers, the native peer binds its connections to
    127.0.0.1 (`platform::backend_config::bind_address`), and the Trystero JS
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
  - Each driver is bounded by 2400 s (`<webcpp-drive-timeout>`), beyond its
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
  - **The app's resource and the room's.** `app_config::memory` defaults
    to `std::pmr::get_default_resource()` and must not be null. It receives
    app state, copied configuration including the immutable password, self id,
    registry, logger and sink,
    every log sentence from the app and its rooms, connections, advertised
    tokens, kept frames, drain order and presence. Peer connections, data
    channels and received frames use it even while a room negotiates them.
    `room_options::memory`, the app's resource when null, receives room
    state, bindings and drain waiters, negotiation records, rebinding
    counts and backoffs, sealed texts, relay sockets, signing context,
    actions, outgoing frames, queues, transfers, the documents of the JSON
    texts it reads, and handlers.
    Every touch runs on the app's executor; one thread needs no thread-safe
    resource, multiple threads require a strand.
  - **References and detachment.** Rooms hold the app strongly, the app
    reaches rooms weakly, bindings hold connections weakly and connections
    hold plain pointers to bindings. Detachment fails and frees all drain
    waiters and removes all pointer registrations before returning. The
    app keeps no room-resource allocation after that room ends. Adoption
    replaces the room's negotiation handlers with the app's; ending a
    negotiation clears the room's handlers.
  - **The resources' lifetimes.** The app's resource must outlive its
    state: its handle, all its rooms, retained values and pending
    operations. A room's own resource must outlive its state, retained
    values and pending operations. Declare both before the io_context,
    then the backend, app and rooms, and drain or destroy the io_context
    before either resource. In a page the resources live until pending
    promises settle, in practice for the page's life. A raw resource
    pointer cannot detect early destruction; a counting resource that checks
    nothing is outstanding at destruction makes this the user's check.
  - **Which resource.** A room allocates and frees on every event (a frame,
    a parsed text, a post, a log line), so a resource that reuses what is
    freed, a pool, suits a long-lived room; a
    `std::pmr::monotonic_buffer_resource` grows with every event and suits
    a short-lived one, or a test.
  - **Containers.** Every container trystero declares is a `std::pmr` one
    or a Boost container over `std::pmr::polymorphic_allocator`, but
    `trystero::bytes` (a `std::vector<std::uint8_t>`) and
    `trystero::payload` (a variant of `std::string`,
    `webcpp::document::document` and `bytes`), the user's choice of
    `std::allocator`:
    `std::pmr::string` (and `peer_id`), `std::pmr::vector` (and
    `backend::bytes`, `protocol::bytes` and `pmr_bytes`), `std::pmr::deque`,
    `std::pmr::map` and `std::pmr::set`, and three Boost containers over
    `std::pmr::polymorphic_allocator`: `boost::unordered_flat_set` (the event
    ids a strategy remembers), `boost::circular_buffer` (the construction
    window, the recent challenges) and
    `boost::circular_buffer_space_optimized` (the replay cache), each built
    over its owner's resource. A struct that
    holds them takes the room's `std::pmr::polymorphic_allocator<>` in its
    constructor, and a container of such structs is given it when it makes
    one (`try_emplace(key, allocator)`). A `std::pmr` container is never
    copied without an allocator: its copy constructor takes the default
    resource, so a copy names the resource
    (`std::pmr::string(text, allocator)`). Moving keeps the resource. No
    type of trystero is a data structure meant for a shared segment, which
    chapter 4 asks each library to state: every type is a run-time object
    (apps, rooms, actions, sockets, handlers, the backend), routed through
    `std::pmr` resources, and none is shareable.
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
    C++17. Constructed from `f` alone it uses the default resource; the app
    moves `app_config::log` into its resource with the allocator-extended
    move, which makes `app_config` move-only: pass it moved or build it in place. A move assignment carries
    the source's allocator with its target. Where a handler must survive its
    own replacement during its call, its owner keeps it in a
    `std::shared_ptr` made in the resource and calls it through a copy of
    that pointer.
  - **The backend.** Each method of `backend::factory` that makes an object
    takes a `std::pmr::memory_resource*` as its last parameter. Peer
    connections, data channels and received frames use the app's; relay
    sockets, sealers and room timers use the room's. The backend allocates
    objects, handlers and their frames or texts through that resource; the
    backend's WebRTC gives webcpp::webrtc's objects a
    `std::pmr::polymorphic_allocator` on the app's resource, and binds it
    over the handler of each operation it starts. The platform backend's own
    state, its copy of `bind_address`, its window of constructions
    (`detail::construction_window`) and its own `webcpp::webrtc::runtime`, is
    in `backend_config::memory`, the default resource unless the program
    gives one, on both targets; it must not be null, and must outlive the
    backend and every connection of its runtime. `platform_backend` proves
    it on both targets, with none of that state in the default resource.
  - **Asio** (webcpp::asio). Every `webcpp::asio::post`, `dispatch`, timer
    wait and `any_completion_handler` the room makes carries
    `webcpp::asio::bind_allocator(std::pmr::polymorphic_allocator<>(memory),
    ...)`. A post through `webcpp::asio::any_io_executor` is wrapped by Asio in
    a function object allocated with `std::allocator`, from Asio's
    per-thread recycling cache, which calls `std::aligned_alloc` on a miss
    (measured: two per post, in every round of five posts, natively and on
    emscripten). So `detail::post` posts through the executor of an
    `io_context` when the room's executor is one, where Asio allocates the
    operation as the handler's allocator says, and a page's sealer awaits
    each Web Crypto promise through it so too
    (`platform_page_sealer_allocation` proves it, against two C library
    allocations a seal or an open before); on another executor, a strand for
    one, the wrapper stays Asio's. A user's completion handler is allocated
    as its own associated allocator says: the room never binds its allocator
    over it, and the page shows `bind_allocator` with the room's resource.
    When the room erases a completion handler's type
    (`detail::completion`, an `any_completion_handler`), it records whether
    the handler's associated executor is the room's, which only the concrete
    type tells without allocating; such a completion runs from an operation
    the room posts to its executor, allocated with the handler's allocator,
    and one bound to another executor is handed to it by Asio, whose
    type-erased executor wraps it with `std::allocator` (measured: telling
    the executor apart through the erased type cost two `operator new` calls
    a completion, and Asio's wrapper a `std::aligned_alloc`).
  - **webcpp::document** keeps a document's image in one buffer, and its
    external storage leaves that buffer to its user: every value, strings
    included, is allocated inside it by Boost.Interprocess's
    `rbtree_best_fit` heap, with offsets for pointers. trystero gives each
    text it reads such a buffer of the room's resource
    (`protocol::json::document`, of the app's for a remote candidate),
    sized for a text of a few values, twice the text and 4 KiB, and made
    again twice as large when the parse fills it, up to the caller's bound
    (`protocol::json::protocol_limits`: twice its text bound and 64 KiB). A
    text the room writes is appended field by field to a `std::pmr::string`
    of its resource, its strings quoted by `webcpp::document::quote`, which
    allocates only through that string, and a JSON payload is written by
    `webcpp::document::stringify_to` in pieces of 8 bytes, which no standard
    library's string keeps outside its own object. What document's parser
    and writer use while they work stays in the standard allocator:
    Boost.JSON's parser stack, the path and the pending key and text of the
    value being read, the tree's record of each change, which places a
    value, and the writer's stack. Each is freed before the parse or the
    write that made it returns, so none is left at any moment the room's
    executor is between handlers (measured natively with the LLVM standard
    library: about seven allocations a value for each text a room reads,
    and 48 to write and read back a JSON payload of 7 values).
  - **Payloads.** `detail::payload_traits` has `std::pmr::string` and
    `std::pmr::vector<std::uint8_t>` (`webcpp::trystero::pmr_bytes`) beside
    the standard types, and `decode(incoming& message,
    std::pmr::memory_resource* memory)` takes the resource for every type:
    the `std::pmr` types are built in it, and `pmr_document` in a buffer of
    it. `std::string`, `bytes`, `webcpp::document::document` and `payload`
    stay as they are, with `std::allocator` or document's own
    `operator new`: the user's choice, and a received `bytes`, or the binary
    of a `payload`, is a copy of the frame, which starts in the app's
    resource; a received `webcpp::document::document` is an image of its
    own, from the global `operator new`, as document's owned documents are.
    An owning pmr binary payload is moved when its message buffer and the
    room use the same resource, and copied once otherwise; chunked transfers
    assemble their buffer in the room's resource. A received
    std::pmr::string always copies the text once into the room's resource.
    `encode` takes the resource too, for what keeps the payload alive.
  - **The protocol** (`protocol/`) takes a `std::pmr::memory_resource*` as
    the last parameter of every function that returns what it allocates,
    with `std::pmr::get_default_resource()` as its default, and returns
    `std::pmr` types; `protocol::nostr::verify` uses
    `secp256k1_context_static` and allocates nothing.
  - **Outside the resource**, stated on the page: libdatachannel's and
    OpenSSL's own allocations, with the copy libdatachannel's C API makes of
    a message before webcpp::webrtc reads it; `emscripten::val`'s JavaScript
    heap; webcpp::document's working state while it reads or writes a JSON
    text (its parser's stack, path and pending text, its tree's record of
    each change, its writer's stack), each piece freed before the call
    returns; Asio's own once-per-context allocations
    (its services and the scheduler's first operation); the wrapper Asio puts
    around a post through an executor that is not an `io_context`'s, which
    webcpp::webrtc's completions get, through `any_io_executor`;
    webcpp::webrtc's posts from libdatachannel's threads, with the copies of
    what they carry, in Asio's default allocator, since the resource is
    touched only on the room's executor; natively, for each relay
    connection, what Boost.Beast, Asio and OpenSSL allocate of their own,
    which takes no allocator: the streams' state, the WebSocket's write
    buffer, Asio's two TLS buffers, the resolver's state and results, the
    handshake's request and response with their fields, and a non-101
    response's body up to Beast's default 8 MiB, which no option of its
    stream lowers, the verify callback and its copy of the host, the timers'
    waits and each keep-alive ping's frame, while the WebSocket's
    operations, read buffer and queued texts are in the room's resource and
    a message allocates nothing outside it (in a page the stream's own state
    is there too, made with `std::allocator_arg` and the room's allocator);
    the payload types `std::string`, `bytes`, `webcpp::document::document`
    and `payload`, with `std::allocator` or document's own `operator new`;
    on Emscripten, the C library's `select()`, which Asio's reactor calls on
    every wait of an io_context with a timer or a socket, and which callocs
    an array on every call (the test replaces it, below), and under Asyncify
    the unwind data of every suspension of the run (the test counts it
    apart, below); and in a page, the storage of the bridge's map of pending
    Web Crypto promises, in the io_context's allocator, made for the first
    and grown when more are pending at once than it holds (the WebSocket and
    `page_call` registrations are intrusive; a socket's events are posted in
    the room's resource, a `page_call`'s in the resource it is given). The
    test's `memory_network` allocates its relays in its own resource.
  - **The proof** is `test/allocation_test.cpp`, on both targets, which
    counts the global `operator new`, replaced, and the C library's
    allocation functions, where the platform lets a program see them: glibc's
    functions are replaced and forward to `__libc_*`, Emscripten's dlmalloc
    has weak definitions the test replaces and forwards to
    `emscripten_builtin_*`, macOS reports each to `malloc_logger`, and MSVC's
    are not counted, nor any under AddressSanitizer, whose own allocator
    reports to none of these. It keeps every global allocation it counts,
    by its address, in a table of a fixed size until it is freed. Two rooms
    over the memory relays exchange a payload, a `std::pmr::string`, a
    `pmr_bytes` and a `pmr_document` in turn: for the first two
    `operator new` sees nothing, for the document exactly what writing it
    and reading its text back cost webcpp::document's working state, counted
    apart beforehand on the same value, and nothing left; the C library
    sees nothing, and the counting resources of the two configurations see
    the rooms'. Each text a room reads (a relay's event carrying an offer, a
    relay's `OK`, a signal, a handshake message, a candidate) is parsed in a
    counting resource, which holds its image until the document goes, and
    document's working state costs `operator new` at most 8 allocations a
    value and 16 more, none left after the parse. Two rooms with a password,
    a log sink and the network's state in a resource of its own are made,
    join, exchange a payload that travels in chunks, measure a round trip,
    one says goodbye, both are destroyed and the io_context is drained,
    while `operator new` sees webcpp::document's working state alone, none
    of it live at the drain, and the C library nothing, and the rooms'
    resources have nothing out once the io_context has drained. Two apps
    with two rooms each also prove one binary payload copy into a distinct
    room resource, with size-specific counters, and no extra copy when
    resources match; their global count sees only webcpp::document's working
    state, nothing live at the drain, and all resources return to zero.
    The counting resources also count a return of memory they
    never gave, a pmr object freed through the wrong resource, which must be
    none. Only the thread `main` runs on is counted: the C library reports
    allocations from every thread, and a system thread's are not the rooms'.
    Asio's once-per-context allocations are made first (its timer service,
    its reactor, the scheduler's first operation, and room in its timer queue
    for 256 waits); without them, the 6 they make would still be out at the
    drain. The replaced
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
    allocations rather than the C library's wait. Under Asyncify its
    `run_for` suspends, and Asyncify allocates the unwind data with the C
    library's `malloc` (measured: 22 in the second case before it was
    marked): the test wraps `Asyncify.allocateData` in JavaScript, which sets
    a flag while it runs, and counts what the C library allocates then
    apart, as webcpp::asio's own allocation test does.
- **The examples and their twins.** The quick start and each
  `diff_<topic>` program run natively in the lane `relayed`
  (`example/Jamfile`), each over a Nostr relay of its own on 127.0.0.1,
  which `example/run_over_relay.mjs` starts and names in
  `TRYSTERO_RELAY_URL`, and passes when it ends by itself having printed its
  `.expected`; its peers stay on the machine (a backend bound to 127.0.0.1,
  and `stun:127.0.0.1:9`). Each has its twin under `test/oracle/twins/`,
  which `test/oracle/run_twin.mjs` runs over a relay of its own: the quick
  start's agrees, and a difference's records what Trystero prints in its own
  `.expected`. Two peers of one program are two apps, and the quick start
  fixes their self ids, since a random id would print differently on every
  run; a twin's second peer is another page (`test/oracle/page.mjs`), since
  Trystero JS has one `selfId` per page, so the quick start's twin prints
  names where the program prints its ids. Without `TRYSTERO_RELAY_URL` the
  quick start meets over Trystero's public relays and STUN servers, as a
  user's program does.
  The chat page alone is a `webcpp.link`, linked and never run, without a
  twin (`test/oracle/twins/without-twin.txt`); its site is written by
  `b2 toolset=emscripten libs/trystero/example/chat//site` under the build
  directory, never in the tree.
