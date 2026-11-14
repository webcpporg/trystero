// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the shape of a page's program, under node: main runs the io_context from the start, under
// a work guard, and the page reaches C++ through functions the program exports, which only post to
// it, as webcpp::asio's model asks (nothing runs the io_context from a JavaScript event).
//
// - A page with no room yet keeps running: nothing but the work guard holds the io_context until
//   the page's first call, 50 ms after main started it, whose posted work joins a room of each of
//   two apps over the memory relays. Without the guard, the run returns at once, having run
//   nothing, and no room is ever joined.
// - A page's call made while the run is suspended reaches the room: once the two rooms meet, the
//   page calls the second export from a JavaScript timer, and its posted work sends on the first
//   room's action, which the second room receives. Neither export runs any of that inside its call.
//
// main ends with the errors counted once the run returns; a step that does not come within 10 s
// fails by name.

#include "support/rooms.hpp"

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/room.hpp>

#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/post.hpp>

#include <emscripten/em_asm.h>
#include <emscripten/em_macros.h>
#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <webcpp/test/exit_status.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
namespace test = webcpp::trystero::test;
using namespace std::chrono_literals;

namespace {

/** What the page keeps alive: the io_context and its guard, the memory relays, two apps, a room of
 * each, and their actions. */
struct page {
    test::fixture f;
    asio::executor_work_guard<asio::io_context::executor_type> guard{f.io.get_executor()};
    std::optional<trystero::app> a_app;
    std::optional<trystero::app> z_app;
    std::optional<trystero::room> a;
    std::optional<trystero::room> z;
    std::optional<trystero::action<std::string>> chat_a;
    std::optional<trystero::action<std::string>> chat_z;
    bool in_export = false;
    bool ran_inside_an_export = false;
    bool joined = false;
    int met = 0;
    bool sent = false;
    std::string received;
};

/** Records whether posted work runs inside an export's call, which it never should. */
void handling(page& p) {
    if (p.in_export) {
        p.ran_inside_an_export = true;
    }
}

/** Schedules the second export from a JavaScript timer, once both rooms met. */
void met(page& p) {
    if (++p.met != 2) {
        return;
    }
    // A function expression: clang-format splits an arrow function's => into = >.
    EM_ASM({ setTimeout(function() { _test_page_send($0); }, 0); }, &p);
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
    p.a->on_peer_join([&p](std::string_view /*peer*/) { met(p); });
    p.z->on_peer_join([&p](std::string_view /*peer*/) { met(p); });
    p.chat_z->on_message([&p](std::string text, const trystero::message_info& /*info*/) {
        p.received = std::move(text);
        p.f.io.stop();
    });
}

}  // namespace

/** The page's first call: it posts the join, nothing more. */
extern "C" EMSCRIPTEN_KEEPALIVE void test_page_join(page* p) {
    p->in_export = true;
    asio::post(p->f.io, [p] { join(*p); });
    p->in_export = false;
}

/** The page's second call: it posts a send on the first room's action, nothing more. */
extern "C" EMSCRIPTEN_KEEPALIVE void test_page_send(page* p) {
    p->in_export = true;
    asio::post(p->f.io, [p] {
        handling(*p);
        p->chat_a->async_send("from the page", [p](boost::system::error_code ec) {
            BOOST_TEST(!ec);
            p->sent = true;
        });
    });
    p->in_export = false;
}

int main() {
    page p;
    // A function expression: clang-format splits an arrow function's => into = >.
    EM_ASM({ setTimeout(function() { _test_page_join($0); }, 50); }, &p);
    p.f.io.run_for(10s);  // lint-run: the page's program, held by its work guard
    if (!p.joined) {
        BOOST_ERROR("the run ended before the page's first call");
    } else if (p.received.empty()) {
        BOOST_ERROR("the page's send never reached the other room");
    }
    BOOST_TEST_EQ(p.received, "from the page");
    BOOST_TEST(!p.ran_inside_an_export);
    return webcpp::test::exit_status(boost::report_errors());
}
