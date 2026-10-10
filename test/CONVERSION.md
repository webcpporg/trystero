# trystero's tests: the conversion to lightweight_test

trystero's tests came from the C++ Trystero repository this library was moved from (`main`,
`d3f4f9a`): the six Boost.Test files of `test/unit/`, built by that repository's `run` rule
against the compiled `boost_unit_test_framework`, natively only. They are now Boost.Core
lightweight_test programs (`<boost/core/lightweight_test.hpp>`), one per file, which
`test/Jamfile` declares with `webcpp.run`: `protocol` and `room` on native and emscripten, the
native backend's four natively only. This file records what each old test checked and where the
new one checks it, so that the conversion can be reviewed assertion by assertion.

## The programs

| Old file | New file | New target | Targets |
| --- | --- | --- | --- |
| `test/unit/protocol_test.cpp` | `test/protocol_test.cpp` | `protocol` | native, emscripten |
| `test/unit/room_test.cpp` | `test/room_test.cpp` | `room` | native, emscripten |
| `test/unit/native_sealer_test.cpp` | `test/native_sealer_test.cpp` | `native_sealer` | native |
| `test/unit/native_websocket_test.cpp` | `test/native_websocket_test.cpp` | `native_websocket` | native |
| `test/unit/native_peer_connection_test.cpp` | `test/native_peer_connection_test.cpp` | `native_peer_connection` | native |
| `test/unit/native_room_test.cpp` | `test/native_room_test.cpp` | `native_room` | native |

The fixture `test/unit/fixtures/expected.json`, which the original's oracle writes from Trystero
JS 0.26, is now `test/fixtures/expected/expected.json`, byte for byte. `protocol` and
`native_sealer` receive its path as their one argument (`<testing.input-file>`), as the old
`run` rule passed it.

`native_peer_connection` and `native_room` declare `<exception-handling>on`: they need
`native::peer_connection` and `native::backend`, which exist only with exceptions, since
libdatachannel reports errors by throwing. `protocol`, `room`, `native_sealer` and
`native_websocket` pass built with `exception-handling=off`.

The old suite had no variant without exceptions. The new one runs the browser backend's
recovery tests a second time without exceptions (`browser_*_no_exceptions`), since whether a
build has exceptions is the user's choice and a library supports both (owner decision,
2026-10-10). The lint analyses each test a second time without exceptions, except the two
that declare them.

## How each form was converted

| Old | New | Why |
| --- | --- | --- |
| `BOOST_TEST_MODULE`, `BOOST_AUTO_TEST_CASE(name)` | `void name()` in the file's anonymous namespace, called from `main` in the old declaration order; `main` returns `boost::report_errors()` | Boost.Test ran a file's cases in declaration order. |
| `BOOST_TEST(a == b)`, operands that print | `BOOST_TEST_EQ(a, b)` | Both print the two values on failure. |
| `BOOST_TEST(a == b)`, operands that are collections (a `std::vector` or a `std::span`) | `BOOST_TEST_ALL_EQ(a.begin(), a.end(), b.begin(), b.end())`, each operand that is a temporary first bound to a local | Boost.Test compared two collections element by element and printed them on failure; lightweight_test's `BOOST_TEST_EQ` needs an `operator<<`, which a vector has not, and `BOOST_TEST_ALL_EQ` is the form that compares the sizes and the elements and prints the mismatches. It needs iterators of one object, hence the locals. Twenty-four assertions, listed below. |
| `BOOST_TEST(a == b)` on `frame_kind`, `payload_format` and `envelope::kind` | `BOOST_TEST(a == b)` | The old file declared the three with `BOOST_TEST_DONT_PRINT_LOG_VALUE`: they have no `operator<<`, so `BOOST_TEST_EQ` would not compile. The declarations are gone. |
| `BOOST_TEST((expr))` | `BOOST_TEST(expr)` | The old suite wrapped in parentheses the comparisons of an `error_code` with a `trystero::error`, which Boost.Test could not print. One wrapped comparison has operands that print, `(1000 - rate_limit_next) < (12)`, and became `BOOST_TEST_LT`. |
| `BOOST_TEST(a >= b)`, `BOOST_TEST(a <= b)`, operands that print | `BOOST_TEST_GE(a, b)`, `BOOST_TEST_LE(a, b)` | As for `==`: both print the two values. One each, in `room` and `native_peer_connection`. |
| `BOOST_TEST(x)` | `BOOST_TEST(x)` | |
| `BOOST_TEST_REQUIRE(x)` in a test case | `if (!BOOST_TEST(x)) { return; }` | In Boost 1.92 `BOOST_TEST` returns `bool`. The case ends, as Boost.Test ended it. |
| `BOOST_TEST_REQUIRE(x)` in a helper | `require(BOOST_TEST(x))`, from `test/require.hpp` | A helper cannot return from its caller's case, and a program built without exceptions cannot throw out of it, so `require` ends the program with `std::exit(boost::report_errors())`, after `BOOST_TEST` has reported the failure. This differs on the failure path only: the cases after it do not run, where Boost.Test ran them. The verdict is the same. Four sites: `run` of `native_sealer`, and the three completion handlers of `pair::connect` in `native_peer_connection`. |
| `BOOST_TEST_REQUIRE(suite.argc == 2, "usage: ...")` in the helper that loads the fixture, with `master_test_suite().argv[1]` | `if (!BOOST_TEST_EQ(argc, 2)) { ... return boost::report_errors(); }` in `main`, which prints the usage line and reads `argv[1]`; the cases that read the fixture take it as a parameter | The fixture was a function-local static, loaded by the first case that asked for it; it is now loaded once by `main`, so no static state is left. Two sites: `protocol` and `native_sealer`. |
| `BOOST_CHECK_THROW`, fixtures, data cases | (none) | None of the six files uses them. The one fixture-like `struct fixture` of `room` and `native_room` is a plain struct each case makes, in both versions. |

These are the macros of each file, old and new. Each old `BOOST_TEST_REQUIRE` became one
`if`-return or one `require`, and each assertion stayed one assertion:

| File | Old `BOOST_TEST_REQUIRE` | Old `BOOST_TEST` | New `if (!BOOST_TEST...)` | New `require(BOOST_TEST...)` | New `BOOST_TEST` | New `_EQ` | New `_ALL_EQ` | New `_LT`, `_LE`, `_GE` |
| --- | --: | --: | --: | --: | --: | --: | --: | --: |
| `protocol_test.cpp` | 2 | 80 | 2 | 0 | 27 | 39 | 16 | 0 |
| `room_test.cpp` | 0 | 47 | 0 | 0 | 28 | 12 | 5 | 2 |
| `native_sealer_test.cpp` | 3 | 9 | 2 | 1 | 7 | 5 | 0 | 0 |
| `native_websocket_test.cpp` | 5 | 17 | 5 | 0 | 13 | 8 | 1 | 0 |
| `native_peer_connection_test.cpp` | 7 | 13 | 4 | 3 | 15 | 3 | 1 | 1 |
| `native_room_test.cpp` | 4 | 12 | 4 | 0 | 12 | 3 | 1 | 0 |
| **all six** | **21** | **178** | **17** | **4** | **102** | **70** | **24** | **3** |

The new columns from `BOOST_TEST` on include those inside an `if`-return or a `require`. The old
files also held 6 `BOOST_TEST_MODULE` and 3 `BOOST_TEST_DONT_PRINT_LOG_VALUE`, which assert
nothing and are gone, and no other Boost.Test macro.

The collections compared with `BOOST_TEST_ALL_EQ`: in `protocol`, the derived relays and the
shuffled order (`relays`), the six inline frames (`inline_frames`), the offer, each chunk and
the rejection (`chunked_frames`), the three envelope frames and the room frame's payload
(`envelopes`), and the decoded base64 (`text`); in `room`, the peers and the picture
(`two_peers_join_and_exchange_every_payload_format`), the two large payloads
(`large_payloads_travel_in_chunks_both_ways`) and the two received values
(`async_receive_works_in_a_coroutine`); the received texts in `native_websocket`, the bytes
the answerer received in `native_peer_connection`, and the file in `native_room`. The payload
of the room frame was compared as `tp::bytes(decoded->payload.begin(), decoded->payload.end())
== action`; it is now compared as the span itself, `decoded->payload`, against `action`.

## How the assertions were counted

Two counts were taken, and both agree, case by case.

- **Static**, in the source. Comments and literals are blanked, then every assertion macro is
  attributed to the outermost definition that holds it: a lambda counts in the function around
  it, a member function in its struct, and a helper on its own row.
  - Old: `BOOST_TEST`, `BOOST_TEST_REQUIRE`, `BOOST_CHECK*`, `BOOST_REQUIRE*` and `BOOST_WARN*`.
  - New: `BOOST_TEST`, `BOOST_TEST_*` and `BOOST_ERROR`.

  Each file's total can be derived again at any time from the old files at `d3f4f9a` and the
  new ones here: 82, 47, 12, 22, 20 and 16, old and new, 199 in all.

      grep -oE '\bBOOST_(TEST|TEST_REQUIRE|CHECK[A-Z_]*|REQUIRE[A-Z_]*|WARN[A-Z_]*)[[:space:]]*\(' <old file> | wc -l
      grep -oE '\bBOOST_(TEST(_[A-Z_]+)?|ERROR)[[:space:]]*\(' <new file> | wc -l

- **At run time**, the assertions each case executes, including those of the helpers it calls.
  - Old: Boost.Test's own count, from `--report_level=detailed`. Each old file was built by
    hand in a scratch directory with Apple clang 21, Boost 1.92's header-only Boost.Test
    (`<boost/test/included/unit_test.hpp>`), Boost.JSON's definitions, and the same
    libsecp256k1, libdatachannel and OpenSSL as the new build.
  - New: each program was built in a scratch directory with a header forced in (`-include`)
    that wraps `BOOST_TEST`, `BOOST_TEST_EQ`, `_LT`, `_LE`, `_GE` and `_ALL_EQ` in a counter,
    and with each call of a case in `main` followed by a line that prints the counter and
    resets it.
  - The runtime counts, 250 old and new, are a record made once, when this file was written.
    The counting header was a scratch tool and is not kept.
  - The check of the fixture's argument ran in the first case that loaded the fixture, in
    `topics` and `opens_what_trystero_js_sealed`, and is counted there in the old report; the
    new one runs in `main` just before those cases, and the counter, reset only after each case,
    counts it there too. In the static count it is on the helper's row, old, and on `main`'s,
    new.
  - Two cases loop over what the rooms reported (`for (const auto& ec : sent)` and
    `for (const auto& ec : errors)` in `room`), so their runtime count depends on how many
    errors a run reports; both runs counted the same.

## The tables

A helper's assertions are counted on its own row in the static count. At run time, they are
counted in the case that calls them, as Boost.Test's report counts them. Helpers without an
assertion are not listed.

### protocol_test.cpp

| Old case | New function | Static, old | Static, new | Run, old | Run, new |
| --- | --- | --: | --: | --: | --: |
| `topics` | same | 5 | 5 | 25 | 25 |
| `relays` | same | 3 | 3 | 10 | 10 |
| `handshake_and_sealing` | same | 8 | 8 | 9 | 9 |
| `nostr_events` | same | 10 | 10 | 14 | 14 |
| `inline_frames` | same | 14 | 14 | 14 | 14 |
| `chunked_frames` | same | 12 | 12 | 18 | 18 |
| `envelopes` | same | 11 | 11 | 11 | 11 |
| `signals` | same | 12 | 12 | 12 | 12 |
| `text` | same | 6 | 6 | 6 | 6 |
| helper `fixture` | `main` | 1 | 1 | in `topics` | in `topics` |
| **9 cases** | **9 functions** | **82** | **82** | **119** | **119** |

### room_test.cpp

| Old case | New function | Static, old | Static, new | Run, old | Run, new |
| --- | --- | --: | --: | --: | --: |
| `two_peers_join_and_exchange_every_payload_format` | same | 12 | 12 | 14 | 14 |
| `large_payloads_travel_in_chunks_both_ways` | same | 7 | 7 | 7 | 7 |
| `ping_measures_a_round_trip` | same | 6 | 6 | 6 | 6 |
| `the_same_password_joins_and_another_does_not` | same | 5 | 5 | 6 | 6 |
| `leaving_is_seen_by_the_others_and_a_return_reconnects` | same | 4 | 4 | 4 | 4 |
| `three_peers_form_a_mesh` | same | 3 | 3 | 3 | 3 |
| `async_receive_works_in_a_coroutine` | same | 3 | 3 | 3 | 3 |
| `view_handlers_read_the_frame_without_copying` | same | 5 | 5 | 5 | 5 |
| `a_rate_limited_relay_does_not_stop_discovery` | same | 2 | 2 | 2 | 2 |
| **9 cases** | **9 functions** | **47** | **47** | **50** | **50** |

### native_sealer_test.cpp

| Old case | New function | Static, old | Static, new | Run, old | Run, new |
| --- | --- | --: | --: | --: | --: |
| `opens_what_trystero_js_sealed` | same | 2 | 2 | 4 | 4 |
| `seals_in_trystero_form_and_opens_it_back` | same | 6 | 6 | 8 | 8 |
| `another_key_is_a_wrong_password` | same | 2 | 2 | 4 | 4 |
| helper `sealed_fixture` | `main` | 1 | 1 | in `opens_what_trystero_js_sealed` | in `opens_what_trystero_js_sealed` |
| helper `run` | same | 1 | 1 | in its callers | in its callers |
| **3 cases** | **3 functions** | **12** | **12** | **16** | **16** |

### native_websocket_test.cpp

| Old case | New function | Static, old | Static, new | Run, old | Run, new |
| --- | --- | --: | --: | --: | --: |
| `urls_name_a_scheme_host_port_and_path` | same | 12 | 12 | 12 | 12 |
| `opens_sends_in_order_receives_and_closes_once` | same | 5 | 5 | 5 | 5 |
| `an_unreachable_relay_closes_without_opening` | same | 2 | 2 | 2 | 2 |
| `a_dropped_socket_reports_nothing` | same | 1 | 1 | 1 | 1 |
| `closing_right_after_a_send_lets_the_write_finish_first` | same | 2 | 2 | 2 | 2 |
| **5 cases** | **5 functions** | **22** | **22** | **22** | **22** |

### native_peer_connection_test.cpp

| Old case | New function | Static, old | Static, new | Run, old | Run, new |
| --- | --- | --: | --: | --: | --: |
| `two_connections_open_a_data_channel_and_exchange_bytes` | same | 8 | 8 | 15 | 15 |
| `closing_one_end_is_seen_by_the_other` | same | 3 | 3 | 10 | 10 |
| `a_malformed_candidate_fails_without_throwing` | same | 2 | 2 | 2 | 2 |
| helper `pair` (`connect`) | same | 7 | 7 | in its callers | in its callers |
| **3 cases** | **3 functions** | **20** | **20** | **27** | **27** |

### native_room_test.cpp

| Old case | New function | Static, old | Static, new | Run, old | Run, new |
| --- | --- | --: | --: | --: | --: |
| `native_rooms_join_and_exchange_every_format` | same | 11 | 11 | 11 | 11 |
| `another_password_never_joins` | same | 3 | 3 | 3 | 3 |
| `leaving_is_seen_over_a_real_connection` | same | 2 | 2 | 2 | 2 |
| **3 cases** | **3 functions** | **16** | **16** | **16** | **16** |

In total, 32 cases, with 199 assertions in the source (178 `BOOST_TEST` and 21
`BOOST_TEST_REQUIRE`, old) and 250 at run time, both old and new.

## Differences, case by case

No count differs, in any case or helper. These are the other differences:

- **The names.** The namespace is `webcpp::trystero`, which each file aliases as `trystero`;
  the headers are `<webcpp/trystero/...>`; the memory backend is included as
  `"support/memory_backend.hpp"`. The rooms' app ids `trystero-cpp-tests` and
  `trystero-cpp-native-tests` are `webcpp-trystero-tests` and `webcpp-trystero-native-tests`,
  as the examples' app ids were renamed: an app id is the program's choice, and these named the
  repository the tests came from. The rooms of a test all use the same one, so nothing they
  check depends on it. The environment variable that prints the rooms' log in `room` is
  `WEBCPP_TEST_TRYSTERO_VERBOSE`, for `TRYSTERO_TEST_VERBOSE`.
- **`signals`** kept the JSON document its views point into in a function-local `static` of
  the lambda `read`; it is now a local of the case, which the lambda captures by reference. The
  views live as long as before, the case's end, and no static state is left.
- **The collections** are compared by `BOOST_TEST_ALL_EQ`, each temporary operand first bound
  to a local (the conversion table above).
- **Each `io.run_for`** says why it may block, after `lint-run:`, as the lint asks of a program
  whose purpose is network I/O: the memory relays and the rooms' timers post to the
  `io_context` (`room`), the client and the echo server do loopback I/O (`native_websocket`),
  and libdatachannel's threads post the connections' events (`native_peer_connection`,
  `native_room`). `native_sealer`'s `run` called `io.run()`; it now calls `io.poll()`, which
  runs the completion the sealer, working synchronously, has already posted, and waits for no
  I/O.
- **`native_websocket`'s echo server** awaited with `use_awaitable`, so a closed connection
  ended its coroutines by an exception, and built without exceptions the test aborted
  ("make_exception_ptr was called in -fno-exceptions mode"). It now awaits
  `as_tuple(use_awaitable)` and returns on the first error, so the test builds and passes both
  ways. What the client sees is unchanged.
- **`native_peer_connection` and `native_room` bind every connection to 127.0.0.1**, through
  the native backend's `backend_config::bind_address` (`peer_connection::create`'s
  `bind_address`). ICE never offers a loopback host candidate (RFC 8445, 5.1.1.1) unless the
  agent is bound to it, so the old tests connected over a non-loopback interface of the
  machine, and failed in a container with no network beyond the loopback
  (`docker run --network none`). Bound, both pass there.
- **A failed `BOOST_TEST_REQUIRE` in a helper** (`run` of `native_sealer`, the three
  completion handlers of `pair::connect`) now ends the program; before, it ended only its case.
  No such check fails while the library is correct, and none failed on the planted defects
  below.
- **A missing or extra argument** to `protocol` or `native_sealer` now fails in `main`, before
  any case runs, with the usage line printed after the failure; before, it failed the first
  case that loaded the fixture, and every later case that read it threw.
- **An exception that escapes a case** ends the program: the tests read `.at()` of a JSON
  object, `std::get` of a variant and `value()`-like dereferences, any of which throws, or is
  undefined, when what the test expects is not there. Boost.Test's execution monitor caught
  such an exception, reported it with the case's name and ran the remaining cases; now it
  leaves `main`, and `std::terminate` ends the program. The verdict is the same, since the exit
  status is not zero. None of these throws happens while the library is correct, and none
  happened on the planted defects below.
- **`protocol` and `room` also run on emscripten**, under node, where the old suite ran them
  natively only.

## Planted defects

Each defect was planted in a scratch copy of the original's headers, against which the old
suite was built by hand and run; and in this repository's headers, against which the new suite
was built from scratch (`b2 -a`, native clang) and run, the headers then restored from git.

| Defect | Planted in | Old tests that fail | New tests that fail | After the revert |
| --- | --- | --- | --- | --- |
| (1) the root topic built from `"Trystero:"` instead of `"Trystero@"` | `protocol/topics.hpp`, `root_topic` | `protocol`: `topics`, 4 failed assertions of 119, the four root topics of the fixture | `protocol`: `topics`, the same 4 failed assertions, at the same values | every test passes |
| (2) a room that does not answer the password challenge | `room.hpp`, `on_handshake_data`: the responder never sends `"__trystero_pw":"response"` | `room`: `the_same_password_joins_and_another_does_not`, 2 failed assertions (the join, then `a.peers().size() == 1`); `native_room`: `native_rooms_join_and_exchange_every_format`, its required join, which ends the case | `room`: the same case, the same 2 assertions; `native_room`: the same case, the same required join, which returns | every test passes |
| (3) a sealer whose `open` skips the GCM tag check | `native/sealer.hpp`, `aes_gcm`: no `EVP_CTRL_GCM_SET_TAG` on decryption, and `EVP_CipherFinal_ex`'s result ignored | `native_sealer`: `another_key_is_a_wrong_password`, 1 failed assertion (the intruder's key opens the text); `native_room`: `another_password_never_joins`, 1 failed assertion (`!errors.empty()`) | `native_sealer`: the same case and assertion; `native_room`: the same case and assertion | every test passes |

In every planted run, every other test passed, in both suites.
