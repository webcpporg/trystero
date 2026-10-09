// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// What a test's helper does with a check its caller cannot go on without. A test case that fails
// such a check itself returns (`if (!BOOST_TEST(...)) { return; }`); a helper cannot return from
// its caller, and a lightweight_test program may be built without exceptions, so the helper ends
// the program instead, with the errors counted so far.

#ifndef WEBCPP_TEST_TRYSTERO_REQUIRE_HPP
#define WEBCPP_TEST_TRYSTERO_REQUIRE_HPP

#include <boost/core/lightweight_test.hpp>

#include <cstdlib>

namespace webcpp::test {

// Ends the program as main would end it, with the errors counted so far, unless `held`, which is
// what BOOST_TEST returned: BOOST_TEST has already reported the failure.
inline void require(bool held) {
    if (!held) {
        std::exit(boost::report_errors());
    }
}

}  // namespace webcpp::test

#endif  // WEBCPP_TEST_TRYSTERO_REQUIRE_HPP
