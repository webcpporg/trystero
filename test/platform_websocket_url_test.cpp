// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests how the platform backend's relay WebSocket reads a relay's URL, with Boost.URL, by the same
// rules on both targets, natively and in a page under node: a scheme, a host, a port and a path;
// an escaped address read as an address; an IPv6 address without its brackets for the resolver;
// what a page's WebSocket refuses (another scheme, userinfo, a fragment, a port beyond 65535, an
// IPvFuture host), and what a page's URL parser reads otherwise than this one (an IPv4 address in
// another form than a dotted quad, a last label that is a number, an international name, a tab, a
// newline, a backslash, a path character outside RFC 3986), each refused. Its cases ran natively
// only, in platform_websocket, before.

#include <webcpp/trystero/platform/websocket.hpp>

#include <boost/core/lightweight_test.hpp>

#include <string>
#include <string_view>

namespace trystero = webcpp::trystero;

namespace {

void urls_name_a_scheme_host_port_and_path() {
    const auto secure = trystero::platform::detail::parse_url("wss://relay.example.org");
    if (!BOOST_TEST(secure.has_value())) {
        return;
    }
    BOOST_TEST(secure->secure);
    BOOST_TEST(secure->named);
    BOOST_TEST_EQ(secure->host, "relay.example.org");
    BOOST_TEST_EQ(secure->port, "443");
    BOOST_TEST_EQ(secure->handshake_host, "relay.example.org");
    BOOST_TEST_EQ(secure->target, "/");
    const auto plain = trystero::platform::detail::parse_url("ws://127.0.0.1:7447/nostr?x=1");
    if (!BOOST_TEST(plain.has_value())) {
        return;
    }
    BOOST_TEST(!plain->secure);
    BOOST_TEST(!plain->named);
    BOOST_TEST_EQ(plain->host, "127.0.0.1");
    BOOST_TEST_EQ(plain->port, "7447");
    BOOST_TEST_EQ(plain->handshake_host, "127.0.0.1:7447");
    BOOST_TEST_EQ(plain->target, "/nostr?x=1");
    BOOST_TEST(!trystero::platform::detail::parse_url("https://example.org").has_value());
    BOOST_TEST(!trystero::platform::detail::parse_url("wss://").has_value());
}

// A host is a name or an address once its escapes are decoded, as a page reads it: 127.0.0.1 with
// escaped digits, a name to Boost.URL, is an address, which TLS never names to the server (SNI).
void urls_read_an_escaped_address_as_an_address() {
    const auto escaped = trystero::platform::detail::parse_url("wss://%31%32%37.0.0.1:7447/");
    if (!BOOST_TEST(escaped.has_value())) {
        return;
    }
    BOOST_TEST(!escaped->named);
    BOOST_TEST_EQ(escaped->host, "127.0.0.1");
    BOOST_TEST_EQ(escaped->handshake_host, "127.0.0.1:7447");
    const auto name = trystero::platform::detail::parse_url("wss://%72elay.example.org/");
    if (!BOOST_TEST(name.has_value())) {
        return;
    }
    BOOST_TEST(name->named);
    BOOST_TEST_EQ(name->host, "relay.example.org");
}

// An IPv6 address reaches the resolver without its brackets; a scheme in capitals is still a
// WebSocket's; userinfo and a fragment, which a page's WebSocket refuses, are refused.
void urls_name_ipv6_hosts_and_refuse_what_a_page_refuses() {
    const auto ipv6 = trystero::platform::detail::parse_url("ws://[::1]:7447/nostr");
    if (!BOOST_TEST(ipv6.has_value())) {
        return;
    }
    BOOST_TEST(!ipv6->named);
    BOOST_TEST_EQ(ipv6->host, "::1");
    BOOST_TEST_EQ(ipv6->port, "7447");
    BOOST_TEST_EQ(ipv6->handshake_host, "[::1]:7447");
    BOOST_TEST_EQ(ipv6->target, "/nostr");
    const auto default_port = trystero::platform::detail::parse_url("wss://[2001:db8::1]");
    if (!BOOST_TEST(default_port.has_value())) {
        return;
    }
    BOOST_TEST_EQ(default_port->host, "2001:db8::1");
    BOOST_TEST_EQ(default_port->port, "443");
    BOOST_TEST_EQ(default_port->handshake_host, "[2001:db8::1]");
    const auto capitals = trystero::platform::detail::parse_url("WSS://Relay.Example.ORG/Path");
    if (!BOOST_TEST(capitals.has_value())) {
        return;
    }
    BOOST_TEST(capitals->secure);
    BOOST_TEST_EQ(capitals->host, "relay.example.org");
    BOOST_TEST_EQ(capitals->port, "443");
    BOOST_TEST_EQ(capitals->target, "/Path");
    // The scheme's own port, written out, is still the default one; an empty port is too.
    const auto written_default =
        trystero::platform::detail::parse_url("ws://relay.example.org:080");
    if (!BOOST_TEST(written_default.has_value())) {
        return;
    }
    BOOST_TEST_EQ(written_default->port, "80");
    BOOST_TEST_EQ(written_default->handshake_host, "relay.example.org");
    const auto empty_port = trystero::platform::detail::parse_url("wss://relay.example.org:/");
    if (!BOOST_TEST(empty_port.has_value())) {
        return;
    }
    BOOST_TEST_EQ(empty_port->port, "443");
    const auto query = trystero::platform::detail::parse_url("ws://relay.example.org?x=1");
    if (!BOOST_TEST(query.has_value())) {
        return;
    }
    BOOST_TEST_EQ(query->target, "/?x=1");
    const auto escaped = trystero::platform::detail::parse_url("ws://rel%61y.example.org/");
    if (!BOOST_TEST(escaped.has_value())) {
        return;
    }
    BOOST_TEST_EQ(escaped->host, "relay.example.org");
    BOOST_TEST(
        !trystero::platform::detail::parse_url("wss://user:pw@relay.example.org").has_value());
    BOOST_TEST(!trystero::platform::detail::parse_url("wss://relay.example.org/#top").has_value());
    BOOST_TEST(!trystero::platform::detail::parse_url("wss://relay.example.org:99999").has_value());
    BOOST_TEST(!trystero::platform::detail::parse_url("ws:relay.example.org").has_value());
    BOOST_TEST(!trystero::platform::detail::parse_url("ws://[v1.x]/").has_value());
    // A name the resolver would read up to its first NUL.
    BOOST_TEST(!trystero::platform::detail::parse_url("ws://relay%00.example.org/").has_value());
}

// The URL, or "refused" when parse_url refuses it, so that a failed check names the URL.
std::string read_or_refused(std::string_view url) {
    return trystero::platform::detail::parse_url(url).has_value() ? std::string(url) : "refused";
}

// What a page's URL parser reads otherwise than this one is refused, never read another way: a
// host a page reads as an IPv4 address in another form than a dotted quad (a shorthand, octal or
// hexadecimal parts, a final dot, a single number), or refuses (a last label of digits); a byte
// beyond ASCII, escaped or not, of an international name, which a page turns into its A-label; a
// tab, a newline or a backslash, which a page drops or reads as a slash; and a path character
// outside RFC 3986, which a page escapes.
void urls_refuse_what_a_page_reads_otherwise() {
    for (const std::string_view url : {
             "ws://127.1/",
             "ws://0x7f.0.0.1/",
             "ws://127.000.0.1/",
             "ws://0177.0.0.1/",
             "ws://127.0.0.1./",
             "ws://2130706433/",
             "ws://relay.123/",
             "ws://relay.0x/",
             "ws://relay.0x1f./",
             "wss://b%C3%BCcher.example/",
             "wss://b\303\274cher.example/",  // a u with diaeresis, its UTF-8 bytes not escaped
             "ws://rel\tay.example.org/",
             "ws://relay.example.org\n/",
             R"(ws:\\relay.example.org\)",
             R"(ws://relay.example.org\nostr)",
             "ws://relay.example.org/a b",
             "ws://relay.example.org/{x}",
             "ws://relay.example.org/b\303\274cher",
         }) {
        BOOST_TEST_EQ(read_or_refused(url), "refused");
    }
    // A last label that is not a number is a name's, a final dot included.
    for (const std::string_view url : {
             "ws://relay.example.org./",
             "ws://relay.example.1a/",
             "ws://relay.0xg/",
             "ws://1.2.3.example/",
         }) {
        BOOST_TEST_EQ(read_or_refused(url), url);
    }
}

}  // namespace

int main() {
    urls_name_a_scheme_host_port_and_path();
    urls_read_an_escaped_address_as_an_address();
    urls_name_ipv6_hosts_and_refuse_what_a_page_refuses();
    urls_refuse_what_a_page_reads_otherwise();
    return boost::report_errors();
}
