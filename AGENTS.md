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
  header alone on the targets that build it, in three calls.
- **The native backend's exceptions.** libdatachannel reports errors by
  throwing, and `native/data_channel.hpp` and `native/peer_connection.hpp`
  catch them, so they, and `native::backend`, which makes their objects, sit
  behind `#ifndef WEBCPP_TRYSTERO_NO_EXCEPTIONS` and do not exist without
  exceptions: code that uses them does not compile there. The native timer,
  sealer and WebSockets, and everything else of trystero, compile without
  exceptions. Every headers-alone call, test and program of the native
  backend declares `<exception-handling>on`, with the reason in a comment:
  `/webcpp/trystero//native` cannot, since b2 ignores a non-free feature among
  usage requirements, so a user's `exception-handling=off` would reach them.
  The lint names those headers as left out of its analysis without
  exceptions. trystero raises nothing of its own; a raise would go through
  `boost::throw_exception`.
- **The external dependencies.** libsecp256k1 (every room), libdatachannel
  and OpenSSL (the native backend) are not part of Boost. `build.jam` finds
  them where `SECP256K1_ROOT`, `SECP256K1_EMSCRIPTEN_ROOT`,
  `LIBDATACHANNEL_ROOT` and `OPENSSL_ROOT` say, until webcpp builds them from
  `third_party/`. A variable that is not set leaves its library to the
  compiler's default search path; one that names a directory without its
  library's header stops the build, naming the variable. libdatachannel is a
  C++ library: it must be built with the same standard library as the
  program that links it (libc++ for clang on macOS, libstdc++ for GCC), or
  the program does not link, though the native headers still compile alone.
- **embind.** Every program on emscripten links `-lembind`, which
  `/webcpp/trystero//trystero` adds as a linker flag: `<embind>on`, b2's own
  feature, is not free, and b2 ignores it among usage requirements.
- **The memory relays.** `test/support/memory_backend.hpp`, in
  `webcpp::trystero::test`, is a backend in memory: Nostr relays that forward
  ephemeral events to matching subscriptions, peer connections that pair
  through their descriptions, data channels that deliver in order, and a
  sealer that opens only what was sealed under the same key, each event
  posted to the `io_context`. A test of a whole room runs over it, on both
  targets, without the network.
- **No twins.** Every example needs the public relays or a browser, so each
  is a `webcpp.link`, linked and never run, and has no `.expected` and no
  twin in Trystero's language. The chat page's site is written by
  `b2 toolset=emscripten libs/trystero/example/chat//site` under the build
  directory, never in the tree.
