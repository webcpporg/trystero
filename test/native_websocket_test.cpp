// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the native WebSocket client, webcpp::beast's, natively Boost.Beast's, against servers in
// the same process, on 127.0.0.1: an echo server in the clear and one over TLS, with a test
// authority of its own, and relays that misbehave.

#include <webcpp/trystero/native/backend.hpp>
#include <webcpp/trystero/native/websocket.hpp>
#include <webcpp/trystero/protocol/relays.hpp>

#include <webcpp/asio/tls_client_context.hpp>

#include <openssl/ssl.h>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/core/lightweight_test.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "require.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace trystero = webcpp::trystero;
using asio::ip::tcp;
using namespace std::chrono_literals;

namespace {

// The test's own authority and a relay's certificate it signed, for the name localhost alone
// (no address), with the relay's key: EC P-256, valid from 2026 to 2126. Made with OpenSSL's
// command line once; nothing else trusts the authority.
constexpr std::string_view test_authority = R"(-----BEGIN CERTIFICATE-----
MIIBqjCCAVGgAwIBAgIUIlj5fjUA1vNsiodeLDxZZrfAED0wCgYIKoZIzj0EAwIw
IjEgMB4GA1UEAwwXd2ViY3BwIHRyeXN0ZXJvIHRlc3QgQ0EwIBcNMjYwMTAxMDAw
MDAwWhgPMjEyNjAxMDEwMDAwMDBaMCIxIDAeBgNVBAMMF3dlYmNwcCB0cnlzdGVy
byB0ZXN0IENBMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEzvwWZu/uKAs+QRvH
XRwLYFz8blZJ+lF0HLMZDQ9hUmiv8EDAcba20o5LMjN0P5nOdJo/036+KXrm9DXw
KbPvQaNjMGEwHQYDVR0OBBYEFBOTazg3MnmU2S1gvIPSiWLRGCyoMB8GA1UdIwQY
MBaAFBOTazg3MnmU2S1gvIPSiWLRGCyoMA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0P
AQH/BAQDAgEGMAoGCCqGSM49BAMCA0cAMEQCIE98bCTUol8DDmoGh81Lh5NirVxq
aMYwfaQM+b4lxJqzAiAsWzw2WW3UbPWI2arYUKE6IMfU+n0pDQd7wfsoDHsqaA==
-----END CERTIFICATE-----
)";

constexpr std::string_view relay_certificate = R"(-----BEGIN CERTIFICATE-----
MIIBxzCCAW2gAwIBAgIUQCMGnMaDIkHCtKl7oV2FPaItruYwCgYIKoZIzj0EAwIw
IjEgMB4GA1UEAwwXd2ViY3BwIHRyeXN0ZXJvIHRlc3QgQ0EwIBcNMjYwMTAxMDAw
MDAwWhgPMjEyNjAxMDEwMDAwMDBaMBQxEjAQBgNVBAMMCWxvY2FsaG9zdDBZMBMG
ByqGSM49AgEGCCqGSM49AwEHA0IABNxV13TdSt0NISLCl7RN2hmPTZVHQbL3ScpF
Upnb9xFsg7XND5KgmoxaqqcNYvre7LvYikPFCMGe9Sxm9RVU9m+jgYwwgYkwDAYD
VR0TAQH/BAIwADAOBgNVHQ8BAf8EBAMCB4AwEwYDVR0lBAwwCgYIKwYBBQUHAwEw
FAYDVR0RBA0wC4IJbG9jYWxob3N0MB0GA1UdDgQWBBTWHv+8dJ288V0jMxEgRATx
q0m9qzAfBgNVHSMEGDAWgBQTk2s4NzJ5lNktYLyD0oli0RgsqDAKBggqhkjOPQQD
AgNIADBFAiBueKyDNiLgTph5gxvoeIG4sywlcZ7JzxWPjgvy6mM6xwIhAP5tU9pZ
uTxXlM1V/h/SZPPaX5Y+Nmyqe+mVyQOrjnRh
-----END CERTIFICATE-----
)";

constexpr std::string_view relay_key = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgcDsozgW+FL+py13F
ilAkInuO5noQH76ZndqbFsKzwXahRANCAATcVdd03UrdDSEiwpe0TdoZj02VR0Gy
90nKRVKZ2/cRbIO1zQ+SoJqMWqqnDWL63uy72IpDxQjBnvUsZvUVVPZv
-----END PRIVATE KEY-----
)";

/** Sends every text message of `ws` back, until its first error. */
template <class Stream>
asio::awaitable<void> echo_messages(beast::websocket::stream<Stream>& ws) {
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
        co_await echo_messages(ws);
    }

    tcp::acceptor acceptor;
};

/**
 * Accepts WebSocket connections over TLS with the relay's certificate, records the name each
 * client sent (SNI) and each handshake that failed, and sends every text message back.
 */
struct tls_echo_server {
    explicit tls_echo_server(asio::io_context& io)
        : acceptor(io, {asio::ip::make_address("127.0.0.1"), 0}),
          context(asio::ssl::context::tls_server) {
        boost::system::error_code ec;
        webcpp::test::require(
            BOOST_TEST(!context.use_certificate_chain(asio::buffer(relay_certificate), ec)));
        webcpp::test::require(BOOST_TEST(
            !context.use_private_key(asio::buffer(relay_key), asio::ssl::context::pem, ec)));
        // Called for every client hello, with the name the client sent or none.
        SSL_CTX_set_tlsext_servername_callback(context.native_handle(), &record_server_name);
        SSL_CTX_set_tlsext_servername_arg(context.native_handle(), this);
        asio::co_spawn(io, accept(), asio::detached);
    }

    tls_echo_server(const tls_echo_server&) = delete;
    tls_echo_server& operator=(const tls_echo_server&) = delete;
    tls_echo_server(tls_echo_server&&) = delete;
    tls_echo_server& operator=(tls_echo_server&&) = delete;
    ~tls_echo_server() = default;

    /** The URL of the relay under `host`, a name or an address of the loopback. */
    [[nodiscard]] std::string url(std::string_view host) const {
        return "wss://" + std::string(host) + ":" +
               std::to_string(acceptor.local_endpoint().port()) + "/relay";
    }

    static int record_server_name(SSL* ssl, int* /*alert*/, void* server) {
        const char* const name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        static_cast<tls_echo_server*>(server)->server_names.emplace_back(name != nullptr ? name
                                                                                         : "");
        return SSL_TLSEXT_ERR_OK;
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

    asio::awaitable<void> echo(tcp::socket socket) {
        beast::websocket::stream<beast::ssl_stream<tcp::socket>> ws(std::move(socket), context);
        auto [shaken] = co_await ws.next_layer().async_handshake(
            asio::ssl::stream_base::server, asio::as_tuple(asio::use_awaitable));
        if (shaken) {
            ++failed_handshakes;
            co_return;
        }
        auto [accepted] = co_await ws.async_accept(asio::as_tuple(asio::use_awaitable));
        if (accepted) {
            co_return;
        }
        co_await echo_messages(ws);
    }

    tcp::acceptor acceptor;
    asio::ssl::context context;
    // Written by the server's own callbacks, also while a test holds the server const.
    mutable std::vector<std::string> server_names;
    mutable int failed_handshakes = 0;
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
    BOOST_TEST(secure->named);
    BOOST_TEST_EQ(secure->host, "relay.example.org");
    BOOST_TEST_EQ(secure->port, "443");
    BOOST_TEST_EQ(secure->handshake_host, "relay.example.org");
    BOOST_TEST_EQ(secure->target, "/");
    const auto plain = trystero::native::detail::parse_url("ws://127.0.0.1:7447/nostr?x=1");
    if (!BOOST_TEST(plain.has_value())) {
        return;
    }
    BOOST_TEST(!plain->secure);
    BOOST_TEST(!plain->named);
    BOOST_TEST_EQ(plain->host, "127.0.0.1");
    BOOST_TEST_EQ(plain->port, "7447");
    BOOST_TEST_EQ(plain->handshake_host, "127.0.0.1:7447");
    BOOST_TEST_EQ(plain->target, "/nostr?x=1");
    BOOST_TEST(!trystero::native::detail::parse_url("https://example.org").has_value());
    BOOST_TEST(!trystero::native::detail::parse_url("wss://").has_value());
}

// An IPv6 address reaches the resolver without its brackets; a scheme in capitals is still a
// WebSocket's; userinfo and a fragment, which a page's WebSocket refuses, are refused.
void urls_name_ipv6_hosts_and_refuse_what_a_page_refuses() {
    const auto ipv6 = trystero::native::detail::parse_url("ws://[::1]:7447/nostr");
    if (!BOOST_TEST(ipv6.has_value())) {
        return;
    }
    BOOST_TEST(!ipv6->named);
    BOOST_TEST_EQ(ipv6->host, "::1");
    BOOST_TEST_EQ(ipv6->port, "7447");
    BOOST_TEST_EQ(ipv6->handshake_host, "[::1]:7447");
    BOOST_TEST_EQ(ipv6->target, "/nostr");
    const auto default_port = trystero::native::detail::parse_url("wss://[2001:db8::1]");
    if (!BOOST_TEST(default_port.has_value())) {
        return;
    }
    BOOST_TEST_EQ(default_port->host, "2001:db8::1");
    BOOST_TEST_EQ(default_port->port, "443");
    BOOST_TEST_EQ(default_port->handshake_host, "[2001:db8::1]");
    const auto capitals = trystero::native::detail::parse_url("WSS://Relay.Example.ORG/Path");
    if (!BOOST_TEST(capitals.has_value())) {
        return;
    }
    BOOST_TEST(capitals->secure);
    BOOST_TEST_EQ(capitals->host, "relay.example.org");
    BOOST_TEST_EQ(capitals->port, "443");
    BOOST_TEST_EQ(capitals->target, "/Path");
    // The scheme's own port, written out, is still the default one; an empty port is too.
    const auto written_default = trystero::native::detail::parse_url("ws://relay.example.org:080");
    if (!BOOST_TEST(written_default.has_value())) {
        return;
    }
    BOOST_TEST_EQ(written_default->port, "80");
    BOOST_TEST_EQ(written_default->handshake_host, "relay.example.org");
    const auto empty_port = trystero::native::detail::parse_url("wss://relay.example.org:/");
    if (!BOOST_TEST(empty_port.has_value())) {
        return;
    }
    BOOST_TEST_EQ(empty_port->port, "443");
    const auto query = trystero::native::detail::parse_url("ws://relay.example.org?x=1");
    if (!BOOST_TEST(query.has_value())) {
        return;
    }
    BOOST_TEST_EQ(query->target, "/?x=1");
    const auto escaped = trystero::native::detail::parse_url("ws://rel%61y.example.org/");
    if (!BOOST_TEST(escaped.has_value())) {
        return;
    }
    BOOST_TEST_EQ(escaped->host, "relay.example.org");
    BOOST_TEST(!trystero::native::detail::parse_url("wss://user:pw@relay.example.org").has_value());
    BOOST_TEST(!trystero::native::detail::parse_url("wss://relay.example.org/#top").has_value());
    BOOST_TEST(!trystero::native::detail::parse_url("wss://relay.example.org:99999").has_value());
    BOOST_TEST(!trystero::native::detail::parse_url("ws:relay.example.org").has_value());
    BOOST_TEST(!trystero::native::detail::parse_url("ws://[v1.x]/").has_value());
    // A name the resolver would read up to its first NUL.
    BOOST_TEST(!trystero::native::detail::parse_url("ws://relay%00.example.org/").has_value());
}

// A relay at an IPv6 literal, here 127.0.0.1 written as an IPv4-mapped IPv6 address so that the
// test stays on the loopback where IPv6 has no address: its brackets must not reach the resolver.
void a_relay_at_an_ipv6_address_opens() {
    asio::io_context io;
    const echo_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    std::vector<std::string> received;
    const auto socket = trystero::native::connect_websocket(
        io.get_executor(), &tls,
        "ws://[::ffff:127.0.0.1]:" + std::to_string(server.acceptor.local_endpoint().port()) +
            "/relay",
        {
            .open = [&] { opened = true; },
            .message = [&](std::string_view text) { received.emplace_back(text); },
            .close = [&] { ++closes; },
        },
        std::pmr::get_default_resource());
    if (!BOOST_TEST(run_until(io, [&] { return opened || closes != 0; }))) {
        return;
    }
    if (!BOOST_TEST(opened)) {
        return;
    }
    socket->send("over IPv6");
    BOOST_TEST(run_until(io, [&] { return received.size() == 1; }));
    BOOST_TEST_EQ(closes, 0);
}

// What a socket reported, recorded by the handlers record() makes.
struct reports {
    bool opened = false;
    int closes = 0;
    std::vector<std::string> received;

    trystero::backend::websocket::handlers record() {
        return {
            .open = [this] { opened = true; },
            .message = [this](std::string_view text) { received.emplace_back(text); },
            .close = [this] { ++closes; },
        };
    }
};

// The context of make_tls_context, the backend's own: a client of TLS 1.2 or newer that verifies
// its peer.
void the_tls_context_verifies_peers_from_tls_1_2() {
    auto tls = trystero::native::make_tls_context();
    if (!BOOST_TEST(tls.has_value())) {
        return;
    }
    BOOST_TEST_EQ(SSL_CTX_get_verify_mode(tls->native_handle()), SSL_VERIFY_PEER);
    BOOST_TEST_EQ(SSL_CTX_get_min_proto_version(tls->native_handle()), TLS1_2_VERSION);
}

// A relay over TLS whose certificate an authority the context trusts signed for the URL's name:
// the socket names the host to the server (SNI), opens, and exchanges texts.
void a_relay_over_tls_opens_and_is_sent_its_name() {
    asio::io_context io;
    tls_echo_server server(io);
    auto tls = webcpp::asio::make_tls_client_context(test_authority);
    if (!BOOST_TEST(tls.has_value())) {
        return;
    }
    reports seen;
    const auto socket =
        trystero::native::connect_websocket(io.get_executor(), &*tls, server.url("localhost"),
                                            seen.record(), std::pmr::get_default_resource());
    if (!BOOST_TEST(run_until(io, [&] { return seen.opened || seen.closes != 0; })) ||
        !BOOST_TEST(seen.opened)) {
        return;
    }
    socket->send("over TLS");
    BOOST_TEST(run_until(io, [&] { return seen.received.size() == 1; }));
    BOOST_TEST_EQ(seen.closes, 0);
    BOOST_TEST_EQ(server.failed_handshakes, 0);
    const std::vector<std::string> names{"localhost"};
    BOOST_TEST_ALL_EQ(server.server_names.begin(), server.server_names.end(), names.begin(),
                      names.end());
}

// The same relay reached at its address, which its certificate does not name: the socket sends
// no name (SNI names no address), refuses the certificate, and closes without opening.
void a_relay_whose_certificate_names_another_host_closes_without_opening() {
    asio::io_context io;
    tls_echo_server server(io);
    auto tls = webcpp::asio::make_tls_client_context(test_authority);
    if (!BOOST_TEST(tls.has_value())) {
        return;
    }
    reports seen;
    const auto socket =
        trystero::native::connect_websocket(io.get_executor(), &*tls, server.url("127.0.0.1"),
                                            seen.record(), std::pmr::get_default_resource());
    BOOST_TEST(run_until(io, [&] { return seen.closes == 1 && server.failed_handshakes == 1; }));
    BOOST_TEST(!seen.opened);
    const std::vector<std::string> names{""};
    BOOST_TEST_ALL_EQ(server.server_names.begin(), server.server_names.end(), names.begin(),
                      names.end());
}

// The same relay under its name, through the backend's own context, which trusts the system's
// authorities and not the test's: the socket refuses the certificate and closes without opening.
void a_relay_of_an_authority_not_trusted_closes_without_opening() {
    asio::io_context io;
    tls_echo_server server(io);
    auto tls = trystero::native::make_tls_context();
    if (!BOOST_TEST(tls.has_value())) {
        return;
    }
    reports seen;
    const auto socket =
        trystero::native::connect_websocket(io.get_executor(), &*tls, server.url("localhost"),
                                            seen.record(), std::pmr::get_default_resource());
    BOOST_TEST(run_until(io, [&] { return seen.closes == 1 && server.failed_handshakes == 1; }));
    BOOST_TEST(!seen.opened);
}

// A backend given the authorities a program trusts reaches a relay they sign; one whose context
// could not be made holds the error, reaches no wss:// relay, and still reaches a ws:// one.
void a_backend_reaches_relays_with_the_tls_context_it_is_given() {
    asio::io_context io;
    const echo_server plain(io);
    const tls_echo_server secure(io);
    {
        trystero::native::backend backend(io.get_executor(),
                                          webcpp::asio::make_tls_client_context(test_authority));
        BOOST_TEST(backend.tls_context().has_value());
        reports seen;
        const auto socket = backend.connect(secure.url("localhost"), seen.record(),
                                            std::pmr::get_default_resource());
        BOOST_TEST(run_until(io, [&] { return seen.opened || seen.closes != 0; }));
        BOOST_TEST(seen.opened);
    }
    trystero::native::backend backend(io.get_executor(), webcpp::asio::make_tls_client_context(""));
    const auto held = backend.tls_context();
    if (!BOOST_TEST(!held.has_value())) {
        return;
    }
    BOOST_TEST(held.error() ==
               webcpp::asio::make_error_code(webcpp::asio::tls_context_error::invalid_ca_bundle));
    const auto handshakes = secure.server_names.size();
    reports refused;
    const auto socket = backend.connect(secure.url("localhost"), refused.record(),
                                        std::pmr::get_default_resource());
    BOOST_TEST(run_until(io, [&] { return refused.closes == 1; }));
    BOOST_TEST(!refused.opened);
    BOOST_TEST_EQ(secure.server_names.size(), handshakes);  // never reached
    reports reached;
    const auto clear =
        backend.connect(plain.url(), reached.record(), std::pmr::get_default_resource());
    BOOST_TEST(run_until(io, [&] { return reached.opened; }));
}

// The backend made without a context of the program's makes its own, the system's authorities'.
void a_backend_makes_its_own_tls_context() {
    asio::io_context io;
    trystero::native::backend backend(io.get_executor());
    BOOST_TEST(backend.tls_context().has_value());
}

void opens_sends_in_order_receives_and_closes_once() {
    asio::io_context io;
    const echo_server server(io);
    asio::ssl::context tls(asio::ssl::context::tls_client);
    bool opened = false;
    int closes = 0;
    std::vector<std::string> received;
    const auto socket = trystero::native::connect_websocket(
        io.get_executor(), &tls, server.url(),
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
        io.get_executor(), &tls, server.url(),
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
        io.get_executor(), &tls, "ws://127.0.0.1:" + std::to_string(port),
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
            trystero::native::connect_websocket(io.get_executor(), &tls, server.url(),
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
    const auto socket = trystero::native::connect_websocket(io.get_executor(), &tls, server.url(),
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
        socket = trystero::native::connect_websocket(io.get_executor(), &tls, server.url(),
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
    const auto socket = trystero::native::connect_websocket(io.get_executor(), &tls, server.url(),
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
        io.get_executor(), &tls, server.url(),
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
        io.get_executor(), &tls, server.url(),
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
    urls_name_ipv6_hosts_and_refuse_what_a_page_refuses();
    a_relay_at_an_ipv6_address_opens();
    the_tls_context_verifies_peers_from_tls_1_2();
    a_relay_over_tls_opens_and_is_sent_its_name();
    a_relay_whose_certificate_names_another_host_closes_without_opening();
    a_relay_of_an_authority_not_trusted_closes_without_opening();
    a_backend_reaches_relays_with_the_tls_context_it_is_given();
    a_backend_makes_its_own_tls_context();
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
