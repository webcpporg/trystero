// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests that the platform backend in a page watches the page's `online` and `offline` events,
// under node, over a stand-in for a window's events on the global object (node's takes no
// listener): the watch adds a listener of each, each event posts its handler with `true` or
// `false` to the io_context, which runs it after the event, never inside it, and the watch's
// destruction removes both listeners, so that a later event reaches nothing.

#include <webcpp/trystero/platform/backend.hpp>

#include <webcpp/asio/browser/bridge.hpp>
#include <webcpp/asio/io_context.hpp>

#include <emscripten/em_js.h>
#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <memory_resource>
#include <vector>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;

// The stand-in: the global object takes listeners as a window does, counts those added and not
// removed, the same function, and fires an event of a type on demand.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, install_page_events, (), {
    const target = new EventTarget();
    const held = new Map();  // by type, the functions added and not removed
    globalThis.addEventListener = (type, f) => {
        if (!held.has(type)) held.set(type, new Set());
        held.get(type).add(f);
        target.addEventListener(type, f);
    };
    globalThis.removeEventListener = (type, f) => {
        if (held.has(type)) held.get(type).delete(f);
        target.removeEventListener(type, f);
    };
    globalThis.heldListeners = () => [...held.values()].reduce((n, s) => n + s.size, 0);
    globalThis.firePageEvent = type => target.dispatchEvent(new Event(type));
});
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(int, held_listeners, (), { return globalThis.heldListeners(); });
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, fire_page_event, (const char* type), { globalThis.firePageEvent(UTF8ToString(type)); });
// clang-format on

namespace {

/** Runs what is ready on `io` until nothing is. */
void settle(asio::io_context& io) {
    io.restart();
    while (io.poll() > 0) {  // lint-run: the page's events post to it
        io.restart();
    }
}

}  // namespace

int main() {
    install_page_events();
    asio::io_context io;
    trystero::platform::backend backend(io.get_executor());
    std::vector<bool> changes;
    bool inside_an_event = false;
    auto watch = backend.watch_connectivity(
        [&](bool online) {
            changes.push_back(online);
            inside_an_event =
                inside_an_event || asio::use_service<asio::browser::bridge>(io).delivering();
        },
        std::pmr::get_default_resource());
    if (!BOOST_TEST(watch != nullptr)) {
        return webcpp::test::exit_status(boost::report_errors());
    }
    BOOST_TEST_EQ(held_listeners(), 2);

    fire_page_event("offline");
    fire_page_event("online");
    BOOST_TEST(changes.empty());  // each event only posts
    settle(io);
    BOOST_TEST((changes == std::vector<bool>{false, true}));
    BOOST_TEST(!inside_an_event);

    fire_page_event("visibilitychange");  // not watched
    settle(io);
    BOOST_TEST_EQ(changes.size(), 2U);

    fire_page_event("offline");
    watch.reset();  // the posted call reaches nothing once the watch is gone
    BOOST_TEST_EQ(held_listeners(), 0);
    fire_page_event("online");
    settle(io);
    BOOST_TEST_EQ(changes.size(), 2U);
    return webcpp::test::exit_status(boost::report_errors());
}
