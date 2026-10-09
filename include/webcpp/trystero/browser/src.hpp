// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_BROWSER_SRC_HPP
#define WEBCPP_TRYSTERO_BROWSER_SRC_HPP

#include <webcpp/trystero/config.hpp>

/**
 * What a browser program compiles once: the export JavaScript calls (`trystero_dispatch`). Include
 * it in exactly one source file of the program.
 *
 * Tip: Boost.JSON's definitions are not here: a program links them from /webcpp//boost_json, which
 * /webcpp/trystero//trystero brings and which builds on emscripten as on every target.
 */
#include <webcpp/trystero/browser/event_loop.hpp>

#include <emscripten/bind.h>

EMSCRIPTEN_BINDINGS(trystero_browser) {
    emscripten::function("trystero_dispatch", &webcpp::trystero::browser::dispatch);
}

#endif
