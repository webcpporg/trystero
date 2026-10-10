// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_NATIVE_WEBSOCKET_HPP
#define WEBCPP_TRYSTERO_NATIVE_WEBSOCKET_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/relays.hpp>

#include <boost/asio/any_io_executor.hpp>    // lint-world: the relays' WebSockets
#include <boost/asio/as_tuple.hpp>           // lint-world: the relays' WebSockets
#include <boost/asio/co_spawn.hpp>           // lint-world: the relays' WebSockets
#include <boost/asio/detached.hpp>           // lint-world: the relays' WebSockets
#include <boost/asio/ip/tcp.hpp>             // lint-world: the relays' WebSockets
#include <boost/asio/ssl.hpp>                // lint-world: the relays' WebSockets
#include <boost/asio/use_awaitable.hpp>      // lint-world: the relays' WebSockets
#include <boost/beast/core/flat_buffer.hpp>  // lint-world: the relays' WebSockets
#include <boost/beast/core/tcp_stream.hpp>   // lint-world: the relays' WebSockets
#include <boost/beast/ssl.hpp>               // lint-world: the relays' WebSockets
#include <boost/beast/websocket.hpp>         // lint-world: the relays' WebSockets
#include <boost/beast/websocket/ssl.hpp>     // lint-world: the relays' WebSockets

#include <charconv>
#include <chrono>
#include <deque>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace webcpp::trystero::native {

/**
 * What a native WebSocket tolerates of a relay that stops reading or answering.
 *
 * @see "The native backend", in the guide.
 */
struct websocket_limits {
    /**
     * The most bytes of text queued to write: a relay that stops reading lets them pile up, and a
     * text beyond this drops the connection, which reports a close, so that the room reconnects
     * with its backoff.
     */
    std::size_t max_queued_bytes = protocol::max_queued_relay_bytes;

    /**
     * How long the connection may stay silent: half of it, a keep-alive ping is sent, and a relay
     * that answers nothing within the whole of it is closed, which reports a close.
     *
     * It counts the bytes received, and a ping waits for the write in flight. Native only: a page's
     * `WebSocket` cannot ping, so the browser backend has no idle timeout.
     */
    std::chrono::seconds idle_timeout = std::chrono::seconds(30);
};

namespace detail {

// Its implicit move assignment may allocate: a std::pmr member copies when the two resources
// differ, which bugprone-exception-escape, expecting every move assignment not to throw, reports.
/** A WebSocket URL's parts, in the memory resource it was parsed into. */
struct url_parts {  // NOLINT(bugprone-exception-escape)
    /** Whether the scheme is `wss`. */
    bool secure = false;
    /** The host, a name or an address. */
    std::pmr::string host;
    /** The port, the scheme's default when the URL names none. */
    std::pmr::string port;
    /** The path and the query, `/` when the URL has neither. */
    std::pmr::string target;
};

/** The parts of a `ws://` or `wss://` URL, in `memory`; nothing for any other scheme or without a
 * host. */
inline std::optional<url_parts> parse_url(
    std::string_view url, std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    url_parts out{
        .secure = false,
        .host = std::pmr::string(memory),
        .port = std::pmr::string(memory),
        .target = std::pmr::string(memory),
    };
    if (url.starts_with("wss://")) {
        out.secure = true;
        url.remove_prefix(6);
    } else if (url.starts_with("ws://")) {
        url.remove_prefix(5);
    } else {
        return std::nullopt;
    }
    const auto slash = url.find_first_of("/?");
    const auto authority = url.substr(0, slash);
    out.target = slash == std::string_view::npos ? std::string_view("/") : url.substr(slash);
    if (out.target.front() == '?') {
        out.target.insert(out.target.begin(), '/');
    }
    const auto colon = authority.rfind(':');
    const bool has_port =
        colon != std::string_view::npos && authority.find(']', colon) == std::string_view::npos;
    out.host = authority.substr(0, has_port ? colon : authority.size());
    out.port = has_port ? authority.substr(colon + 1) : std::string_view(out.secure ? "443" : "80");
    if (out.host.empty() || out.port.empty()) {
        return std::nullopt;
    }
    return out;
}

/**
 * One WebSocket connection, run by a coroutine that owns it until the connection ends: connect, TLS
 * for `wss`, the handshake, then a read loop handing each message to the handler as a view of the
 * read buffer. Writes are queued, one in flight at a time, as Beast requires.
 *
 * Tip: the session, its handlers and its queued texts are in the room's resource; Boost.Beast's
 * stream state, the resolver's results and the coroutines' frames are Beast's and Asio's.
 */
template <bool Secure>
class websocket_session : public std::enable_shared_from_this<websocket_session<Secure>> {
public:
    /** Asio's TCP. */
    using tcp = boost::asio::ip::tcp;
    /** The stream the WebSocket runs over: TLS over TCP for `wss`, TCP for `ws`. */
    using next_layer =
        std::conditional_t<Secure, boost::asio::ssl::stream<boost::beast::tcp_stream>,
                           boost::beast::tcp_stream>;
    /** Beast's WebSocket stream. */
    using stream_type = boost::beast::websocket::stream<next_layer>;

    /** A session, not started, for a parsed URL, in `memory`. */
    websocket_session(const boost::asio::any_io_executor& ex, boost::asio::ssl::context& tls,
                      url_parts url, backend::websocket::handlers h,
                      std::pmr::memory_resource* memory, websocket_limits limits)
        : ex_(ex),
          resolver_(ex),
          ws_(make_stream(ex, tls)),
          url_(std::move(url)),
          memory_(memory),
          limits_(limits),
          h_(std::allocate_shared<backend::websocket::handlers>(
              std::pmr::polymorphic_allocator<>(memory), std::move(h))),
          queue_(memory) {}

    /** Starts the coroutine that connects, shakes hands and reads. */
    void start() {
        boost::asio::co_spawn(
            ex_, [self = this->shared_from_this()] { return self->run(); }, boost::asio::detached);
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
            boost::beast::get_lowest_layer(ws_).close();
            return;
        }
        queued_bytes_ += text.size();
        queue_.emplace_back(text);  // a write outlives the call, so the text is copied once, here
        if (std::exchange(writing_, true)) {
            return;  // marked now: the writer only starts once the executor runs it
        }
        boost::asio::co_spawn(
            ex_, [self = this->shared_from_this()] { return self->write_queue(); },
            boost::asio::detached);
    }

    /** Closes with a WebSocket close frame, or drops a connection still being made. */
    void close() {
        if (closing_ || ended_) {
            return;
        }
        closing_ = true;
        if (!open_) {
            boost::beast::get_lowest_layer(ws_)
                .close();  // still connecting: the run ends with an error and reports
            return;
        }
        boost::asio::co_spawn(
            ex_,
            [self = this->shared_from_this()]() -> boost::asio::awaitable<void> {
                co_await self->ws_.async_close(boost::beast::websocket::close_code::normal,
                                               boost::asio::as_tuple(boost::asio::use_awaitable));
                boost::beast::get_lowest_layer(self->ws_).close();
            },
            boost::asio::detached);
    }

    /** The owner let go: nothing is reported any more, and the connection is dropped. */
    void abandon() {
        h_.reset();
        closing_ = true;
        boost::beast::get_lowest_layer(ws_).close();
    }

private:
    static stream_type make_stream(const boost::asio::any_io_executor& ex,
                                   boost::asio::ssl::context& tls) {
        if constexpr (Secure) {
            return stream_type(ex, tls);
        } else {
            (void)tls;
            return stream_type(ex);
        }
    }

    boost::asio::awaitable<void> run() {
        using boost::asio::as_tuple;
        using boost::asio::use_awaitable;
        auto [resolve_error, endpoints] =
            co_await resolver_.async_resolve(url_.host, url_.port, as_tuple(use_awaitable));
        if (resolve_error || closing_) {
            co_return end();
        }
        auto& lowest = boost::beast::get_lowest_layer(ws_);
        lowest.expires_after(std::chrono::seconds(30));
        auto [connect_error, endpoint] =
            co_await lowest.async_connect(endpoints, as_tuple(use_awaitable));
        if (connect_error || closing_) {
            co_return end();
        }
        if constexpr (Secure) {
            auto& tls = ws_.next_layer();
            if (SSL_set_tlsext_host_name(tls.native_handle(), url_.host.c_str()) != 1) {
                co_return end();  // SNI
            }
            tls.set_verify_callback(boost::asio::ssl::host_name_verification(
                std::string(url_.host)));  // Asio's, a std::string
            auto [tls_error] = co_await tls.async_handshake(boost::asio::ssl::stream_base::client,
                                                            as_tuple(use_awaitable));
            if (tls_error || closing_) {
                co_return end();
            }
        }
        lowest.expires_never();
        auto timeouts = boost::beast::websocket::stream_base::timeout::suggested(
            boost::beast::role_type::client);
        timeouts.idle_timeout = limits_.idle_timeout;
        timeouts.keep_alive_pings = true;  // a relay that reads nothing answers no ping
        ws_.set_option(timeouts);
        const bool default_port = url_.port == (Secure ? "443" : "80");
        std::pmr::string host(url_.host, memory_);
        if (!default_port) {
            host.append(":").append(url_.port);
        }
        auto [handshake_error] =
            co_await ws_.async_handshake(host, url_.target, as_tuple(use_awaitable));
        if (handshake_error || closing_) {
            co_return end();
        }
        open_ = true;
        if (const auto h = h_; h && h->open) {
            h->open();
        }
        // A longer message fails the read, which closes the connection, as the browser backend
        // does.
        ws_.read_message_max(protocol::max_relay_message_bytes);
        boost::beast::flat_buffer buffer;
        while (!closing_) {
            auto [read_error, size] = co_await ws_.async_read(buffer, as_tuple(use_awaitable));
            if (read_error) {
                break;
            }
            if (const auto h = h_; h && h->message) {
                const auto data = buffer.cdata();
                h->message(std::string_view(static_cast<const char*>(data.data()), size));
            }
            buffer.consume(buffer.size());
        }
        end();
    }

    /**
     * Writes the queued texts in turn, each moved out of the queue into this frame before its
     * write starts.
     *
     * Tip: the connection can end while a write is in flight, and `end` then clears the queue; the
     * text being written is no longer in it, so the write reads a text it owns, and its completion
     * touches only that text.
     */
    boost::asio::awaitable<void> write_queue() {
        while (!queue_.empty() && open_ && !ended_) {
            const std::pmr::string text = std::move(queue_.front());
            queue_.pop_front();
            queued_bytes_ -= text.size();
            ws_.text(true);
            auto [ec, size] = co_await ws_.async_write(
                boost::asio::buffer(text), boost::asio::as_tuple(boost::asio::use_awaitable));
            if (ec) {
                break;
            }
        }
        writing_ = false;
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

    boost::asio::any_io_executor ex_;
    tcp::resolver resolver_;
    stream_type ws_;
    url_parts url_;
    std::pmr::memory_resource* memory_;
    websocket_limits limits_;
    // Shared so that a handler that destroys the connection's owner during its own call outlives
    // the call.
    std::shared_ptr<backend::websocket::handlers> h_;
    std::pmr::deque<std::pmr::string> queue_;
    std::size_t queued_bytes_ = 0;  // of the texts in queue_
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
 * Connects to a Nostr relay over Boost.Beast: `ws://` in the clear, `wss://` over TLS with `tls`.
 *
 * Writes are queued, one in flight at a time; a received message is handed over as a view of the
 * read buffer, and one longer than `protocol::max_relay_message_bytes` closes the connection. A
 * relay that stops reading is dropped once the queue would pass `limits.max_queued_bytes`, and one
 * that stops answering once `limits.idle_timeout` passes with its keep-alive ping unanswered; both
 * report a close. A URL it cannot parse reports a close, as an unreachable relay does. It needs no
 * exceptions.
 *
 * @param ex The executor every handler runs on.
 * @param tls The TLS context of `wss://`, which must outlive the connection and should verify
 * peers.
 * @param url The relay's URL, read during the call.
 * @param h What the WebSocket reports.
 * @param memory The room's resource, which the connection, its handlers and its queued texts are
 * allocated in; Beast's stream state and the coroutines' frames are Asio's.
 * @param limits What it tolerates of a relay that stops reading or answering.
 * @return The WebSocket; destroying it drops the connection without reporting it.
 * @see "The native backend", in the guide.
 */
inline std::shared_ptr<backend::websocket> connect_websocket(const boost::asio::any_io_executor& ex,
                                                             boost::asio::ssl::context& tls,
                                                             std::string_view url,
                                                             backend::websocket::handlers h,
                                                             std::pmr::memory_resource* memory,
                                                             websocket_limits limits = {}) {
    auto parts = detail::parse_url(url, memory);
    if (!parts) {
        parts = detail::url_parts{
            .secure = false,
            .host = std::pmr::string(memory),
            .port = std::pmr::string(memory),
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

}  // namespace webcpp::trystero::native

#endif
