// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PLATFORM_WEBSOCKET_HPP
#define WEBCPP_TRYSTERO_PLATFORM_WEBSOCKET_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/post.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/relays.hpp>

#include <webcpp/asio/any_io_executor.hpp>    // lint-world: the relays' WebSockets
#include <webcpp/asio/bind_allocator.hpp>     // lint-world: binds an allocator only
#include <webcpp/asio/buffer.hpp>             // lint-world: a text's buffer only
#include <webcpp/asio/ip/tcp.hpp>             // lint-world: the relays' WebSockets
#include <webcpp/asio/ssl.hpp>                // lint-world: the relays' TLS
#include <webcpp/beast/core/flat_buffer.hpp>  // lint-world: the relays' WebSockets
#include <webcpp/beast/ssl.hpp>               // lint-world: the relays' TLS
#include <webcpp/beast/tcp_stream.hpp>        // lint-world: the relays' WebSockets
#include <webcpp/beast/websocket.hpp>         // lint-world: the relays' WebSockets

#ifndef WEBCPP_TARGET_EMSCRIPTEN
#include <webcpp/beast/websocket/bounded_handshake_stream.hpp>  // lint-world: the relays' WebSockets

#include <openssl/err.h>
#include <openssl/ssl.h>
#endif
#include <boost/system/errc.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <boost/url/grammar/digit_chars.hpp>
#include <boost/url/grammar/hexdig_chars.hpp>
#include <boost/url/ipv4_address.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/scheme.hpp>
#include <boost/url/url_view.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace webcpp::trystero::platform {

/**
 * What a relay's WebSocket tolerates of a relay that stops reading or answering.
 *
 * @see "The platform backend", in the guide.
 */
struct websocket_limits {
    /**
     * The most bytes of text queued to write: a relay that stops reading lets them pile up, and a
     * text beyond this drops the connection, which reports a close, so that the room reconnects
     * with its backoff.
     *
     * It bounds the session's own queue on both targets, and in a page also what the page's
     * `WebSocket` holds unsent, its `bufferedAmount`, through webcpp::beast's `queue_limits::send`.
     */
    std::size_t max_queued_bytes = protocol::max_queued_relay_bytes;

    /**
     * How long the connection may stay silent: half of it, a keep-alive ping is sent, and a relay
     * that answers nothing within the whole of it is closed, which reports a close.
     *
     * It counts the bytes received, and a ping waits for the write in flight. Native only: a page's
     * `WebSocket` cannot ping, so a page's relay socket has no idle timeout.
     */
    std::chrono::seconds idle_timeout = std::chrono::seconds(30);
};

namespace detail {

/** The error OpenSSL queued last, or `not_enough_memory` when none; the queue is emptied. */
inline boost::system::error_code openssl_error() {
#ifndef WEBCPP_TARGET_EMSCRIPTEN
    // The queue's first error may be one an earlier call left behind; the last is the failure's.
    const auto code = ERR_peek_last_error();
    ERR_clear_error();
    // Asio's own TLS errors are OpenSSL's codes, in its category, as an int.
    if (code != 0) {
        return {static_cast<int>(code), webcpp::asio::error::get_ssl_category()};
    }
#endif
    // OpenSSL queues no reason when it cannot allocate a context.
    return boost::system::errc::make_error_code(boost::system::errc::not_enough_memory);
}

}  // namespace detail

/**
 * Makes the TLS context of the relays' `wss://` connections, a client of TLS 1.2 or newer that
 * verifies each relay against the system's certificate authorities.
 *
 * Natively the authorities are those OpenSSL's default verify paths name, the system's own; a
 * relay whose certificate does not verify is unreachable. On Windows those paths are in OpenSSL's
 * build-time directory, which neither Windows nor webcpp fills, and OpenSSL reads no Windows
 * certificate store by default, so there are none there unless `SSL_CERT_FILE` or `SSL_CERT_DIR`
 * names some: a Windows program gives the backend its own context. A program that trusts other
 * authorities, as webcpp's embedded Mozilla roots (`webcpp::asio::make_tls_client_context()`) or a
 * private one, makes its context itself and gives it to the backend. In the browser the page's
 * `WebSocket` verifies the relay against the browser's authorities, and the context records only
 * the policy its stream asks for: a client that verifies its peer.
 *
 * @return The context, or the error that kept OpenSSL from making it or from finding the system's
 * authorities.
 * @see "The relays", in the guide.
 */
inline boost::system::result<webcpp::asio::ssl::context> make_tls_context() {
#ifndef WEBCPP_TARGET_EMSCRIPTEN
    // What an earlier call left in OpenSSL's queue is not this one's error.
    ERR_clear_error();
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> raw(SSL_CTX_new(TLS_client_method()),
                                                          &SSL_CTX_free);
    if (!raw) {
        return detail::openssl_error();
    }
    if (SSL_CTX_set_min_proto_version(raw.get(), TLS1_2_VERSION) != 1) {
        return detail::openssl_error();
    }
    SSL_CTX_set_options(raw.get(), SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_verify(raw.get(), SSL_VERIFY_PEER, nullptr);
    // The system's certificate authorities: the files of OpenSSL's default verify paths, read as
    // Asio's set_default_verify_paths(ec) reads them, which also returns a copy of its error.
    if (SSL_CTX_set_default_verify_paths(raw.get()) != 1) {  // lint-world: the system's authorities
        return detail::openssl_error();
    }
    // The context owns the handle from here, and throws only for a null one.
    return webcpp::asio::ssl::context(raw.release());
#else
    webcpp::asio::ssl::context context(webcpp::asio::ssl::context::tls_client);
    boost::system::error_code ec;
    context.set_verify_mode(webcpp::asio::ssl::verify_peer, ec);
    if (ec) {
        return ec;
    }
    return context;
#endif
}

namespace detail {

// Its implicit move assignment may allocate: a std::pmr member copies when the two resources
// differ, which bugprone-exception-escape, expecting every move assignment not to throw, reports.
/** A WebSocket URL's parts, in the memory resource it was parsed into. */
struct url_parts {  // NOLINT(bugprone-exception-escape)
    /** Whether the scheme is `wss`. */
    bool secure = false;
    /** Whether the host is a name, not an address: TLS names only a name to the server (SNI). */
    bool named = false;
    /**
     * The host the resolver looks up: a name in lower case, or an address, IPv6 without brackets.
     */
    std::pmr::string host;
    /** The port, in decimal, the scheme's default when the URL names none. */
    std::pmr::string port;
    /**
     * The host the handshake names, as a page's `WebSocket` does: an IPv6 address in brackets, and
     * the port unless it is the scheme's default.
     */
    std::pmr::string handshake_host;
    /** The path and the query, `/` when the URL has neither. */
    std::pmr::string target;
};

/**
 * Whether a decoded host name holds a byte that a page's URL parser refuses in a host, or a byte
 * beyond ASCII, of an international name that a page turns into its A-label and this parser does
 * not.
 */
inline bool forbidden_in_host(std::string_view name) {
    constexpr std::string_view forbidden = "#%/:<>?@[\\]^|";
    return std::ranges::any_of(name, [forbidden](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte <= 0x20U || byte >= 0x7FU || forbidden.find(c) != std::string_view::npos;
    });
}

/**
 * Whether a decoded host name in lower case ends in a number, as a page's URL parser asks before it
 * reads the host as an IPv4 address: its last label, a final empty one left out, is decimal digits,
 * or `0x` and hexadecimal digits.
 */
inline bool ends_in_a_number(std::string_view name) {
    if (name.ends_with('.')) {
        name.remove_suffix(1);
    }
    const auto dot = name.rfind('.');
    const auto last = dot == std::string_view::npos ? name : name.substr(dot + 1);
    if (last.starts_with("0x")) {
        return std::ranges::all_of(last.substr(2), boost::urls::grammar::hexdig_chars);
    }
    return !last.empty() && std::ranges::all_of(last, boost::urls::grammar::digit_chars);
}

/**
 * The parts of a `ws://` or `wss://` URL, in `memory`, read with Boost.URL; nothing for another
 * scheme, for a URL without a host, with userinfo or a fragment, which a page's `WebSocket` refuses
 * too, with a port beyond 65535, or with a host that a page's URL parser refuses or reads
 * otherwise.
 */
inline std::optional<url_parts> parse_url(
    std::string_view url, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    const auto parsed = boost::urls::parse_uri(url);
    if (!parsed) {
        return std::nullopt;
    }
    const boost::urls::url_view& view = *parsed;
    const auto scheme = view.scheme_id();
    if ((scheme != boost::urls::scheme::ws && scheme != boost::urls::scheme::wss) ||
        !view.has_authority() || view.has_userinfo() || view.has_fragment()) {
        return std::nullopt;
    }
    const bool secure = scheme == boost::urls::scheme::wss;
    const std::uint16_t default_port = secure ? 443 : 80;
    std::uint16_t port = default_port;
    if (const auto text = view.port(); view.has_port() && !text.empty()) {
        const auto* const end = text.data() + text.size();
        const auto [last, error] = std::from_chars(text.data(), end, port);
        if (error != std::errc{} || last != end) {
            return std::nullopt;
        }
    }
    url_parts out{
        .secure = secure,
        .named = false,
        .host = std::pmr::string(memory),
        .port = std::pmr::string(memory),
        .handshake_host = std::pmr::string(memory),
        .target = std::pmr::string(memory),
    };
    const auto address = view.encoded_host_address();
    switch (view.host_type()) {
        case boost::urls::host_type::name: {
            const auto decoded = *address;
            out.host.assign(decoded.begin(), decoded.end());
            // A host name is case-insensitive; a page's URL parser writes it in lower case.
            std::ranges::transform(out.host, out.host.begin(), [](char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            });
            if (out.host.empty() || forbidden_in_host(out.host)) {
                return std::nullopt;
            }
            // Escaped digits make Boost.URL read an address as a name; decoded, it is an address,
            // as a page reads it, and TLS names no address to the server. A decoded name holds no
            // ':', refused above, so it is never an IPv6 address.
            out.named = !boost::urls::parse_ipv4_address(out.host).has_value();
            // A page reads a host that ends in a number as an IPv4 address in any of its forms
            // (127.1, octal and hexadecimal parts, a single number), or refuses it: a form other
            // than a dotted quad is refused here, never resolved as a name.
            if (out.named && ends_in_a_number(out.host)) {
                return std::nullopt;
            }
            out.handshake_host = out.host;
            break;
        }
        case boost::urls::host_type::ipv4:
            out.host.assign(address.data(), address.size());
            out.handshake_host = out.host;
            break;
        case boost::urls::host_type::ipv6:
            out.host.assign(address.data(), address.size());
            out.handshake_host.push_back('[');
            out.handshake_host += out.host;
            out.handshake_host.push_back(']');
            break;
        default:  // none, or an IPvFuture address, which no resolver knows
            return std::nullopt;
    }
    std::array<char, 5> digits{};
    const auto written = std::to_chars(digits.data(), digits.data() + digits.size(), port);
    out.port.assign(digits.data(), written.ptr);
    if (port != default_port) {
        out.handshake_host.push_back(':');
        out.handshake_host += out.port;
    }
    const auto target = view.encoded_target();
    if (target.empty() || target.front() != '/') {
        out.target.assign("/");
    }
    out.target.append(target.data(), target.size());
    return out;
}

/**
 * One WebSocket connection over webcpp::beast, on both targets, composed of Asio operations on the
 * room's executor, each a callback that holds the session until it completes: resolve, connect, TLS
 * for `wss`, the handshake, then a read loop handing each message to the handler as a view of the
 * read buffer. Writes are queued, one in flight at a time, as Beast requires; a write that fails
 * ends the connection, which reports its close once.
 *
 * Tip: every operation's handler carries the room's allocator, so that what Asio and Beast
 * allocate for it, and the read buffer, are in the room's resource beside the session, its
 * handlers and its queued texts; the reads' and the writes' handlers have no executor of their own
 * (`transfer_handler`), so that Asio wraps none of their completions. Natively what Beast, Asio
 * and OpenSSL allocate of their own takes no allocator and stays outside the room's resource, for
 * each connection: the WebSocket stream's state and the TCP stream's, the WebSocket's write buffer
 * and, over TLS, Asio's two TLS buffers and OpenSSL's state; the resolver's state and results; the
 * handshake's request and response and the header fields of both, the response's header at most
 * 8 KiB, which webcpp::beast's bounded handshake layer reads through Beast's own parser, keeping a
 * header line split between two reads in the room's resource, and reading no byte of the body of a
 * response that refuses the upgrade; the verify callback Asio's `set_verify_callback` makes with
 * `new`, with its copy of the host; each wait of Beast's timers, from Asio's per-thread recycling
 * cache (the TCP stream's deadline over the connection and the TLS handshake, the WebSocket's
 * handshake, idle and close timeouts, the idle one started again at each read); and each
 * keep-alive ping's frame. On Emscripten the page's stream keeps its state in the room's resource
 * too, and its `WebSocket` makes the handshake; what stays outside is, for each connection, the
 * results webcpp::asio's page resolver makes with `new` (Asio's list of one entry, its shared count
 * and the entry's storage, and two copies of the host when it is longer than a short string holds,
 * 10 bytes in a page), and, for each event of the page's `WebSocket` (its open, each message and
 * its close), the C library's allocation embind makes for the `std::string` into which
 * webcpp::beast's page stream reads the event's type, and for a message a second, for the type of
 * its data, each freed before the call that made it returns.
 */
template <bool Secure>
class websocket_session : public std::enable_shared_from_this<websocket_session<Secure>> {
public:
    /** The executor every handler runs on. */
    using executor_type = webcpp::asio::any_io_executor;
    /** Asio's TCP. */
    using tcp = webcpp::asio::ip::tcp;
    /** The transport the WebSocket runs over: TLS over TCP for `wss`, TCP for `ws`. */
    using transport =
        std::conditional_t<Secure, webcpp::beast::ssl_stream<webcpp::beast::tcp_stream>,
                           webcpp::beast::tcp_stream>;
#ifdef WEBCPP_TARGET_EMSCRIPTEN
    /** The stream the WebSocket runs over: in a page the transport, whose WebSocket reads its own
     * handshake response. */
    using next_layer = transport;
#else
    /**
     * The stream the WebSocket runs over: natively the transport under webcpp::beast's bounded
     * handshake layer, which reads a refusal's header alone, a split header line kept in the room's
     * resource.
     */
    using next_layer =
        webcpp::beast::websocket::bounded_handshake_stream<transport,
                                                           std::pmr::polymorphic_allocator<char>>;
#endif
    /** Beast's WebSocket stream. */
    using stream_type = webcpp::beast::websocket::stream<next_layer>;

    /** A session, not started, for a parsed URL, in `memory`; `tls` is read for `wss` only. */
    websocket_session(const executor_type& ex, webcpp::asio::ssl::context* tls, url_parts url,
                      backend::websocket::handlers h, std::pmr::memory_resource* memory,
                      websocket_limits limits)
        : ex_(ex),
          resolver_(ex),
          ws_(make_stream(ex, tls, memory)),
          url_(std::move(url)),
          memory_(memory),
          limits_(limits),
          h_(std::allocate_shared<backend::websocket::handlers>(
              std::pmr::polymorphic_allocator<>(memory), std::move(h))),
          buffer_(std::pmr::polymorphic_allocator<char>(memory)),
          queue_(memory),
          writing_text_(memory) {}

    /**
     * Starts to resolve the relay's host; a session without a host, whose URL did not parse,
     * reports its close from the executor instead.
     */
    void start() {
        if (url_.host.empty()) {
            trystero::detail::post(ex_, memory_,
                                   [self = this->shared_from_this()] { self->end(); });
            return;
        }
        resolver_.async_resolve(
            url_.host, url_.port,
            bound([self = this->shared_from_this()](const boost::system::error_code& ec,
                                                    const tcp::resolver::results_type& results) {
                self->on_resolve(ec, results);
            }));
    }

    /**
     * Queues a text to write, when open and not closing; one write is in flight at a time. A text
     * that would take the queue beyond `websocket_limits::max_queued_bytes` drops the connection.
     */
    void send(std::string_view text) {
        if (!open_ || closing_) {
            return;
        }
        if (queued_bytes_ + text.size() > limits_.max_queued_bytes) {
            // The relay stopped reading: no close frame could reach it, so the socket is closed,
            // the read fails, and end() reports the close.
            closing_ = true;
            webcpp::beast::get_lowest_layer(ws_).close();
            return;
        }
        queued_bytes_ += text.size();
        queue_.emplace_back(text);  // a write outlives the call, so the text is copied once, here
        if (std::exchange(writing_, true)) {
            return;
        }
        write_next();
    }

    /** Closes with a WebSocket close frame, or drops a connection still being made. */
    void close() {
        if (closing_ || ended_) {
            return;
        }
        closing_ = true;
        if (!open_) {
            // Still connecting: the operation under way ends with an error, and reports.
            resolver_.cancel();
            webcpp::beast::get_lowest_layer(ws_).close();
            return;
        }
        ws_.async_close(webcpp::beast::websocket::close_code::normal,
                        bound([self = this->shared_from_this()](const boost::system::error_code&) {
                            webcpp::beast::get_lowest_layer(self->ws_).close();
                        }));
    }

    /** The owner let go: nothing is reported any more, and the connection is dropped. */
    void abandon() {
        h_.reset();
        closing_ = true;
        resolver_.cancel();
        webcpp::beast::get_lowest_layer(ws_).close();
    }

private:
    /** The stream, natively Boost.Beast's own; on Emscripten its state is in `memory`. */
    static stream_type make_stream(const executor_type& ex, webcpp::asio::ssl::context* tls,
                                   std::pmr::memory_resource* memory) {
#ifdef WEBCPP_TARGET_EMSCRIPTEN
        const std::pmr::polymorphic_allocator<std::byte> allocator(memory);
        if constexpr (Secure) {
            return stream_type(std::allocator_arg, allocator, ex, *tls);
        } else {
            (void)tls;
            return stream_type(std::allocator_arg, allocator, ex);
        }
#else
        const std::pmr::polymorphic_allocator<char> allocator(memory);
        if constexpr (Secure) {
            return stream_type(std::allocator_arg, allocator, ex, *tls);
        } else {
            (void)tls;
            return stream_type(std::allocator_arg, allocator, ex);
        }
#endif
    }

    /** The TLS stream, natively under the bounded handshake layer, in a page the next layer. */
    transport& tls_stream() {
#ifdef WEBCPP_TARGET_EMSCRIPTEN
        return ws_.next_layer();
#else
        return ws_.next_layer().next_layer();
#endif
    }

    /** `f` with the room's allocator, which Asio and Beast allocate its operation with. */
    template <class F>
    auto bound(F&& f) const {
        return webcpp::asio::bind_allocator(std::pmr::polymorphic_allocator<>(memory_),
                                            std::forward<F>(f));
    }

    void on_resolve(const boost::system::error_code& ec,
                    const tcp::resolver::results_type& results) {
        if (ec || closing_) {
            end();
            return;
        }
        auto& lowest = webcpp::beast::get_lowest_layer(ws_);
        lowest.expires_after(std::chrono::seconds(30));  // the connection and the TLS handshake
        lowest.async_connect(
            results, bound([self = this->shared_from_this()](const boost::system::error_code& error,
                                                             const tcp::endpoint&) {
                self->on_connect(error);
            }));
    }

    void on_connect(const boost::system::error_code& ec) {
        if (ec || closing_) {
            end();
            return;
        }
        if constexpr (Secure) {
            auto& tls = tls_stream();
            // The relay is verified whatever the context's own mode, which a program's context
            // made as Asio makes a client's leaves at verify_none: natively a certificate that does
            // not verify fails the handshake; a page's WebSocket always verifies, and its stream
            // refuses a mode other than verify_peer, which it reads from the stream before the
            // context.
            boost::system::error_code verify;
            tls.set_verify_mode(webcpp::asio::ssl::verify_peer, verify);
            if (verify) {
                end();
                return;
            }
#ifndef WEBCPP_TARGET_EMSCRIPTEN
            // SNI names a host name only, never an address (RFC 6066).
            if (url_.named && !webcpp::beast::set_server_name(tls, url_.host)) {
                end();
                return;
            }
            // Asio's verification takes a std::string, and reads an address as one.
            tls.set_verify_callback(
                webcpp::asio::ssl::host_name_verification(std::string(url_.host)));
#endif
            // In a page its WebSocket sends the name and checks the certificate against the host.
            tls.async_handshake(
                webcpp::asio::ssl::stream_base::client,
                bound([self = this->shared_from_this()](const boost::system::error_code& error) {
                    self->on_tls(error);
                }));
        } else {
            handshake();
        }
    }

    void on_tls(const boost::system::error_code& ec) {
        if (ec || closing_) {
            end();
            return;
        }
        handshake();
    }

    void handshake() {
        webcpp::beast::get_lowest_layer(ws_).expires_never();
        // NOLINTNEXTLINE(misc-const-correctness): changed natively only
        auto timeouts = webcpp::beast::websocket::stream_base::timeout::suggested(
            webcpp::beast::role_type::client);
#ifndef WEBCPP_TARGET_EMSCRIPTEN
        timeouts.idle_timeout = limits_.idle_timeout;
        timeouts.keep_alive_pings = true;  // a relay that reads nothing answers no ping
#endif
        // A page's WebSocket cannot ping, and its stream refuses keep-alive pings: no idle timeout.
        ws_.set_option(timeouts);
#ifdef WEBCPP_TARGET_EMSCRIPTEN
        // The page's WebSocket writes at once: what a relay does not read piles up in its
        // bufferedAmount, which the stream bounds as the session bounds its queue, and closes past.
        ws_.set_option(webcpp::beast::websocket::queue_limits{
            .send = limits_.max_queued_bytes,
            .receive = webcpp::beast::websocket::queue_limits{}.receive,
        });
#endif
        ws_.text(true);
        // A longer message fails the read, which closes the connection. It is set before the
        // handshake: a page's stream checks each message as its event arrives, and a relay's first
        // ones can arrive in the same task of the page as its open, before the handshake's
        // completion runs on the executor.
        ws_.read_message_max(protocol::max_relay_message_bytes);
#ifndef WEBCPP_TARGET_EMSCRIPTEN
        // A relay that refuses the upgrade has its response's header read, at most 8 KiB, and no
        // byte of its body: Beast's own parser would read a body of up to 8 MiB outside the room's
        // resource. A page's WebSocket reads its own response.
        ws_.next_layer().limits({.body = 0});
#endif
        ws_.async_handshake(
            url_.handshake_host, url_.target,
            bound([self = this->shared_from_this()](const boost::system::error_code& ec) {
                self->on_handshake(ec);
            }));
    }

    void on_handshake(const boost::system::error_code& ec) {
        if (ec || closing_) {
            end();
            return;
        }
        open_ = true;
        if (const auto h = h_; h && h->open) {
            h->open();
        }
        read();
    }

    /**
     * A read's or a write's completion: the session, and the member it calls, through a pointer.
     *
     * Tip: the reads, and the writes, are a loop of asynchronous operations, not a recursion: each
     * handler runs from the executor once the call that started its operation has returned, so the
     * stack never grows. The member is called through a pointer, so that the analysis of calls
     * (misc-no-recursion) sees no cycle through Asio's post on either target, natively in Boost's
     * headers and on Emscripten in webcpp::asio's and webcpp::beast's own, where the reports would
     * land. Its allocator is the room's, and it has no executor of its own, so that Asio completes
     * it on the stream's executor, which it finds is an io_context's. Asio's type-erased handler,
     * `any_completion_handler`, which the session used before, has an executor of another type,
     * `any_completion_executor`: Asio made one around the stream's with `new` for each operation,
     * and dispatched each completion through it in a function object of its own (measured natively:
     * six `operator new` and four of Asio's recycled blocks for a text sent and its echo received).
     */
    struct transfer_handler {
        /** The room's allocator, which Asio and Beast allocate the operation with. */
        using allocator_type = std::pmr::polymorphic_allocator<>;

        /** The session, held until the operation completes. */
        std::shared_ptr<websocket_session> self;
        /** What the completion calls. */
        void (websocket_session::*member)(const boost::system::error_code&, std::size_t);

        /** The room's allocator. */
        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return allocator_type(self->memory_);
        }

        /** Calls the member with the operation's result. */
        void operator()(const boost::system::error_code& ec, std::size_t size) const {
            ((*self).*member)(ec, size);
        }
    };

    void read() {
        ws_.async_read(buffer_, transfer_handler{
                                    .self = this->shared_from_this(),
                                    .member = &websocket_session::on_read,
                                });
    }

    void on_read(const boost::system::error_code& ec, std::size_t size) {
        if (ec || ended_) {  // a failed write ended it: nothing is reported after its close
            end();
            return;
        }
        // A relay's messages are texts (NIP-01): a binary one is dropped, as the room would drop a
        // text that is no Nostr message.
        if (const auto h = h_; h && h->message && ws_.got_text()) {
            const auto data = buffer_.cdata();
            h->message(std::string_view(static_cast<const char*>(data.data()), size));
        }
        buffer_.consume(buffer_.size());
        if (closing_) {
            end();
            return;
        }
        read();
    }

    /**
     * Writes the next queued text, moved out of the queue into `writing_text_` before its write
     * starts; the completion writes the one after it.
     *
     * Tip: the connection can end while a write is in flight, and `end` then clears the queue; the
     * text being written is no longer in it, so the write reads a text the session owns, and its
     * completion touches only that text. Its completion calls it again from the executor, a loop
     * and not a recursion, as the reads' (`transfer_handler`).
     */
    void write_next() {
        if (queue_.empty() || !open_ || ended_) {
            writing_ = false;
            return;
        }
        writing_text_ = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= writing_text_.size();
        ws_.async_write(webcpp::asio::buffer(writing_text_),
                        transfer_handler{
                            .self = this->shared_from_this(),
                            .member = &websocket_session::on_write,
                        });
    }

    void on_write(const boost::system::error_code& ec, std::size_t /*size*/) {
        if (ec) {
            fail();
            return;
        }
        write_next();
    }

    /**
     * A write failed: the relay is unreachable, or, in a page, its `WebSocket` held more than the
     * bound unsent, whose close event may never come. The connection is dropped and its close
     * reported at once, without waiting for the read to fail; one already closing, whose writes
     * fail as it closes, ends as its read does.
     */
    void fail() {
        writing_ = false;
        if (std::exchange(closing_, true)) {
            return;
        }
        webcpp::beast::get_lowest_layer(ws_).close();
        end();
    }

    void end() {
        if (std::exchange(ended_, true)) {
            return;
        }
        open_ = false;
        queue_.clear();
        queued_bytes_ = 0;
        if (const auto h = h_) {
            if (auto f = std::exchange(h->close, {})) {
                f();
            }
        }
    }

    executor_type ex_;
    tcp::resolver resolver_;
    stream_type ws_;
    url_parts url_;
    std::pmr::memory_resource* memory_;
    websocket_limits limits_;
    // Shared so that a handler that destroys the connection's owner during its own call outlives
    // the call.
    std::shared_ptr<backend::websocket::handlers> h_;
    webcpp::beast::basic_flat_buffer<std::pmr::polymorphic_allocator<char>> buffer_;
    std::pmr::deque<std::pmr::string> queue_;
    std::size_t queued_bytes_ = 0;   // of the texts in queue_
    std::pmr::string writing_text_;  // the text whose write is in flight, out of queue_
    bool open_ = false;
    bool writing_ = false;
    bool closing_ = false;
    bool ended_ = false;
};

/** What the caller holds: dropping it abandons the session, which may still be finishing an
 * operation. */
template <bool Secure>
class websocket_handle : public backend::websocket {
public:
    /** A handle of a started session. */
    explicit websocket_handle(std::shared_ptr<websocket_session<Secure>> session)
        : session_(std::move(session)) {}

    /** Abandons the session. */
    ~websocket_handle() override { session_->abandon(); }

    /** Sends a text through the session. */
    void send(std::string_view text) override { session_->send(text); }

    /** Closes the session. */
    void close() override { session_->close(); }

private:
    std::shared_ptr<websocket_session<Secure>> session_;
};

}  // namespace detail

/**
 * Connects to a Nostr relay over webcpp::beast, on both targets: `ws://` in the clear, `wss://`
 * over TLS with `tls`; natively over Boost.Beast's own stream, in a page over its `WebSocket`.
 *
 * Writes are queued, one in flight at a time; a received text is handed over as a view of the
 * read buffer, a binary message is dropped, and a message longer than
 * `protocol::max_relay_message_bytes` closes the connection, a page's before it is copied into
 * WebAssembly, the relay's first ones too. A relay that stops reading is dropped once the
 * queue, or a page's `bufferedAmount`, would pass `limits.max_queued_bytes`, and, natively, one
 * that stops answering once `limits.idle_timeout` passes with its keep-alive ping unanswered; each
 * reports one close. The URL is read with Boost.URL: `ws://` or `wss://` in any case, a host name
 * with its escapes decoded, an IPv4 address as a dotted quad or an IPv6 address in brackets, and a
 * port up to 65535. A URL it refuses, which includes what a page's URL parser reads otherwise (an
 * IPv4 shorthand such as `127.1`, an international name, a tab or a backslash), and a `wss://` URL
 * without `tls`, report a close, as an unreachable relay does. A `wss://` connection asks for the
 * relay's verification whatever the verify mode of `tls`, on both targets. Natively it names the
 * host to the server (SNI) when it is a name, verifies the relay's certificate against the
 * authorities of `tls`, and checks it against the URL's host, a name or an address; in a page the
 * browser does all three, against its own authorities, and the connection reports a close, making
 * no `WebSocket`, when `tls` asks for what a page cannot honour: authorities added, a client
 * certificate or key, options, a method other than `tls_client` or `sslv23_client`, or a verify
 * callback other than `host_name_verification` of the relay's host. It needs no exceptions.
 *
 * The connection, its handlers, its queued texts, its read buffer and its operations are in
 * `memory`. What stays outside it, for each connection: natively what Boost.Beast, Asio and OpenSSL
 * allocate of their own, their streams' state, the resolver's, the handshake's request and
 * response, the verify callback, their timers' waits, the idle timer's again at each read, and
 * each keep-alive ping's frame; a relay that answers the handshake with a status other than 101
 * has its response's header read, at most 8 KiB, and no byte of its body, through webcpp::beast's
 * bounded handshake layer, before the connection reports its close. In a page, the page
 * resolver's results, made with `new`, and, for each event of the page's `WebSocket`, the open,
 * each message and the close, the C library's allocation embind makes for its type, and for a
 * message a second, for the type of its data, each freed before the call that made it returns.
 *
 * @param ex The executor every handler runs on.
 * @param tls The TLS context of `wss://`, which must outlive the connection, or null when the
 * program has none; the connection verifies the relay even when its verify mode is `verify_none`,
 * as Asio makes a client's context, natively against its authorities and in a page against the
 * browser's.
 * @param url The relay's URL, read during the call.
 * @param h What the WebSocket reports.
 * @param memory The room's resource, which the connection's own allocations are in.
 * @param limits What it tolerates of a relay that stops reading or answering.
 * @return The WebSocket; destroying it drops the connection without reporting it.
 * @see "The platform backend", in the guide.
 * @see "What stays outside the resource", in the guide.
 */
inline std::shared_ptr<backend::websocket> connect_websocket(
    const webcpp::asio::any_io_executor& ex, webcpp::asio::ssl::context* tls, std::string_view url,
    backend::websocket::handlers h, std::pmr::memory_resource* memory,
    websocket_limits limits = {}) {
    auto parts = detail::parse_url(url, memory);
    if (!parts || (parts->secure && tls == nullptr)) {
        // A session without a host reports its close once started.
        parts = detail::url_parts{
            .secure = false,
            .named = false,
            .host = std::pmr::string(memory),
            .port = std::pmr::string(memory),
            .handshake_host = std::pmr::string(memory),
            .target = std::pmr::string(memory),
        };
    }
    const std::pmr::polymorphic_allocator<> allocator(memory);
    if (parts->secure) {
        const auto session = std::allocate_shared<detail::websocket_session<true>>(
            allocator, ex, tls, std::move(*parts), std::move(h), memory, limits);
        session->start();
        return std::allocate_shared<detail::websocket_handle<true>>(allocator, session);
    }
    const auto session = std::allocate_shared<detail::websocket_session<false>>(
        allocator, ex, tls, std::move(*parts), std::move(h), memory, limits);
    session->start();
    return std::allocate_shared<detail::websocket_handle<false>>(allocator, session);
}

}  // namespace webcpp::trystero::platform

#endif
