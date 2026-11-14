// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the browser backend's sealer, under node, whose crypto.subtle is the page's Web Crypto: a
// text it seals opens again, and every completion runs after the call that started it, never
// inside it, as the backend interface says. Before the fix, three completions ran inside the call:
// opening a text that is not a sealed one, and every operation when the key cannot be imported,
// here because there is no crypto.subtle, as on a page that is not a secure context.
//
// main starts the first step and runs the io_context, which each Web Crypto promise the sealer
// awaits holds; each step starts from the completions of the one before, and the last ends the
// run. A step that does not come within 10 s fails by name.

#include <webcpp/trystero/browser/backend.hpp>

#include <webcpp/asio/io_context.hpp>

#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <webcpp/test/exit_status.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using emscripten::val;
using namespace std::chrono_literals;

namespace {

/** What a completion saw: its error, its text, and whether it ran inside the call. */
struct outcome {
    boost::system::error_code ec;
    std::string text;
    bool inline_call = false;
};

struct state {
    asio::io_context io;
    trystero::browser::backend backend{io.get_executor()};
    std::shared_ptr<trystero::backend::sealer> sealer;
    bool in_call = false;
    bool judged = false;
    std::optional<outcome> sealed;
    std::optional<outcome> opened;
    std::optional<outcome> garbage;
    std::optional<outcome> without_subtle;
    std::optional<outcome> after_failure;
};

/** A completion that records what it saw into `into`, then runs `next`. */
trystero::handler<void(boost::system::error_code, std::string_view)> record(
    state& s, std::optional<outcome>& into, void (*next)(state&)) {
    return [&s, &into, next](boost::system::error_code ec, std::string_view text) {
        into = outcome{.ec = ec, .text = std::string(text), .inline_call = s.in_call};
        next(s);
    };
}

/** Calls `f` with `in_call` raised, so that a completion run inside it says so. */
template <class F>
void call(state& s, F f) {
    s.in_call = true;
    f();
    s.in_call = false;
}

std::shared_ptr<trystero::backend::sealer> make_sealer(state& s) {
    std::array<std::uint8_t, 32> key{};
    key.fill(7);
    return s.backend.make_sealer(key, std::pmr::get_default_resource());
}

/** Checks that a completion came, after its call, with `expected`. */
void expect(const std::optional<outcome>& got, boost::system::error_code expected) {
    if (BOOST_TEST(got.has_value())) {
        BOOST_TEST(got->ec == expected);
        BOOST_TEST(!got->inline_call);
    }
}

void verdict(state& s) {
    expect(s.opened, {});
    if (s.opened) {
        BOOST_TEST_EQ(s.opened->text, "hello");
    }
    expect(s.garbage, trystero::error::wrong_password);
    expect(s.without_subtle, trystero::error::backend_failure);
    expect(s.after_failure, trystero::error::backend_failure);
    s.judged = true;
    s.io.stop();
}

/** Once the key could not be imported: an operation after the failure completes after its call. */
void open_after_failure(state& s) {
    call(s, [&s] { s.sealer->open("x$AAAA", record(s, s.after_failure, verdict)); });
}

/**
 * Without crypto.subtle, as outside a secure context: the key cannot be imported, and the first
 * operation completes after its call.
 */
void seal_without_subtle(state& s) {
    val descriptor = val::object();
    descriptor.set("value", val::object());
    val::global("Object").call<void>("defineProperty", val::global(), val("crypto"), descriptor);
    s.sealer = make_sealer(s);
    call(s, [&s] { s.sealer->seal("hello", record(s, s.without_subtle, open_after_failure)); });
}

/** Once both opens completed, the next step. */
void opened(state& s) {
    if (s.opened && s.garbage) {
        seal_without_subtle(s);
    }
}

/**
 * Once the key is imported and the text sealed: the sealed text opens, and a text that is not a
 * sealed one is refused after the call.
 */
void open_sealed(state& s) {
    if (!BOOST_TEST(!s.sealed->ec)) {
        verdict(s);
        return;
    }
    call(s, [&s] {
        s.sealer->open(s.sealed->text, record(s, s.opened, opened));
        s.sealer->open("not a sealed text", record(s, s.garbage, opened));
    });
}

}  // namespace

int main() {
    state s;
    s.sealer = make_sealer(s);
    call(s, [&s] { s.sealer->seal("hello", record(s, s.sealed, open_sealed)); });
    s.io.run_for(10s);  // lint-run: the test's event loop, held by the promises the sealer awaits
    if (!s.judged) {
        BOOST_ERROR("the steps stopped before the verdict");
    }
    return webcpp::test::exit_status(boost::report_errors());
}
