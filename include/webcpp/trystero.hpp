// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The apps and rooms of trystero, which build on every target: include this one
 header to make an app over a backend and join its rooms.

 It leaves out the two backends, each with a dependency of its own target:
 `<webcpp/trystero/browser/backend.hpp>`, which builds only on emscripten, and
 `<webcpp/trystero/native/backend.hpp>`, which brings libdatachannel and
 OpenSSL.
*/
#ifndef WEBCPP_TRYSTERO_HPP
#define WEBCPP_TRYSTERO_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/json_resource.hpp>
#include <webcpp/trystero/options.hpp>
#include <webcpp/trystero/room.hpp>

#endif
