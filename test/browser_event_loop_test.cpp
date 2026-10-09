// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the browser's event loop, under node: a callback that throws a JavaScript exception, from
// an event of the page, leaves the loop working. The exception is reported to the page and stops at
// the function the page called, and the work the callback posted before it threw still runs. Before
// the fix the loop's depth stayed raised and it never polled again, so every room of the page
// stopped; without WebAssembly exceptions (exception-handling=off), no destructor brought it down.
//
// main sets the test up and returns with node's exit code at 1; the verdict comes when the posted
// work runs, or from a deadline that fails by name, and node exits once nothing is pending.

#include <webcpp/trystero/browser/event_loop.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript exports

#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>  // lint-world: posts handlers only
#include <boost/core/lightweight_test.hpp>

namespace trystero = webcpp::trystero;
using emscripten::val;

namespace {

struct state {
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    val deadline = val::undefined();
};

state& the_state() {
    static state s;
    return s;
}

/** Calls `setTimeout` with a function that runs `f` through the loop, as the page's events do. */
val after(int milliseconds, trystero::browser::event_loop::callback f) {
    auto& loop = the_state().loop;
    const int id = loop.add(std::move(f));
    return val::global("setTimeout")(loop.handler(id, 0), val(milliseconds), val::undefined());
}

/** Sets the code node exits with once nothing is pending. */
void set_exit_code(int code) {
    val::global("process").set("exitCode", val(code));
}

}  // namespace

int main() {
    // An event whose callback posts work, then throws: JSON.parse of a text that is not JSON, as
    // the backend once did with a peer's candidate.
    after(0, [](int /*slot*/, const val& /*event*/) {
        boost::asio::post(the_state().loop.get_executor(), [] {
            val::global("clearTimeout")(the_state().deadline);
            set_exit_code(boost::report_errors());
        });
        val::global("JSON").call<val>("parse", val("not JSON at all"));
    });
    the_state().deadline = after(10'000, [](int /*slot*/, const val& /*event*/) {
        BOOST_ERROR("the work posted before the exception never ran");
        set_exit_code(boost::report_errors());
    });
    set_exit_code(1);  // until the verdict
    return 0;
}
