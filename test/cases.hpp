// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// How a test's main runs its cases: each in a frame of its own, which main's frame never holds,
// whatever the link. Under Asyncify each suspension saves every frame from main down to the call
// that suspends, each with the locals its instrumentation keeps. A case that main alone calls is
// inlined into it at -O3, and a program linked without optimization, as b2 linked a release build
// until 2026-11-15 and as <linkflags>-O0 still links one, then keeps the locals of every case in
// main's frame at each suspension of any: the room test's main saved 29,776 bytes so, past the
// 8,192 of webcpp::asio's stack, and stopped at its first suspension. Through run_case its deepest
// suspension writes 3,072 bytes linked so, 564 linked at -O3 and 2,344 in a debug build (measured
// on 2026-11-15 with Emscripten 6.0.11).

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
