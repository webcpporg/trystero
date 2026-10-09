// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_BROWSER_SRC_HPP
#define WEBCPP_TRYSTERO_BROWSER_SRC_HPP

#include <webcpp/trystero/config.hpp>

/**
 * What a browser program compiles once: the exports JavaScript calls (`trystero_dispatch` and
 * `trystero_recover`), and the JavaScript functions the backend calls through (browser/js.hpp).
 * Include it in exactly one source file of the program.
 *
 * Tip: Boost.JSON's definitions are not here: a program links them from /webcpp//boost_json, which
 * /webcpp/trystero//trystero brings and which builds on emscripten as on every target.
 */
#include <webcpp/trystero/browser/event_loop.hpp>
#include <webcpp/trystero/browser/js.hpp>

#include <emscripten/bind.h>
#include <emscripten/em_js.h>

// The JavaScript functions of browser/js.hpp. Emval turns the handles C++ passes into values and
// back, and stackSave and stackRestore keep WebAssembly's stack pointer; EM_JS_DEPS has Emscripten
// link them. A native parse (the reference, the lint) has
// no JavaScript to define, nor a section EM_JS can name.
#ifdef __EMSCRIPTEN__
EM_JS_DEPS(webcpp_trystero, "$Emval,$stackSave,$stackRestore");

// clang-format off
EM_JS(emscripten::EM_VAL, webcpp_trystero_js_try,
      (emscripten::EM_VAL target, emscripten::EM_VAL member, emscripten::EM_VAL arguments), {
    const object = Emval.toValue(target);
    const name = Emval.toValue(member);
    const values = Emval.toValue(arguments);
    try {
        const value =
            name === undefined ? Reflect.construct(object, values) : object[name](...values);
        return Emval.toHandle({ok: true, value});
    } catch (error) {
        return Emval.toHandle({ok: false, value: error});
    }
});

EM_JS(emscripten::EM_VAL, webcpp_trystero_js_handler,
      (emscripten::EM_VAL token, const void* loop, int id, int slot), {
    const alive = Emval.toValue(token);
    const address = loop >>> 0;
    return Emval.toHandle(event => {
        // An exception leaves WebAssembly without running its frames' epilogues: the stack
        // pointer and the loop's depth are put back as they were before the call.
        const top = stackSave();
        const depth = Module['trystero_depth'](alive, address);
        try {
            Module['trystero_dispatch'](alive, address, id, slot, event);
        } catch (error) {
            stackRestore(top);
            (globalThis.reportError ?? console.error)(error);
            Module['trystero_recover'](alive, address, depth);
        }
    });
});

EM_JS(emscripten::EM_VAL, webcpp_trystero_js_census, (), {
    const key = Symbol.for("webcpp.trystero.census");
    if (!globalThis[key]) {
        const census = {held: 0, onrelease: null};
        // Without FinalizationRegistry nothing is counted, so that the page never stops making
        // connections for a count that never comes down.
        const registry = typeof FinalizationRegistry == "function"
            ? new FinalizationRegistry(function() {
                --census.held;
                if (census.onrelease) {
                    census.onrelease(census.held);
                }
            })
            : null;
        census.add = function(connection) {
            if (registry) {
                ++census.held;
                registry.register(connection, 0);
            }
        };
        globalThis[key] = census;
    }
    return Emval.toHandle(globalThis[key]);
});
// clang-format on
#endif

EMSCRIPTEN_BINDINGS(trystero_browser) {
    emscripten::function("trystero_dispatch", &webcpp::trystero::browser::dispatch);
    emscripten::function("trystero_recover", &webcpp::trystero::browser::recover);
    emscripten::function("trystero_depth", &webcpp::trystero::browser::depth);
}

#endif
