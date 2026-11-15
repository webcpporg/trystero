// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// How a test's main runs its cases: each in a frame of its own, which main's frame never holds.
// Under Asyncify each suspension saves every frame from main down to the call that suspends, each
// with the locals its instrumentation keeps, which a program linked without optimization, as b2's
// release variant is on emscripten, keeps whole. A case that main alone calls is inlined into it
// at -O3, so that main would keep the locals of every case at each suspension of any: the room
// test's main saved 29,776 bytes so, past the 8,192 of webcpp::asio's stack, and stopped at its
// first suspension; through run_case its deepest suspension wrote 3,096 (measured on 2026-11-15
// with Emscripten 6.0.11).

#ifndef WEBCPP_TEST_TRYSTERO_CASES_HPP
#define WEBCPP_TEST_TRYSTERO_CASES_HPP

#include <boost/config.hpp>

namespace webcpp::test {

/** A case of a test, which its main calls once. */
using test_case = void (*)();

/** Runs `run` in a frame of its own: never inlined into its caller, it calls the case through a
 * pointer, so that no case is inlined into main. */
BOOST_NOINLINE inline void run_case(test_case run) {
    run();
}

}  // namespace webcpp::test

#endif  // WEBCPP_TEST_TRYSTERO_CASES_HPP
