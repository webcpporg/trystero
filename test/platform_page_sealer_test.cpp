// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests a page's randomness and sealer against a page whose crypto misbehaves, under node: no
// JavaScript exception enters WebAssembly, and each failure is the error the backend interface
// has.
//
// - When the page's `crypto.getRandomValues` throws, or is missing, the platform's source of
//   randomness reports that it filled nothing, and a seal completes with `error::backend_failure`,
//   after its call; once the function is back, both work again. Before the fix the C library's
//   entropy call, Emscripten's, let the function's exception through into WebAssembly, and the
//   program ended with it.
// - When `crypto.subtle.encrypt` or `decrypt` settles with something other than an ArrayBuffer, the
//   seal or the open completes with `error::backend_failure`. Before the fix the sealer read the
//   settlement as an ArrayBuffer whatever it was: a seal's -1 threw a RangeError into WebAssembly,
//   and an open's undefined opened as an empty text.
//
// main runs the io_context; each step starts from the completion of the one before, and the last
// stops the run. A step that does not come within 10 s fails by name.

#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/platform/entropy.hpp>
#include <webcpp/trystero/platform/sealer.hpp>
#include <webcpp/trystero/protocol/signal.hpp>

#include <webcpp/asio/io_context.hpp>

#include <emscripten/em_js.h>
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
using namespace std::chrono_literals;

// What the page's crypto does: 0, as the page has it; 1, getRandomValues throws; 2, it is missing;
// 3, subtle.encrypt settles with -1; 4, subtle.decrypt settles with undefined. node's own crypto
// object will not take another getRandomValues, so the page's is replaced by one that calls node's,
// but for what it changes, and put back for 0.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, misbehave, (int what), {
    if (!globalThis.pageCrypto) globalThis.pageCrypto = globalThis.crypto;
    const page = globalThis.pageCrypto;
    let crypto = page;
    if (what !== 0) {
        crypto = {
            getRandomValues: view => page.getRandomValues(view),
            subtle: {
                importKey: (...values) => page.subtle.importKey(...values),
                encrypt: (...values) => page.subtle.encrypt(...values),
                decrypt: (...values) => page.subtle.decrypt(...values),
            },
        };
    }
    if (what === 1) {
        crypto.getRandomValues = () => { throw new Error("no entropy"); };
    } else if (what === 2) {
        crypto.getRandomValues = undefined;
    } else if (what === 3) {
        crypto.subtle.encrypt = async () => -1;
    } else if (what === 4) {
        crypto.subtle.decrypt = async () => undefined;
    }
    Object.defineProperty(globalThis, "crypto", {value: crypto, configurable: true, writable: true});
});
// clang-format on

namespace {

/** What the page's crypto does, as misbehave() takes it. */
enum class page_crypto : std::uint8_t {
    working = 0,
    throwing_random = 1,
    missing_random = 2,
    encrypt_not_a_buffer = 3,
    decrypt_not_a_buffer = 4,
};

void set(page_crypto what) {
    misbehave(static_cast<int>(what));
}

/** What a completion saw: its error, its text, and whether it ran inside the call. */
struct outcome {
    boost::system::error_code ec;
    std::string text;
    bool inline_call = false;
};

struct state {
    asio::io_context io;
    std::shared_ptr<trystero::platform::sealer> sealer =
        std::make_shared<trystero::platform::sealer>(
            io.get_executor(), trystero::protocol::seal_key("secret", "my-app", "room-1"),
            std::pmr::get_default_resource());
    bool in_call = false;
    bool judged = false;
    std::optional<outcome> imported;
    std::optional<bool> filled_while_throwing;
    std::optional<outcome> sealed_while_throwing;
    std::optional<bool> filled_while_missing;
    std::optional<outcome> sealed_while_missing;
    std::optional<bool> filled_once_back;
    std::optional<outcome> sealed_once_back;
    std::optional<outcome> sealed_not_a_buffer;
    std::optional<outcome> opened_not_a_buffer;
};

/** The plaintext sealed: a candidate's JSON, as a room seals it. */
constexpr std::string_view plaintext =
    R"({"candidate":"candidate:1 1 udp 2122260223 192.0.2.1 54321 typ host"})";

/** Fills a buffer from the platform's source of randomness, as the backend's `random()` does. */
bool fill() {
    std::array<std::uint8_t, 32> bytes{};
    return trystero::platform::detail::fill_random(bytes);
}

/** A completion that records what it saw into `into`, then runs `next`. */
trystero::platform::sealer::done_handler record(state& s, std::optional<outcome>& into,
                                                void (*next)(state&)) {
    return [&s, &into, next](boost::system::error_code ec, std::string_view text) {
        into = outcome{.ec = ec, .text = std::string(text), .inline_call = s.in_call};
        next(s);
    };
}

/** Seals with `in_call` raised, so that a completion run inside the call says so. */
void seal(state& s, std::optional<outcome>& into, void (*next)(state&)) {
    s.in_call = true;
    s.sealer->seal(plaintext, record(s, into, next));
    s.in_call = false;
}

/** Checks that a completion came, after its call, with `expected`. */
void expect(const std::optional<outcome>& got, boost::system::error_code expected,
            const char* name) {
    if (!got) {
        BOOST_ERROR(name);
        return;
    }
    BOOST_TEST(got->ec == expected);
    BOOST_TEST(!got->inline_call);
}

void verdict(state& s) {
    set(page_crypto::working);
    expect(s.imported, {}, "the first seal did not complete");
    BOOST_TEST(s.filled_while_throwing == std::optional<bool>(false));
    expect(s.sealed_while_throwing, trystero::error::backend_failure,
           "the seal without randomness, getRandomValues throwing, did not complete");
    BOOST_TEST(s.filled_while_missing == std::optional<bool>(false));
    expect(s.sealed_while_missing, trystero::error::backend_failure,
           "the seal without randomness, getRandomValues missing, did not complete");
    BOOST_TEST(s.filled_once_back == std::optional<bool>(true));
    expect(s.sealed_once_back, {}, "the seal with randomness back did not complete");
    expect(s.sealed_not_a_buffer, trystero::error::backend_failure,
           "the seal whose encrypt settled with no ArrayBuffer did not complete");
    expect(s.opened_not_a_buffer, trystero::error::backend_failure,
           "the open whose decrypt settled with no ArrayBuffer did not complete");
    s.judged = true;
    s.io.stop();
}

/** decrypt settles with undefined: the text sealed once randomness was back does not open. */
void open_not_a_buffer(state& s) {
    set(page_crypto::decrypt_not_a_buffer);
    if (!s.sealed_once_back || s.sealed_once_back->ec) {
        verdict(s);
        return;
    }
    s.in_call = true;
    s.sealer->open(s.sealed_once_back->text, record(s, s.opened_not_a_buffer, verdict));
    s.in_call = false;
}

/** encrypt settles with -1: the seal fails. */
void seal_not_a_buffer(state& s) {
    set(page_crypto::encrypt_not_a_buffer);
    seal(s, s.sealed_not_a_buffer, open_not_a_buffer);
}

/** getRandomValues is back: the source fills again, and a seal works. */
void seal_once_back(state& s) {
    set(page_crypto::working);
    s.filled_once_back = fill();
    seal(s, s.sealed_once_back, seal_not_a_buffer);
}

/** getRandomValues is missing: the source fills nothing, and the seal fails. */
void seal_while_missing(state& s) {
    set(page_crypto::missing_random);
    s.filled_while_missing = fill();
    seal(s, s.sealed_while_missing, seal_once_back);
}

/** getRandomValues throws: the source fills nothing, and the seal fails. */
void seal_while_throwing(state& s) {
    set(page_crypto::throwing_random);
    s.filled_while_throwing = fill();
    seal(s, s.sealed_while_throwing, seal_while_missing);
}

}  // namespace

int main() {
    state s;
    // The first seal imports the key, with the page's crypto working.
    seal(s, s.imported, seal_while_throwing);
    s.io.run_for(10s);  // lint-run: the test's event loop, held by the sealer's Web Crypto waits
    if (!s.judged) {
        set(page_crypto::working);
        BOOST_ERROR("the steps stopped before the verdict");
    }
    return webcpp::test::exit_status(boost::report_errors());
}
