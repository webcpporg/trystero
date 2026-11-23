// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)
//
// The browser's C++ side of the interop test: the peer of peer_room.hpp in a page (cpp.html),
// built with Emscripten. The driver (run.mjs) gives the parameters in the page's URL, reads the
// events from `window.events`, and calls `window.leave`, `window.offline` and `window.online`,
// each a `platform::page_call` that posts to the io_context, which main runs for the page's life
// under a work guard. The app's log goes to `window.trysteroLog`. The URL's `program` names the
// program's JavaScript, which cpp.html loads; the peer does not read it.
#include "peer_room.hpp"

#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/platform/page.hpp>

#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>

#include <emscripten/val.h>

#include <optional>
#include <string>
#include <string_view>

using emscripten::val;
namespace trystero = webcpp::trystero;

namespace {

std::string param(std::string_view name) {
    const val value = val::global("URLSearchParams")
                          .new_(val::global("location")["search"])
                          .call<val>("get", val(std::string(name)));
    return value.isNull() ? std::string() : value.as<std::string>();
}

/** What the page keeps for its life, as long as the tab: the io_context, the backend and the
 * peer. */
struct page {
    webcpp::asio::io_context io;
    trystero::platform::backend backend{io.get_executor()};
    std::optional<interop::peer> peer;
};

page& the_page() {
    static page state;
    return state;
}

}  // namespace

int main() {
    auto& [io, backend, made] = the_page();
    auto& peer = made.emplace(
        io.get_executor(), backend, param,
        [](std::string_view event) {
            val::global("window")["events"].call<void>(
                "push", val::global("JSON").call<val>("parse", val(std::string(event))));
        },
        [](std::string_view category, std::string_view text) {
            val::global("window").call<void>("trysteroLog", trystero::platform::js_string(category),
                                             trystero::platform::js_string(text));
        });
    // Between the driver's calls the page may have no work: the guard keeps the run going.
    const auto guard = webcpp::asio::make_work_guard(io);
    const trystero::platform::page_call leave(io.get_executor(),
                                              [&peer](const val& /*argument*/) { peer.leave(); });
    const trystero::platform::page_call offline(io.get_executor(), [&peer](const val&) {
        peer.set_connectivity(trystero::relay_connectivity::offline);
    });
    const trystero::platform::page_call online(io.get_executor(), [&peer](const val&) {
        peer.set_connectivity(trystero::relay_connectivity::online);
    });
    val::global("window").set("leave", leave.function());
    val::global("window").set("offline", offline.function());
    val::global("window").set("online", online.function());
    io.run();  // lint-run: the page's program, suspended into the browser's event loop
}
