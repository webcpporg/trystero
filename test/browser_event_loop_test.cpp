// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the browser's event loop, under node, against exceptions that escape a callback, with
// WebAssembly exceptions and without them (exception-handling=off):
//
// - a callback that posts work, then throws, from an event of the page: the exception is reported
//   and stops at the function the page called, and the posted work still runs;
// - 3000 such events in a row: WebAssembly's stack pointer is where it was before them, and the
//   loop still serves an event and runs posted work after them. Before the fix each escape left
//   the frames' stack behind (128 bytes without exceptions), the stack ran past its end after
//   about 500, and the loop silently stopped;
// - a recovery from a nested entry, which the page's function makes with the depth it found
//   there, keeps the depth of the call still running, and the loop goes on polling.
//
// main starts the first step and returns with node's exit code at 1; each step starts from the
// previous one's posted work, the last sets the exit code to `boost::report_errors()`, and a
// deadline fails by name if the chain stops. Node exits once nothing is pending.

#include <webcpp/trystero/browser/event_loop.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript exports

#include <emscripten/em_asm.h>
#include <emscripten/stack.h>
#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>  // lint-world: posts handlers only
#include <boost/core/lightweight_test.hpp>

#include <array>
#include <cstdint>
#include <cstring>

namespace trystero = webcpp::trystero;
using emscripten::val;

namespace {

constexpr int escapes = 3000;

struct state {
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    val deadline = val::undefined();
    std::uintptr_t stack_before = 0;
    std::uintptr_t stack_after = 0;
    int served_after = 0;
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

/** Ends the test. */
void verdict() {
    val::global("clearTimeout")(the_state().deadline);
    set_exit_code(boost::report_errors());
}

/**
 * A recovery from a nested entry: inside a callback, the depth is the callback's own, and
 * recovering to it, as the page's function does when a nested call escapes, leaves the depth the
 * running call brings back down when it returns.
 */
void nested_recovery() {
    after(0, [](int /*slot*/, const val& /*event*/) {
        auto& loop = the_state().loop;
        BOOST_TEST_EQ(loop.depth(), 1);
        loop.recover(loop.depth());
        BOOST_TEST_EQ(loop.depth(), 1);
        boost::asio::post(loop.get_executor(), [] {
            BOOST_TEST_EQ(the_state().loop.depth(), 1);  // the poll that runs it
            verdict();
        });
    });
}

/** A callback with a frame of 4 KiB, which throws from inside it. */
__attribute__((noinline)) void throw_from_deep(char* sink) {
    std::array<char, 4096> buffer{};
    std::memset(buffer.data(), 1, buffer.size());
    sink[0] = buffer[100];
    val::global("JSON").call<val>("parse", val("not JSON at all"));
    sink[1] = buffer[200];
}

/**
 * 3000 escapes in a row, each from an event of the page; the stack pointer is measured from the
 * same depth before and after them, and the loop must still serve an event and run posted work.
 */
void many_escapes() {
    auto& loop = the_state().loop;
    static std::array<char, 2> sink{};
    const int throwing =
        loop.add([](int /*slot*/, const val& /*event*/) { throw_from_deep(sink.data()); });
    // One registration measures before and after, so that both measures come from one frame.
    const int measure = loop.add([](int /*slot*/, const val& /*event*/) {
        auto& s = the_state();
        if (s.stack_before == 0) {
            s.stack_before = emscripten_stack_get_current();
            return;
        }
        s.stack_after = emscripten_stack_get_current();
        ++s.served_after;
        boost::asio::post(s.loop.get_executor(), [] {
            const auto& s = the_state();
            BOOST_TEST_EQ(s.served_after, 1);
            BOOST_TEST_EQ(s.stack_after, s.stack_before);
            nested_recovery();
        });
    });
    val::global().set("trysteroTestThrowing", loop.handler(throwing, 0));
    val::global().set("trysteroTestMeasure", loop.handler(measure, 0));
    // The escapes are reported with console.error in node; counted here rather than printed.
    // EM_ASM spaces its tokens apart: no arrow function, no strict comparison.
    // clang-format off
    EM_ASM({
        const escapes = $0;
        setTimeout(function() {
            let reported = 0;
            const error = console.error;
            console.error = function() { ++reported; };
            globalThis.trysteroTestMeasure(null);
            for (let i = 0; i < escapes; ++i) {
                globalThis.trysteroTestThrowing(null);
            }
            console.error = error;
            if (reported != escapes) {
                console.log("reported " + reported + " of " + escapes + " escapes");
            }
            globalThis.trysteroTestMeasure(null);
        }, 0);
    }, escapes);
    // clang-format on
}

}  // namespace

int main() {
    // An event whose callback posts work, then throws: JSON.parse of a text that is not JSON, as
    // the backend once did with a peer's candidate.
    after(0, [](int /*slot*/, const val& /*event*/) {
        boost::asio::post(the_state().loop.get_executor(), many_escapes);
        val::global("JSON").call<val>("parse", val("not JSON at all"));
    });
    the_state().deadline = after(10'000, [](int /*slot*/, const val& /*event*/) {
        BOOST_ERROR("the steps stopped before the verdict");
        set_exit_code(boost::report_errors());
    });
    set_exit_code(1);  // until the verdict
    return 0;
}
