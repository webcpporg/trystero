// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the shape of a page's program, under node: main runs the io_context from the start, under
// a work guard, and the page calls into C++ through `platform::page_call`, functions of
// webcpp::asio's bridge that only post to it (nothing runs the io_context from a JavaScript event).
//
// - A page with no room yet keeps running: nothing but the work guard holds the io_context until
//   the page's first call, 50 ms after main started it, whose posted work joins a room of each of
//   two apps over the memory relays. Without the guard, the run returns at once, having run
//   nothing, and no room is ever joined.
// - A page's call made while the run is suspended reaches the room: once the two rooms meet, the
//   page calls the second function with two texts from a JavaScript timer, and its posted work
//   sends each text on the first room's action, which the second room receives. A call returns
//   undefined at once, and its work runs from the run, never inside the call. Under Asyncify an
//   embind function called while main is suspended returns a promise that never settles, and its
//   second call aborts a debug build (measured with Emscripten 6.0.11): page_call is no embind
//   function.
// - A page's call with a value its handler does not expect breaks nothing: before its texts, the
//   page calls the second function with a number and with undefined, which the handler checks
//   for a string and ignores, and the texts that follow still reach the other room. Before the
//   handler checked, `as<std::string>()` threw embind's BindingError out of the run, which ended
//   the program.
// - A call of a page_call destroyed meanwhile reaches nothing.
//
// main ends with the errors counted once the run returns; a step that does not come within 10 s
// fails by name.

#include "support/rooms.hpp"

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/platform/page.hpp>
#include <webcpp/trystero/room.hpp>

#include <webcpp/asio/browser/bridge.hpp>
#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>

#include <emscripten/em_asm.h>
#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <webcpp/test/exit_status.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
namespace test = webcpp::trystero::test;
using emscripten::val;
using namespace std::chrono_literals;

namespace {

/** What the page keeps alive: the io_context and its guard, the memory relays, two apps, a room of
 * each, and their actions. */
struct page {
    test::fixture f;
    asio::executor_work_guard<asio::io_context::executor_type> guard{f.io.get_executor()};
    std::optional<trystero::test::app> a_app;
    std::optional<trystero::test::app> z_app;
    std::optional<trystero::room> a;
    std::optional<trystero::room> z;
    std::optional<trystero::action<std::string>> chat_a;
    std::optional<trystero::action<std::string>> chat_z;
    bool ran_inside_a_call = false;
    bool joined = false;
    std::size_t met = 0;
    std::vector<std::string> received;
    std::size_t ignored = 0;
    std::size_t late = 0;
};

/** Records whether posted work runs inside a call of the page's, which it never should. */
void handling(page& p) {
    if (asio::use_service<asio::browser::bridge>(p.f.io).delivering()) {
        p.ran_inside_a_call = true;
    }
}

/**
 * Calls the page's second function from a JavaScript timer, once both rooms met: with a number and
 * with undefined, then twice with a text.
 */
void met(page& p) {
    if (++p.met != 2) {
        return;
    }
    // A function expression: clang-format splits an arrow function's => into = >.
    EM_ASM({
        setTimeout(
            function() {
                globalThis.pageSend(42);
                globalThis.pageSend(undefined);
                globalThis.pageReturned = globalThis.pageSend("from the page");
                globalThis.pageSend("again");
            },
            0);
    });
}

/** Joins a room of each app, and sets their handlers. */
void join(page& p) {
    handling(p);
    p.joined = true;
    p.a_app.emplace(p.f.io.get_executor(), p.f.net, test::app_config("AAAAAAAAAAAAAAAAAAAA"));
    p.z_app.emplace(p.f.io.get_executor(), p.f.net, test::app_config("zzzzzzzzzzzzzzzzzzzz"));
    p.a.emplace(test::join(*p.a_app, "lobby"));
    p.z.emplace(test::join(*p.z_app, "lobby"));
    p.chat_a.emplace(p.a->make_action<std::string>("chat"));
    p.chat_z.emplace(p.z->make_action<std::string>("chat"));
    p.a->on_peer_join([&p](const trystero::peer_event&) { met(p); });
    p.z->on_peer_join([&p](const trystero::peer_event&) { met(p); });
    p.chat_z->on_message([&p](std::string text, const trystero::message_info& /*info*/) {
        p.received.push_back(std::move(text));
        if (p.received.size() == 2) {
            p.f.io.stop();
        }
    });
}

/**
 * Sends the page's text on the first room's action, and ignores a value that is not a string:
 * converting it would throw embind's BindingError out of the run.
 */
void send(page& p, const val& text) {
    handling(p);
    if (!text.isString()) {
        ++p.ignored;
        return;
    }
    p.chat_a->async_send(text.as<std::string>(),
                         [](boost::system::error_code ec) { BOOST_TEST(!ec); });
}

}  // namespace

int main() {
    page p;
    const trystero::platform::page_call join_call(p.f.io.get_executor(),
                                                  [&p](const val& /*argument*/) { join(p); });
    const trystero::platform::page_call send_call(p.f.io.get_executor(),
                                                  [&p](const val& text) { send(p, text); });
    {
        // A call of a page_call destroyed before its work runs reaches nothing.
        const trystero::platform::page_call gone(p.f.io.get_executor(),
                                                 [&p](const val& /*argument*/) { ++p.late; });
        gone.function()(val(1));
    }
    // clang-format off
    EM_ASM(({
        const join = Emval.toValue($0);
        globalThis.pageSend = Emval.toValue($1);
        globalThis.pageReturned = "not called";
        setTimeout(() => join(null), 50);
    }), join_call.function().as_handle(), send_call.function().as_handle());
    // clang-format on
    p.f.io.run_for(10s);  // lint-run: the page's program, held by its work guard
    if (!p.joined) {
        BOOST_ERROR("the run ended before the page's first call");
    } else if (p.received.size() != 2) {
        BOOST_ERROR("the page's sends never reached the other room");
    }
    const std::vector<std::string> expected{"from the page", "again"};
    BOOST_TEST_ALL_EQ(p.received.begin(), p.received.end(), expected.begin(), expected.end());
    BOOST_TEST(val::global("pageReturned").isUndefined());
    BOOST_TEST_EQ(p.ignored, 2U);
    BOOST_TEST(!p.ran_inside_a_call);
    BOOST_TEST_EQ(p.late, 0U);
    return webcpp::test::exit_status(boost::report_errors());
}
