// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_BROWSER_JS_HPP
#define WEBCPP_TRYSTERO_BROWSER_JS_HPP

#include <webcpp/trystero/config.hpp>

#include <emscripten/val.h>

#include <string>

/**
 * The browser backend's calls into JavaScript that can throw, made so that no exception enters
 * WebAssembly: one JavaScript function, which `browser/src.hpp` defines with `EM_JS`, runs the
 * operation inside `try` and returns what it came to, a value or the exception, as a value.
 *
 * Tip: an exception thrown into WebAssembly unwinds C++ code written to raise none and leaves it
 * mid-operation; without WebAssembly exceptions (`exception-handling=off`, or Emscripten's
 * JavaScript exceptions), no destructor runs on the way. The same file defines the function every
 * event of the page enters C++ through, which catches what still escapes and recovers the loop.
 */

extern "C" {

/**
 * Runs `target[member](...arguments)`, or `new target(...arguments)` when `member` is undefined,
 * and returns `{ok, value}`: what it returned, or, with `ok` false, what it threw.
 */
emscripten::EM_VAL webcpp_trystero_js_try(emscripten::EM_VAL target, emscripten::EM_VAL member,
                                          emscripten::EM_VAL arguments);

/**
 * A function of one argument that calls `trystero_dispatch(token, loop, id, slot, argument)`, and
 * on an exception puts WebAssembly's stack pointer back as it was before the call, reports the
 * exception, and calls `trystero_recover(token, loop, depth)` with the depth `trystero_depth` gave
 * before the call.
 */
emscripten::EM_VAL webcpp_trystero_js_handler(emscripten::EM_VAL token, const void* loop, int id,
                                              int slot);

/**
 * The page's census of its `RTCPeerConnection` objects, one for every program of the page, made on
 * the first call: `held`, the connections counted from their construction until the garbage
 * collector frees them, as Chromium counts them against its limit; `add(connection)`, which counts
 * one; and `onrelease`, a function of the new count called after each release, for a test. Without
 * `FinalizationRegistry` nothing is counted.
 */
emscripten::EM_VAL webcpp_trystero_js_census();
}

namespace webcpp::trystero::browser::detail {

/** What a JavaScript operation came to: what it returned, or, with `ok` false, what it threw. */
struct js_outcome {
    /** Whether the operation returned rather than threw. */
    bool ok = false;
    /** What it returned, or the exception it threw. */
    emscripten::val value = emscripten::val::undefined();
};

/** The arguments of a JavaScript call, as an array. */
template <class... Args>
emscripten::val js_arguments(const Args&... arguments) {
    emscripten::val array = emscripten::val::array();
    (array.call<void>("push", emscripten::val(arguments)), ...);
    return array;
}

/** Runs the operation `webcpp_trystero_js_try` describes, and reads its outcome. */
inline js_outcome js_try(const emscripten::val& target, const emscripten::val& member,
                         const emscripten::val& arguments) {
    const emscripten::val outcome = emscripten::val::take_ownership(
        webcpp_trystero_js_try(target.as_handle(), member.as_handle(), arguments.as_handle()));
    return {.ok = outcome["ok"].as<bool>(), .value = outcome["value"]};
}

/** Calls `object[method](arguments...)`, catching what it throws. */
template <class... Args>
js_outcome js_call(const emscripten::val& object, const char* method, const Args&... arguments) {
    return js_try(object, emscripten::val(method), js_arguments(arguments...));
}

/** Constructs `new constructor(arguments...)`, catching what it throws. */
template <class... Args>
js_outcome js_new(const emscripten::val& constructor, const Args&... arguments) {
    return js_try(constructor, emscripten::val::undefined(), js_arguments(arguments...));
}

/**
 * Reports an exception that escaped a callback to the page: `reportError` with an `Error` of
 * `message`, or `console.error` where there is no `reportError`, as in node.
 */
inline void report_escape(const emscripten::val& message) {
    const emscripten::val error = emscripten::val::global("Error").new_(message);
    const emscripten::val report = emscripten::val::global("reportError");
    if (report.typeOf().as<std::string>() == "function") {
        report(error);
    } else {
        emscripten::val::global("console").call<void>("error", error);
    }
}

}  // namespace webcpp::trystero::browser::detail

#endif
