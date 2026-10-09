// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// <webcpp/trystero/config.hpp> defines WEBCPP_TRYSTERO_NO_EXCEPTIONS exactly when the
// build has no exceptions, and keeps the developer's own definition in a build
// that has them. test/Jamfile builds this file twice: as the build asks, and
// with the macro defined as a developer defines it, on the command line, which
// WEBCPP_TEST_TRYSTERO_DEVELOPER_DEFINED tells this file.

#include <webcpp/trystero/config.hpp>

// Boost.Config, which config.hpp includes, has said whether the build has
// exceptions: without it, BOOST_NO_EXCEPTIONS would never be defined, and the
// checks below would pass in a build without exceptions too.
#if defined(__GNUC__) && defined(__cpp_exceptions) == defined(BOOST_NO_EXCEPTIONS)
#error "config.hpp does not include <boost/config.hpp>, which defines BOOST_NO_EXCEPTIONS"
#endif

#ifdef WEBCPP_TEST_TRYSTERO_DEVELOPER_DEFINED
#ifndef WEBCPP_TRYSTERO_NO_EXCEPTIONS
#error "config.hpp drops WEBCPP_TRYSTERO_NO_EXCEPTIONS, which the developer defined"
#endif
#elif defined(BOOST_NO_EXCEPTIONS) != defined(WEBCPP_TRYSTERO_NO_EXCEPTIONS)
#error "WEBCPP_TRYSTERO_NO_EXCEPTIONS is not defined exactly when the build has no exceptions"
#endif
