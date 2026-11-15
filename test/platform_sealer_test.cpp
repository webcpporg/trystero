// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the platform backend's sealer on both targets, natively OpenSSL's AES-GCM and on
// emscripten the page's Web Crypto under node, against text Trystero JS sealed with WebCrypto,
// which the original's oracle wrote into fixtures/expected/expected.json, the test's one argument:
// it opens what Trystero JS sealed, a text it seals opens again in Trystero's form, a text sealed
// under another key or that is no sealed text is a wrong password, and every completion runs after
// the call that started it, never inside it, as the backend interface says. On emscripten, without
// crypto.subtle, as on a page that is not a secure context, the key cannot be imported: the first
// operation and one after the failure complete after their calls with a backend failure. Before
// the fix, three of the page's completions ran inside the call.
//
// main starts the first step and runs the io_context, which each completion the sealer posts, or
// each Web Crypto promise it awaits, holds; each step starts from the completion of the one before,
// and the last ends the run. A step that does not come within 10 s fails by name.

#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/protocol/signal.hpp>

#include <webcpp/asio/io_context.hpp>

#ifdef WEBCPP_TARGET_EMSCRIPTEN
#include <emscripten/val.h>
#endif
#include <webcpp/document/document.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/value.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <webcpp/test/exit_status.hpp>

#include <chrono>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace asio = webcpp::asio;
namespace tp = webcpp::trystero::protocol;
namespace trystero = webcpp::trystero;
using namespace std::chrono_literals;

namespace {

/** The fixture at `path`, read with webcpp::document; nothing when it does not parse. */
std::optional<webcpp::document::document> load(const char* path) {
    const std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    auto parsed = webcpp::document::parse(text.str());
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return std::move(parsed).unsafe_value();
}

/** The resource of every sealer and text of the test. */
std::pmr::memory_resource* memory() {
    return std::pmr::get_default_resource();
}

/** What a completion saw: its error, its text, and whether it ran inside the call. */
struct outcome {
    boost::system::error_code ec;
    std::string text;
    bool inline_call = false;
};

struct state {
    explicit state(webcpp::document::document fixture) : fixture(std::move(fixture)) {}

    /** The fixture's string `key` of what Trystero JS sealed; empty when it holds none. */
    [[nodiscard]] std::string_view sealed_by_js(std::string_view key) const {
        return fixture["sealed"][key].value_or(std::string_view());
    }

    webcpp::document::document fixture;
    asio::io_context io;
    trystero::platform::backend backend{io.get_executor()};
    std::shared_ptr<trystero::backend::sealer> sealer =
        backend.make_sealer(tp::seal_key("secret", "my-app", "room-1"), memory());
    std::shared_ptr<trystero::backend::sealer> intruder =
        backend.make_sealer(tp::seal_key("guess", "my-app", "room-1"), memory());
    std::shared_ptr<trystero::backend::sealer> fresh =
        backend.make_sealer(tp::seal_key("", "app", "room"), memory());
    bool in_call = false;
    bool judged = false;
    std::optional<outcome> opened_js;
    std::optional<outcome> sealed;
    std::optional<outcome> opened;
    std::optional<outcome> wrong_key;
    std::optional<outcome> garbage;
    std::optional<outcome> without_subtle;
    std::optional<outcome> after_failure;
};

/** The plaintext sealed and opened again: a candidate's JSON, as a room seals it. */
constexpr std::string_view plaintext =
    R"({"candidate":"candidate:1 1 udp 2122260223 192.0.2.1 54321 typ host"})";

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

/** Checks that a completion came, after its call, with `expected`. */
void expect(const std::optional<outcome>& got, boost::system::error_code expected) {
    if (BOOST_TEST(got.has_value())) {
        BOOST_TEST(got->ec == expected);
        BOOST_TEST(!got->inline_call);
    }
}

void verdict(state& s) {
    expect(s.opened_js, {});
    if (s.opened_js) {
        BOOST_TEST_EQ(s.opened_js->text, s.sealed_by_js("plaintext"));
    }
    expect(s.sealed, {});
    if (s.sealed) {
        const auto parts = tp::parse_sealed(s.sealed->text);
        if (BOOST_TEST(parts.has_value())) {
            BOOST_TEST_EQ(parts->iv.size(), tp::seal_iv_size);
            // The GCM tag follows, as WebCrypto writes it.
            BOOST_TEST_EQ(parts->ciphertext.size(), plaintext.size() + 16);
        }
    }
    expect(s.opened, {});
    if (s.opened) {
        BOOST_TEST_EQ(s.opened->text, plaintext);
    }
    expect(s.wrong_key, trystero::error::wrong_password);
    expect(s.garbage, trystero::error::wrong_password);
#ifdef WEBCPP_TARGET_EMSCRIPTEN
    expect(s.without_subtle, trystero::error::backend_failure);
    expect(s.after_failure, trystero::error::backend_failure);
#endif
    s.judged = true;
    s.io.stop();
}

#ifdef WEBCPP_TARGET_EMSCRIPTEN
/** Once the key could not be imported: an operation after the failure completes after its call. */
void open_after_failure(state& s) {
    call(s, [&s] { s.fresh->open("x$AAAA", record(s, s.after_failure, verdict)); });
}

/**
 * Without crypto.subtle, as outside a secure context: the key cannot be imported, and the first
 * operation completes after its call.
 */
void seal_without_subtle(state& s) {
    emscripten::val descriptor = emscripten::val::object();
    descriptor.set("value", emscripten::val::object());
    emscripten::val::global("Object").call<void>("defineProperty", emscripten::val::global(),
                                                 emscripten::val("crypto"), descriptor);
    s.fresh = s.backend.make_sealer(tp::seal_key("", "app", "room"), memory());
    call(s, [&s] { s.fresh->seal("hello", record(s, s.without_subtle, open_after_failure)); });
}
#endif

/** Once the wrong key and the garbage are refused: on emscripten, a page without crypto.subtle. */
void refused(state& s) {
    if (!s.wrong_key || !s.garbage) {
        return;
    }
#ifdef WEBCPP_TARGET_EMSCRIPTEN
    seal_without_subtle(s);
#else
    verdict(s);
#endif
}

/**
 * Another key's sealer opens what Trystero JS sealed as a wrong password, and the room's opens a
 * text that is no sealed one so too.
 */
void refuse(state& s) {
    call(s, [&s] {
        s.intruder->open(s.sealed_by_js("text"), record(s, s.wrong_key, refused));
        s.sealer->open("not a sealed text", record(s, s.garbage, refused));
    });
}

/** Once its text is sealed, the sealer opens it again. */
void open_sealed(state& s) {
    if (!s.sealed || s.sealed->ec) {
        verdict(s);
        return;
    }
    call(s, [&s] { s.fresh->open(s.sealed->text, record(s, s.opened, refuse)); });
}

/** Once Trystero JS's text is opened, a sealer under another key seals one of its own. */
void seal(state& s) {
    call(s, [&s] { s.fresh->seal(plaintext, record(s, s.sealed, open_sealed)); });
}

/** Opens what Trystero JS sealed under the room's key. */
void open_what_js_sealed(state& s) {
    call(s, [&s] { s.sealer->open(s.sealed_by_js("text"), record(s, s.opened_js, seal)); });
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!BOOST_TEST_EQ(argc, 2)) {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
            << "usage: platform_sealer_test <fixtures/expected/expected.json>\n";
        return webcpp::test::exit_status(boost::report_errors());
    }
    auto fixture = load(argv[1]);
    if (!BOOST_TEST(fixture.has_value() && (*fixture)["sealed"].is_object())) {
        return webcpp::test::exit_status(boost::report_errors());
    }
    state s(std::move(*fixture));
    open_what_js_sealed(s);
    s.io.run_for(10s);  // lint-run: the test's event loop, held by the sealers' completions
    if (!s.judged) {
        BOOST_ERROR("the steps stopped before the verdict");
    }
    return webcpp::test::exit_status(boost::report_errors());
}
