// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the native WebSocket client, Boost.Beast's, against an echo server in the same process,
// on 127.0.0.1.

#include <webcpp/trystero/native/websocket.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/core/lightweight_test.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace trystero = webcpp::trystero;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

namespace {

/**
 * Accepts WebSocket connections and sends every text message back.
 *
 * Tip: each coroutine ends on its first error, read from a tuple, so the server needs no
 * exceptions, as the client it tests does not.
 */
struct echo_server {
    explicit echo_server(asio::io_context& io)
        : acceptor(io, {asio::ip::make_address("127.0.0.1"), 0}) {
        asio::co_spawn(io, accept(), asio::detached);
    }

    std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/relay";
    }

    asio::awaitable<void> accept() {
        for (;;) {
            auto [ec, socket] = co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return;
            }
            asio::co_spawn(acceptor.get_executor(), echo(std::move(socket)), asio::detached);
        }
    }

    static asio::awaitable<void> echo(tcp::socket socket) {
        beast::websocket::stream<tcp::socket> ws(std::move(socket));
        auto [accepted] = co_await ws.async_accept(asio::as_tuple(asio::use_awaitable));
        if (accepted) {
            co_return;
        }
        beast::flat_buffer buffer;
        for (;;) {
            auto [read, read_size] =
                co_await ws.async_read(buffer, asio::as_tuple(asio::use_awaitable));
            if (read) {
                co_return;
            }
            ws.text(true);
            auto [written, written_size] =
                co_await ws.async_write(buffer.data(), asio::as_tuple(asio::use_awaitable));
            if (written) {
                co_return;
            }
            buffer.consume(buffer.size());
        }
    }

    tcp::acceptor acceptor;
};

bool run_until(asio::io_context& io, const std::function<bool()>& done,
               std::chrono::milliseconds limit = 5s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < end) {
        io.run_for(10ms);  // lint-run: the client and the echo server do loopback network I/O
        if (io.stopped()) {
            io.restart();
        }
    }
    return done();
}

void urls_name_a_scheme_host_port_and_path() {
    const auto secure = trystero::native::detail::parse_url("wss://relay.example.org");
    if (!BOOST_TEST(secure.has_value())) {
        return;
    }
    BOOST_TEST(secure->secure);
    BOOST_TEST_EQ(secure->host, "relay.example.org");
    BOOST_TEST_EQ(secure->port, "443");
    BOOST_TEST_EQ(secure->target, "/");
    const auto plain = trystero::native::detail::parse_url("ws://127.0.0.1:7447/nostr?x=1");
    if (!BOOST_TEST(plain.has_value())) {
        return;
    }
    BOOST_TEST(!plain->secure);
    BOOST_TEST_EQ(plain->host, "127.0.0.1");
    BOOST_TEST_EQ(plain->port, "7447");
    BOOST_TEST_EQ(plain->target, "/nostr?x=1");
    BOOST_TEST(!trystero::native::detail::parse_url("https://example.org").has_value());
    BOOST_TEST(!trystero::native::detail::parse_url("wss://").has_value());
}

void opens_sends_in_order_receives_and_closes_once() {
    asio::io_context io;
    echo_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    std::vector<std::string> received;
    auto socket = trystero::native::connect_websocket(
        io.get_executor(), tls, server.url(),
        {
            .open = [&] { opened = true; },
            .message = [&](std::string_view text) { received.emplace_back(text); },
            .close = [&] { ++closes; },
        });
    if (!BOOST_TEST(run_until(io, [&] { return opened; }))) {
        return;
    }
    for (const char* text : {"one", "two", R"(["EVENT",{"content":"three"}])"}) {
        socket->send(text);
    }
    if (!BOOST_TEST(run_until(io, [&] { return received.size() == 3; }))) {
        return;
    }
    const std::vector<std::string> sent{"one", "two", R"(["EVENT",{"content":"three"}])"};
    BOOST_TEST_ALL_EQ(received.begin(), received.end(), sent.begin(), sent.end());
    socket->close();
    BOOST_TEST(run_until(io, [&] { return closes == 1; }));
    socket->close();
    run_until(io, [] { return false; }, 100ms);
    BOOST_TEST_EQ(closes, 1);
}

void an_unreachable_relay_closes_without_opening() {
    asio::io_context io;
    tcp::acceptor taken(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto port = taken.local_endpoint().port();
    taken.close();  // nothing listens there now
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    auto socket = trystero::native::connect_websocket(io.get_executor(), tls,
                                                      "ws://127.0.0.1:" + std::to_string(port),
                                                      {
                                                          .open = [&] { opened = true; },
                                                          .message = {},
                                                          .close = [&] { ++closes; },
                                                      });
    BOOST_TEST(run_until(io, [&] { return closes == 1; }));
    BOOST_TEST(!opened);
}

void a_dropped_socket_reports_nothing() {
    asio::io_context io;
    echo_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    int events = 0;
    {
        auto socket =
            trystero::native::connect_websocket(io.get_executor(), tls, server.url(),
                                                {
                                                    .open = [&] { ++events; },
                                                    .message = [&](std::string_view) { ++events; },
                                                    .close = [&] { ++events; },
                                                });
    }
    run_until(io, [] { return false; }, 200ms);
    BOOST_TEST_EQ(events, 0);
}

void closing_right_after_a_send_lets_the_write_finish_first() {
    asio::io_context io;
    echo_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    auto socket = trystero::native::connect_websocket(io.get_executor(), tls, server.url(),
                                                      {
                                                          .open = [&] { opened = true; },
                                                          .message = {},
                                                          .close = [&] { ++closes; },
                                                      });
    if (!BOOST_TEST(run_until(io, [&] { return opened; }))) {
        return;
    }
    // What the Nostr strategy sends before it closes a relay.
    socket->send(R"(["CLOSE","subscription"])");
    socket->close();
    BOOST_TEST(run_until(io, [&] { return closes == 1; }));
}

}  // namespace

int main() {
    urls_name_a_scheme_host_port_and_path();
    opens_sends_in_order_receives_and_closes_once();
    an_unreachable_relay_closes_without_opening();
    a_dropped_socket_reports_nothing();
    closing_right_after_a_send_lets_the_write_finish_first();
    return boost::report_errors();
}
