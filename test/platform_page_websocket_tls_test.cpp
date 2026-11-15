// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the TLS contexts with which the platform backend reaches a `wss://` relay in a page, under
// node, over a stand-in for the page's WebSocket, which the browser verifies against its own
// authorities:
//
// - A program's context made as Asio makes a client's, `ssl::context(ssl::context::tls_client)`,
//   whose verify mode is verify_none, reaches the relay, as the backend's own context does, and as
//   it does natively, where the relay is verified whatever the context's mode. Before the fix the
//   session set verify_peer on its stream natively only, and webcpp::beast's page stream, which
//   refuses a mode other than verify_peer, refused the program's context: every `wss://` relay
//   reported a close.
// - A context the page cannot honour reports a close without making a WebSocket, rather than reach
//   the relay with less than it asks: one with an authority added, with a client certificate, with
//   an option, of a method that is not a client's of every version, or with a verify callback of
//   its own.
//
// main runs the io_context under a work guard, which the verdict releases once every socket has
// reported its close; a step that does not come within 10 s fails by name.

#include <webcpp/trystero/platform/backend.hpp>

#include <webcpp/asio/buffer.hpp>
#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/ssl.hpp>

#include <emscripten/em_js.h>
#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <webcpp/test/exit_status.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
namespace ssl = webcpp::asio::ssl;
using emscripten::val;
using namespace std::chrono_literals;

// The stand-in: it records the URL of each construction on globalThis, opens on the next turn,
// and closes on the next turn after close(), once.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, install_recording_websocket, (), {
    globalThis.recordedUrls = [];
    globalThis.WebSocket = class {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;
        constructor(url) {
            globalThis.recordedUrls.push(url);
            this.url = url;
            this.readyState = 0;
            this.bufferedAmount = 0;
            setTimeout(() => {
                this.readyState = 1;
                if (this.onopen) this.onopen({type: "open"});
            }, 0);
        }
        send(text) {}
        close() {
            if (this.readyState >= 2) return;
            this.readyState = 2;
            setTimeout(() => {
                this.readyState = 3;
                if (this.onclose) this.onclose({type: "close", code: 1000, reason: ""});
            }, 0);
        }
    };
});
// clang-format on

namespace {

/** One backend with a context of its own, its socket to the relay, and what the socket reported. */
struct relay {
    relay(asio::io_context& io, ssl::context context)
        : backend(io.get_executor(), std::move(context)) {}

    trystero::platform::backend backend;
    std::shared_ptr<trystero::backend::websocket> socket;
    std::size_t opens = 0;
    std::size_t closes = 0;
};

/** The contexts the page cannot honour, by what each asks. */
enum class refused : std::uint8_t {
    authority,
    certificate,
    option,
    method,
    callback,
};

constexpr std::array<refused, 5> refusals{
    refused::authority, refused::certificate, refused::option, refused::method, refused::callback,
};

/** A context of `tls_client` that asks for `what`, which a page's stream refuses. */
ssl::context refused_context(refused what) {
    ssl::context context(what == refused::method ? ssl::context::tlsv12_client
                                                 : ssl::context::tls_client);
    boost::system::error_code ec;
    constexpr std::string_view pem = "-----BEGIN CERTIFICATE-----";
    switch (what) {
        case refused::authority: context.add_certificate_authority(asio::buffer(pem), ec); break;
        case refused::certificate: context.use_certificate_chain(asio::buffer(pem), ec); break;
        case refused::option: context.set_options(ssl::context::no_compression, ec); break;
        case refused::method: break;
        case refused::callback:
            context.set_verify_callback(
                [](bool preverified, auto& /*context*/) { return preverified; }, ec);
            break;
    }
    BOOST_TEST(!ec);
    return context;
}

struct state {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    // A program's context as Asio makes a client's, and the backend's own.
    relay asio_client{io, ssl::context(ssl::context::tls_client)};
    std::optional<trystero::platform::backend> own{std::in_place, io.get_executor()};
    std::shared_ptr<trystero::backend::websocket> own_socket;
    std::size_t own_opens = 0;
    std::size_t own_closes = 0;
    std::array<std::optional<relay>, refusals.size()> refusing;
    bool judged = false;
};

void verdict(state& s) {
    if (s.asio_client.closes == 0 || s.own_closes == 0) {
        return;
    }
    for (const auto& r : s.refusing) {
        if (r->closes == 0) {
            return;
        }
    }
    BOOST_TEST_EQ(s.asio_client.opens, 1U);
    BOOST_TEST_EQ(s.asio_client.closes, 1U);
    BOOST_TEST_EQ(s.own_opens, 1U);
    BOOST_TEST_EQ(s.own_closes, 1U);
    for (std::size_t i = 0; i < refusals.size(); ++i) {
        BOOST_TEST_EQ(s.refusing.at(i)->opens, 0U);
        BOOST_TEST_EQ(s.refusing.at(i)->closes, 1U);
    }
    // The two that opened, over TLS, and no other: a refused context makes no WebSocket.
    // webcpp::beast writes the port into the URL it gives the page.
    const val urls = val::global("recordedUrls");
    if (BOOST_TEST_EQ(urls["length"].as<std::uint32_t>(), 2U)) {
        BOOST_TEST(urls[0].strictlyEquals(val("wss://relay.test:443/")));
        BOOST_TEST(urls[1].strictlyEquals(val("wss://relay.test:443/")));
    }
    s.asio_client.socket.reset();
    s.own_socket.reset();
    for (auto& r : s.refusing) {
        r->socket.reset();
    }
    s.judged = true;
    s.guard.reset();
}

/** Handlers that count into `r`, and close the socket once it opens. */
trystero::backend::websocket::handlers counting(state& s, relay& r) {
    return {
        .open =
            [&r] {
                ++r.opens;
                r.socket->close();
            },
        .message = {},
        .close =
            [&s, &r] {
                ++r.closes;
                verdict(s);
            },
    };
}

}  // namespace

int main() {
    install_recording_websocket();
    state s;
    s.asio_client.socket = s.asio_client.backend.connect(
        "wss://relay.test", counting(s, s.asio_client), std::pmr::get_default_resource());
    s.own_socket = s.own->connect("wss://relay.test",
                                  {
                                      .open =
                                          [&s] {
                                              ++s.own_opens;
                                              s.own_socket->close();
                                          },
                                      .message = {},
                                      .close =
                                          [&s] {
                                              ++s.own_closes;
                                              verdict(s);
                                          },
                                  },
                                  std::pmr::get_default_resource());
    for (std::size_t i = 0; i < refusals.size(); ++i) {
        auto& r = s.refusing.at(i).emplace(s.io, refused_context(refusals.at(i)));
        r.socket =
            r.backend.connect("wss://relay.test", counting(s, r), std::pmr::get_default_resource());
    }
    s.io.run_for(10s);  // lint-run: the test's event loop, held by its work guard until the verdict
    if (!s.judged) {
        BOOST_ERROR("a socket did not report its close");
        BOOST_TEST_EQ(s.asio_client.opens, 1U);
    }
    return webcpp::test::exit_status(boost::report_errors());
}
