// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the native WebSocket client, Boost.Beast's, against an echo server in the same process,
// on 127.0.0.1.

#include <webcpp/trystero/native/websocket.hpp>
#include <webcpp/trystero/protocol/relays.hpp>

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
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace trystero = webcpp::trystero;
using asio::ip::tcp;
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

    [[nodiscard]] std::string url() const {
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

/** What a hostile relay does once it reads a client's first message. */
enum class misbehaviour {
    /** Sends a text one byte longer than a room reads, then goes on reading. */
    too_long_text,
    /** Drops the TCP connection with a reset. */
    reset,
};

/**
 * Accepts WebSocket connections, reads every message, and misbehaves at the first: what any relay
 * of a room's list can do, and what a network drop does by chance.
 */
struct hostile_server {
    hostile_server(asio::io_context& io, misbehaviour m)
        : acceptor(io, {asio::ip::make_address("127.0.0.1"), 0}), what(m) {
        asio::co_spawn(io, accept(), asio::detached);
    }

    [[nodiscard]] std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/relay";
    }

    asio::awaitable<void> accept() {
        for (;;) {
            auto [ec, socket] = co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return;
            }
            asio::co_spawn(acceptor.get_executor(), serve(std::move(socket), what), asio::detached);
        }
    }

    static asio::awaitable<void> serve(tcp::socket socket, misbehaviour what) {
        const auto ws = std::make_shared<beast::websocket::stream<tcp::socket>>(std::move(socket));
        auto [accepted] = co_await ws->async_accept(asio::as_tuple(asio::use_awaitable));
        if (accepted) {
            co_return;
        }
        beast::flat_buffer buffer;
        for (bool first = true;; first = false) {
            auto [read, read_size] =
                co_await ws->async_read(buffer, asio::as_tuple(asio::use_awaitable));
            if (read) {
                co_return;
            }
            buffer.consume(buffer.size());
            if (!first) {
                continue;
            }
            if (what == misbehaviour::reset) {
                // A linger of 0 makes the close a reset.
                boost::system::error_code ec;
                const bool reset =
                    !ws->next_layer().set_option(asio::socket_base::linger(true, 0), ec) &&
                    !ws->next_layer().close(ec);
                BOOST_TEST(reset);
                co_return;
            }
            // Written beside the reads, so that the client's writes keep draining meanwhile.
            asio::co_spawn(
                ws->get_executor(),
                [ws]() -> asio::awaitable<void> {
                    const std::string text(trystero::protocol::max_relay_message_bytes + 1, 'x');
                    ws->text(true);
                    co_await ws->async_write(asio::buffer(text),
                                             asio::as_tuple(asio::use_awaitable));
                },
                asio::detached);
        }
    }

    tcp::acceptor acceptor;
    misbehaviour what;
};

/**
 * Accepts WebSocket connections, answers the handshake, and never reads again, keeping each
 * connection open: a relay that stalls, or a path that died without a reset.
 */
struct stalled_server {
    explicit stalled_server(asio::io_context& io)
        : acceptor(io, {asio::ip::make_address("127.0.0.1"), 0}) {
        asio::co_spawn(io, accept(), asio::detached);
    }

    [[nodiscard]] std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/relay";
    }

    asio::awaitable<void> accept() {
        for (;;) {
            auto [ec, socket] = co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return;
            }
            auto ws = std::make_shared<beast::websocket::stream<tcp::socket>>(std::move(socket));
            auto [accepted] = co_await ws->async_accept(asio::as_tuple(asio::use_awaitable));
            if (!accepted) {
                held.push_back(std::move(ws));
            }
        }
    }

    tcp::acceptor acceptor;
    std::vector<std::shared_ptr<beast::websocket::stream<tcp::socket>>> held;
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
    const echo_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    std::vector<std::string> received;
    const auto socket = trystero::native::connect_websocket(
        io.get_executor(), tls, server.url(),
        {
            .open = [&] { opened = true; },
            .message = [&](std::string_view text) { received.emplace_back(text); },
            .close = [&] { ++closes; },
        },
        std::pmr::get_default_resource());
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

// A message longer than any relay's (protocol::max_relay_message_bytes) is never handed over: the
// socket closes, as the browser's does, and the room's strategy reconnects.
void a_message_longer_than_a_relay_sends_closes_the_socket() {
    asio::io_context io;
    const echo_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    std::vector<std::size_t> received;
    const auto socket = trystero::native::connect_websocket(
        io.get_executor(), tls, server.url(),
        {
            .open = [&] { opened = true; },
            .message = [&](std::string_view text) { received.push_back(text.size()); },
            .close = [&] { ++closes; },
        },
        std::pmr::get_default_resource());
    if (!BOOST_TEST(run_until(io, [&] { return opened; }))) {
        return;
    }
    socket->send(std::string(trystero::protocol::max_relay_message_bytes, 'x'));
    if (!BOOST_TEST(run_until(io, [&] { return received.size() == 1; }))) {
        return;
    }
    socket->send(std::string(trystero::protocol::max_relay_message_bytes + 1, 'x'));
    BOOST_TEST(run_until(io, [&] { return closes == 1; }));
    BOOST_TEST_EQ(received.size(), 1U);
    BOOST_TEST_EQ(received.at(0), trystero::protocol::max_relay_message_bytes);
}

void an_unreachable_relay_closes_without_opening() {
    asio::io_context io;
    tcp::acceptor taken(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto port = taken.local_endpoint().port();
    taken.close();  // nothing listens there now
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    const auto socket = trystero::native::connect_websocket(
        io.get_executor(), tls, "ws://127.0.0.1:" + std::to_string(port),
        {
            .open = [&] { opened = true; },
            .message = {},
            .close = [&] { ++closes; },
        },
        std::pmr::get_default_resource());
    BOOST_TEST(run_until(io, [&] { return closes == 1; }));
    BOOST_TEST(!opened);
}

void a_dropped_socket_reports_nothing() {
    asio::io_context io;
    const echo_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    int events = 0;
    {
        const auto socket =
            trystero::native::connect_websocket(io.get_executor(), tls, server.url(),
                                                {
                                                    .open = [&] { ++events; },
                                                    .message = [&](std::string_view) { ++events; },
                                                    .close = [&] { ++events; },
                                                },
                                                std::pmr::get_default_resource());
    }
    run_until(io, [] { return false; }, 200ms);
    BOOST_TEST_EQ(events, 0);
}

void closing_right_after_a_send_lets_the_write_finish_first() {
    asio::io_context io;
    const echo_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    const auto socket = trystero::native::connect_websocket(io.get_executor(), tls, server.url(),
                                                            {
                                                                .open = [&] { opened = true; },
                                                                .message = {},
                                                                .close = [&] { ++closes; },
                                                            },
                                                            std::pmr::get_default_resource());
    if (!BOOST_TEST(run_until(io, [&] { return opened; }))) {
        return;
    }
    // What the Nostr strategy sends before it closes a relay.
    socket->send(R"(["CLOSE","subscription"])");
    socket->close();
    BOOST_TEST(run_until(io, [&] { return closes == 1; }));
}

// A connection that ends while a write is in flight: the relay sends a text too long to read, or
// resets the connection, while the client has many texts queued. The socket reports one close, and
// the write that was in flight finishes after it on a text it still owns. Each misbehaviour is met
// several times, since which of the read and the write completes first is the network's choice.
// The test is built with the standard library's own checks (test/Jamfile), so that a write
// completion that touches a cleared queue fails at once.
void a_connection_that_ends_during_a_write_closes_once(misbehaviour what) {
    constexpr int connections = 10;
    constexpr int texts = 200;
    asio::io_context io;
    const hostile_server server(io, what);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    const std::string text(std::size_t{64} * 1024, 't');
    for (int i = 0; i < connections; ++i) {
        bool opened = false;
        int closes = 0;
        std::shared_ptr<trystero::backend::websocket> socket;
        socket = trystero::native::connect_websocket(io.get_executor(), tls, server.url(),
                                                     {
                                                         .open =
                                                             [&] {
                                                                 opened = true;
                                                                 for (int t = 0; t < texts; ++t) {
                                                                     socket->send(text);
                                                                 }
                                                             },
                                                         .message = {},
                                                         .close = [&] { ++closes; },
                                                     },
                                                     std::pmr::get_default_resource());
        if (!BOOST_TEST(run_until(io, [&] { return closes == 1; }))) {
            return;
        }
        BOOST_TEST(opened);
        socket.reset();  // as the room's strategy replaces a relay's socket when it reconnects
        run_until(io, [] { return false; }, 50ms);
        BOOST_TEST_EQ(closes, 1);
    }
}

// A relay that stops reading: the texts a room sends it pile up in the socket's queue. At
// websocket_limits::max_queued_bytes the socket drops the connection, reports one close, and holds
// no more, so that the room reconnects with its backoff.
void a_relay_that_stops_reading_is_dropped_at_the_queue_bound() {
    asio::io_context io;
    const stalled_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    const auto socket = trystero::native::connect_websocket(io.get_executor(), tls, server.url(),
                                                            {
                                                                .open = [&] { opened = true; },
                                                                .message = {},
                                                                .close = [&] { ++closes; },
                                                            },
                                                            std::pmr::get_default_resource());
    if (!BOOST_TEST(run_until(io, [&] { return opened; }))) {
        return;
    }
    const std::string text(std::size_t{64} * 1024, 't');
    for (int i = 0; i < 2000 && closes == 0; ++i) {
        socket->send(text);
        io.poll();
    }
    BOOST_TEST(run_until(io, [&] { return closes == 1; }));
    run_until(io, [] { return false; }, 100ms);
    BOOST_TEST_EQ(closes, 1);
}

// A relay that stops answering while the room has nothing to send: a keep-alive ping goes
// unanswered, and the socket closes after websocket_limits::idle_timeout, here 1 s.
void a_relay_that_stops_answering_times_out() {
    asio::io_context io;
    const stalled_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    const auto socket = trystero::native::connect_websocket(
        io.get_executor(), tls, server.url(),
        {
            .open = [&] { opened = true; },
            .message = {},
            .close = [&] { ++closes; },
        },
        std::pmr::get_default_resource(),
        {
            .max_queued_bytes = trystero::native::websocket_limits{}.max_queued_bytes,
            .idle_timeout = 1s,
        });
    if (!BOOST_TEST(run_until(io, [&] { return opened; }))) {
        return;
    }
    BOOST_TEST(run_until(io, [&] { return closes == 1; }, 10s));
}

// A relay that is quiet but healthy: it sends nothing, but reads, so it answers each keep-alive
// ping. With a 1 s idle timeout, the socket stays open well past it, and then still echoes.
void a_quiet_relay_that_answers_pings_stays_connected() {
    asio::io_context io;
    const echo_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    std::vector<std::string> received;
    const auto socket = trystero::native::connect_websocket(
        io.get_executor(), tls, server.url(),
        {
            .open = [&] { opened = true; },
            .message = [&](std::string_view text) { received.emplace_back(text); },
            .close = [&] { ++closes; },
        },
        std::pmr::get_default_resource(),
        {
            .max_queued_bytes = trystero::native::websocket_limits{}.max_queued_bytes,
            .idle_timeout = 1s,
        });
    if (!BOOST_TEST(run_until(io, [&] { return opened; }))) {
        return;
    }
    run_until(io, [&] { return closes != 0; }, 3s);  // three idle timeouts
    BOOST_TEST_EQ(closes, 0);
    socket->send("still there");
    BOOST_TEST(run_until(io, [&] { return received.size() == 1; }));
}

}  // namespace

int main() {
    urls_name_a_scheme_host_port_and_path();
    opens_sends_in_order_receives_and_closes_once();
    a_message_longer_than_a_relay_sends_closes_the_socket();
    an_unreachable_relay_closes_without_opening();
    a_dropped_socket_reports_nothing();
    closing_right_after_a_send_lets_the_write_finish_first();
    a_connection_that_ends_during_a_write_closes_once(misbehaviour::too_long_text);
    a_connection_that_ends_during_a_write_closes_once(misbehaviour::reset);
    a_relay_that_stops_reading_is_dropped_at_the_queue_bound();
    a_relay_that_stops_answering_times_out();
    a_quiet_relay_that_answers_pings_stays_connected();
    return boost::report_errors();
}
