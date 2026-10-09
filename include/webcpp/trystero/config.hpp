// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The configuration of trystero, which each of its headers includes first:
 whether trystero is built without exceptions.

 @see "Targets", in the guide.
*/
#ifndef WEBCPP_TRYSTERO_CONFIG_HPP
#define WEBCPP_TRYSTERO_CONFIG_HPP

#include <boost/config.hpp>

// MrDocs parses trystero as a build with exceptions does, where the macro is
// not defined: this definition, undone at once, is the one the reference
// lists, and the parse goes on with every API that needs exceptions in it.
#ifdef __MRDOCS__
/**
 Defined when trystero is built without exceptions: automatically when the
 compiler has none (`BOOST_NO_EXCEPTIONS`), or by the developer to disable them
 in a build that has them.

 trystero raises no exception of its own: every operation that can fail
 completes with an error code. The native backend's WebRTC, over
 libdatachannel, which reports errors by throwing, needs exceptions: while this
 is defined, `native::data_channel`, `native::peer_connection` and
 `native::backend` do not exist, and the rest of trystero is unchanged.

 @see "Targets", in the guide.
*/
#define WEBCPP_TRYSTERO_NO_EXCEPTIONS
#undef WEBCPP_TRYSTERO_NO_EXCEPTIONS
#endif

#if defined(BOOST_NO_EXCEPTIONS) && !defined(WEBCPP_TRYSTERO_NO_EXCEPTIONS)
#define WEBCPP_TRYSTERO_NO_EXCEPTIONS
#endif

#endif  // WEBCPP_TRYSTERO_CONFIG_HPP
