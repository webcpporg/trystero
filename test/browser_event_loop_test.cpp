// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the browser's event loop, under node: a callback that throws a JavaScript exception, which
// unwinds out of `trystero_dispatch` into the page, leaves the loop working. The work posted after
// it still runs. Before the fix the loop's depth stayed raised and it never polled again, so every
// room of the page stopped.
//
// main sets the test up and returns with node's exit code at 1; the verdict comes from a timer,
// which sets the exit code to `boost::report_errors()`, and node exits once nothing is pending.

#include <webcpp/trystero/browser/event_loop.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript export

#include <emscripten/em_asm.h>
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
    bool ran = false;
};

state& the_state() {
    static state s;
    return s;
}

/** Calls `setTimeout` with a function that runs `f` through the loop, as the page's events do. */
void after(int milliseconds, trystero::browser::event_loop::callback f) {
    auto& loop = the_state().loop;
    const int id = loop.add(std::move(f));
    // The handler takes one argument, which setTimeout passes when given one.
    val::global("setTimeout")(loop.handler(id, 0), val(milliseconds), val::undefined());
}

/** Sets the code node exits with once nothing is pending. */
void set_exit_code(int code) {
    val::global("process").set("exitCode", val(code));
}

}  // namespace

int main() {
    auto& loop = the_state().loop;

    // A callback that throws: JSON.parse of a text that is not JSON, as the backend once did with a
    // peer's candidate.
    const int throwing = loop.add([](int /*slot*/, const val& /*event*/) {
        val::global("JSON").call<val>("parse", val("not JSON at all"));
    });
    val::global().set("trysteroTestThrowing", loop.handler(throwing, 0));
    const int threw = EM_ASM_INT({
        try {
            globalThis.trysteroTestThrowing(null);
            return 0;
        } catch (e) {
            return 1;
        }
    });
    BOOST_TEST_EQ(threw, 1);

    // Work posted afterwards, outside every JavaScript event: the loop schedules a poll for it.
    boost::asio::post(loop.get_executor(), [] { the_state().ran = true; });

    after(200, [](int /*slot*/, const val& /*event*/) {
        BOOST_TEST(the_state().ran);
        set_exit_code(boost::report_errors());
    });
    set_exit_code(1);  // until the verdict
    return 0;
}
