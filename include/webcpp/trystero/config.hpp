// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The configuration of trystero, which each of its headers includes first:
 whether trystero is built without exceptions.
*/
#ifndef WEBCPP_TRYSTERO_CONFIG_HPP
#define WEBCPP_TRYSTERO_CONFIG_HPP

#include <webcpp/config.hpp>

// MrDocs parses trystero as a build with exceptions does, where the macro is
// not defined: this definition, undone at once, is the one the reference
// lists, and the parse goes on as such a build's.
#ifdef __MRDOCS__
/**
 Defined when trystero is built without exceptions: when webcpp has none (the
 compiler has none, or the developer defined `WEBCPP_NO_EXCEPTIONS` for every
 webcpp library), or when the developer defines it alone, to disable trystero's
 in a build that has them.

 trystero raises no exception of its own: every operation that can fail
 completes with an error code, and every part of it, the platform backend
 included, works with exceptions and without them. Its WebRTC is
 webcpp::webrtc's, which natively calls libdatachannel's C API, where no
 exception crosses.

 @see "The platform backend", in the guide.
*/
#define WEBCPP_TRYSTERO_NO_EXCEPTIONS
#undef WEBCPP_TRYSTERO_NO_EXCEPTIONS
#endif

#if !defined(WEBCPP_HAS_EXCEPTIONS) && !defined(WEBCPP_TRYSTERO_NO_EXCEPTIONS)
#define WEBCPP_TRYSTERO_NO_EXCEPTIONS
#endif

#endif  // WEBCPP_TRYSTERO_CONFIG_HPP
