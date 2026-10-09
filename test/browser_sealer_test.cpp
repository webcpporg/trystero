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
// main starts the first step and returns with node's exit code at 1; each step starts from the
// completions of the one before, the last sets the exit code to `boost::report_errors()`, and a
// deadline fails by name if the chain stops. Node exits once nothing is pending.

#include <webcpp/trystero/browser/backend.hpp>
#include <webcpp/trystero/browser/src.hpp>  // once per program: the JavaScript exports

#include <emscripten/val.h>
#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace trystero = webcpp::trystero;
using emscripten::val;

namespace {

/** What a completion saw: its error, its text, and whether it ran inside the call. */
struct outcome {
    boost::system::error_code ec;
    std::string text;
    bool inline_call = false;
};

struct state {
    boost::asio::io_context io;
    trystero::browser::event_loop loop{io};
    trystero::browser::backend backend{loop};
    std::shared_ptr<trystero::backend::sealer> sealer;
    bool in_call = false;
    std::optional<outcome> sealed;
    std::optional<outcome> opened;
    std::optional<outcome> garbage;
    std::optional<outcome> without_subtle;
    std::optional<outcome> after_failure;
    val deadline = val::undefined();
};

state& the_state() {
    static state s;
    return s;
}

void set_exit_code(int code) {
    val::global("process").set("exitCode", val(code));
}

/** A completion that records what it saw into `into`, then runs `next`. */
trystero::handler<void(boost::system::error_code, std::string_view)> record(
    std::optional<outcome>& into, void (*next)()) {
    return [&into, next](boost::system::error_code ec, std::string_view text) {
        into = outcome{.ec = ec, .text = std::string(text), .inline_call = the_state().in_call};
        next();
    };
}

/** Calls `f` with `in_call` raised, so that a completion run inside it says so. */
template <class F>
void call(F f) {
    the_state().in_call = true;
    f();
    the_state().in_call = false;
}

std::shared_ptr<trystero::backend::sealer> make_sealer() {
    std::array<std::uint8_t, 32> key{};
    key.fill(7);
    return the_state().backend.make_sealer(key, std::pmr::get_default_resource());
}

/** Checks that a completion came, after its call, with `expected`. */
void expect(const std::optional<outcome>& got, boost::system::error_code expected) {
    if (BOOST_TEST(got.has_value())) {
        BOOST_TEST(got->ec == expected);
        BOOST_TEST(!got->inline_call);
    }
}

void verdict() {
    const auto& s = the_state();
    expect(s.opened, {});
    if (s.opened) {
        BOOST_TEST_EQ(s.opened->text, "hello");
    }
    expect(s.garbage, trystero::error::wrong_password);
    expect(s.without_subtle, trystero::error::backend_failure);
    expect(s.after_failure, trystero::error::backend_failure);
    val::global("clearTimeout")(s.deadline);
    set_exit_code(boost::report_errors());
}

/** Once the key could not be imported: an operation after the failure completes after its call. */
void open_after_failure() {
    call([] { the_state().sealer->open("x$AAAA", record(the_state().after_failure, verdict)); });
}

/**
 * Without crypto.subtle, as outside a secure context: the key cannot be imported, and the first
 * operation completes after its call.
 */
void seal_without_subtle() {
    val descriptor = val::object();
    descriptor.set("value", val::object());
    val::global("Object").call<void>("defineProperty", val::global(), val("crypto"), descriptor);
    the_state().sealer = make_sealer();
    call([] {
        the_state().sealer->seal("hello", record(the_state().without_subtle, open_after_failure));
    });
}

/** Once both opens completed, the next step. */
void opened() {
    const auto& s = the_state();
    if (s.opened && s.garbage) {
        seal_without_subtle();
    }
}

/**
 * Once the key is imported and the text sealed: the sealed text opens, and a text that is not a
 * sealed one is refused after the call.
 */
void open_sealed() {
    if (!BOOST_TEST(!the_state().sealed->ec)) {
        verdict();
        return;
    }
    call([] {
        auto& s = the_state();
        s.sealer->open(s.sealed->text, record(s.opened, opened));
        s.sealer->open("not a sealed text", record(s.garbage, opened));
    });
}

}  // namespace

int main() {
    auto& s = the_state();
    s.sealer = make_sealer();
    call([] { the_state().sealer->seal("hello", record(the_state().sealed, open_sealed)); });
    const int id = s.loop.add([](int /*slot*/, const val& /*event*/) {
        BOOST_ERROR("the steps stopped before the verdict");
        set_exit_code(boost::report_errors());
    });
    s.deadline = val::global("setTimeout")(s.loop.handler(id, 0), val(10'000), val::undefined());
    set_exit_code(1);  // until the verdict
    return 0;
}
