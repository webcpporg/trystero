// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests an app in a page without the browser's generator, under node: once with no `crypto` at
// all, and once with a `crypto.getRandomValues` that throws, as a page whose generator is missing
// or broken has it. The app cannot make its endpoint key nor its ephemeral user's, so each room it
// joins reports `error::identity_failed`, as 0.27.0's `failSetup` reports `identity-failed`, and
// no JavaScript exception enters WebAssembly: one would end main unfinished, and the run with it.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/room.hpp>
#include <webcpp/trystero/room_error.hpp>

#include <webcpp/asio/io_context.hpp>

#include <emscripten/em_js.h>
#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <optional>
#include <string>
#include <vector>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;

// The page's generator, taken away (`mode` 0: no `crypto`) or broken (1: its `getRandomValues`
// throws), and given back.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, break_generator, (int mode), {
    if (globalThis.savedCrypto === undefined) globalThis.savedCrypto = globalThis.crypto;
    const broken = mode === 0 ? undefined : {
        getRandomValues() { throw new Error('the generator is broken'); },
        subtle: globalThis.savedCrypto.subtle,
    };
    Object.defineProperty(globalThis, 'crypto', {value: broken, configurable: true, writable: true});
});
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, restore_generator, (), {
    Object.defineProperty(globalThis, 'crypto', {value: globalThis.savedCrypto, configurable: true, writable: true});
});
// clang-format on

namespace {

/** Runs what is ready on `io` until nothing is. */
void settle(asio::io_context& io) {
    io.restart();
    while (io.poll() > 0) {  // lint-run: the room's reports post to it
        io.restart();
    }
}

/** Joins a room of an app made without a generator, and checks what the room reports. */
void without_generator(int mode) {
    break_generator(mode);
    asio::io_context io;
    trystero::platform::backend backend(io.get_executor());
    trystero::app_config config;
    config.app_id = "webcpp-trystero-entropy";
    config.relay_urls.emplace_back("ws://127.0.0.1:9");
    std::optional<trystero::app> app;
    app.emplace(io.get_executor(), backend, std::move(config));
    BOOST_TEST(app->self_id().empty());
    BOOST_TEST(app->user_id().empty());
    std::vector<std::string> reports;
    auto joined = app->join("room");
    BOOST_TEST(joined.has_value());
    if (joined) {
        joined->on_error([&](const trystero::room_error& e) {
            reports.emplace_back(trystero::room_error_code_name(e.code));
        });
    }
    settle(io);
    restore_generator();
    BOOST_TEST_EQ(reports.size(), 1U);
    BOOST_TEST(!reports.empty() && reports.front() == "identity-failed");
}

}  // namespace

int main() {
    without_generator(0);
    without_generator(1);
    return webcpp::test::exit_status(boost::report_errors());
}
