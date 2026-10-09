// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_NATIVE_WEBSOCKET_HPP
#define WEBCPP_TRYSTERO_NATIVE_WEBSOCKET_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <charconv>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace webcpp::trystero::native {

namespace detail {

struct url_parts {
    bool secure = false;
    std::string host;
    std::string port;
    std::string target;
};

/** The parts of a `ws://` or `wss://` URL; nothing for any other scheme or without a host. */
inline std::optional<url_parts> parse_url(std::string_view url) {
    url_parts out;
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
    out.target = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
    if (out.target.front() == '?') out.target.insert(out.target.begin(), '/');
    const auto colon = authority.rfind(':');
    const bool has_port = colon != std::string_view::npos && authority.find(']', colon) == std::string_view::npos;
    out.host = std::string(authority.substr(0, has_port ? colon : authority.size()));
    out.port = has_port ? std::string(authority.substr(colon + 1)) : std::string(out.secure ? "443" : "80");
    if (out.host.empty() || out.port.empty()) return std::nullopt;
    return out;
}

/**
 * One WebSocket connection, run by a coroutine that owns it until the connection ends: connect, TLS for `wss`, the
 * handshake, then a read loop handing each message to the handler as a view of the read buffer. Writes are queued,
 * one in flight at a time, as Beast requires.
 */
template <bool Secure>
class websocket_session : public std::enable_shared_from_this<websocket_session<Secure>> {
public:
    using tcp = boost::asio::ip::tcp;
    using next_layer = std::conditional_t<Secure, boost::asio::ssl::stream<boost::beast::tcp_stream>, boost::beast::tcp_stream>;
    using stream_type = boost::beast::websocket::stream<next_layer>;

    websocket_session(boost::asio::any_io_executor ex, boost::asio::ssl::context& tls, url_parts url, backend::websocket::handlers h)
        : ex_(ex), resolver_(ex), ws_(make_stream(ex, tls)), url_(std::move(url)), h_(std::move(h)) {}

    void start() {
        boost::asio::co_spawn(ex_, [self = this->shared_from_this()] { return self->run(); }, boost::asio::detached);
    }

    void send(std::string_view text) {
        if (!open_ || closing_) return;
        queue_.emplace_back(text);  // a write outlives the call, so the text is copied once, here
        if (std::exchange(writing_, true)) return;  // marked now: the writer only starts once the executor runs it
        boost::asio::co_spawn(ex_, [self = this->shared_from_this()] { return self->write_queue(); }, boost::asio::detached);
    }

    void close() {
        if (closing_ || ended_) return;
        closing_ = true;
        if (!open_) {
            boost::beast::get_lowest_layer(ws_).close();  // still connecting: the run ends with an error and reports
            return;
        }
        boost::asio::co_spawn(ex_, [self = this->shared_from_this()]() -> boost::asio::awaitable<void> {
            co_await self->ws_.async_close(boost::beast::websocket::close_code::normal, boost::asio::as_tuple(boost::asio::use_awaitable));
            boost::beast::get_lowest_layer(self->ws_).close();
        }, boost::asio::detached);
    }

    /** The owner let go: nothing is reported any more, and the connection is dropped. */
    void abandon() {
        h_ = {};
        closing_ = true;
        boost::beast::get_lowest_layer(ws_).close();
    }

private:
    static stream_type make_stream(boost::asio::any_io_executor ex, boost::asio::ssl::context& tls) {
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
        auto [resolve_error, endpoints] = co_await resolver_.async_resolve(url_.host, url_.port, as_tuple(use_awaitable));
        if (resolve_error || closing_) co_return end();
        auto& lowest = boost::beast::get_lowest_layer(ws_);
        lowest.expires_after(std::chrono::seconds(30));
        auto [connect_error, endpoint] = co_await lowest.async_connect(endpoints, as_tuple(use_awaitable));
        if (connect_error || closing_) co_return end();
        if constexpr (Secure) {
            auto& tls = ws_.next_layer();
            if (SSL_set_tlsext_host_name(tls.native_handle(), url_.host.c_str()) != 1) co_return end();  // SNI
            tls.set_verify_callback(boost::asio::ssl::host_name_verification(url_.host));
            auto [tls_error] = co_await tls.async_handshake(boost::asio::ssl::stream_base::client, as_tuple(use_awaitable));
            if (tls_error || closing_) co_return end();
        }
        lowest.expires_never();
        ws_.set_option(boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::client));
        const bool default_port = url_.port == (Secure ? "443" : "80");
        const std::string host = default_port ? url_.host : url_.host + ":" + url_.port;
        auto [handshake_error] = co_await ws_.async_handshake(host, url_.target, as_tuple(use_awaitable));
        if (handshake_error || closing_) co_return end();
        open_ = true;
        if (auto f = h_.open) f();
        boost::beast::flat_buffer buffer;
        while (!closing_) {
            auto [read_error, size] = co_await ws_.async_read(buffer, as_tuple(use_awaitable));
            if (read_error) break;
            if (auto f = h_.message) {
                const auto data = buffer.cdata();
                f(std::string_view(static_cast<const char*>(data.data()), size));
            }
            buffer.consume(buffer.size());
        }
        end();
    }

    boost::asio::awaitable<void> write_queue() {
        while (!queue_.empty() && open_ && !ended_) {
            ws_.text(true);
            auto [ec, size] = co_await ws_.async_write(boost::asio::buffer(queue_.front()), boost::asio::as_tuple(boost::asio::use_awaitable));
            queue_.pop_front();
            if (ec) break;
        }
        writing_ = false;
    }

    void end() {
        if (std::exchange(ended_, true)) return;
        open_ = false;
        queue_.clear();
        if (auto f = std::exchange(h_.close, {})) f();
    }

    boost::asio::any_io_executor ex_;
    tcp::resolver resolver_;
    stream_type ws_;
    url_parts url_;
    backend::websocket::handlers h_;
    std::deque<std::string> queue_;
    bool open_ = false;
    bool writing_ = false;
    bool closing_ = false;
    bool ended_ = false;
};

/** What the caller holds: dropping it abandons the session, which may still be finishing an operation. */
template <bool Secure>
class websocket_handle : public backend::websocket {
public:
    explicit websocket_handle(std::shared_ptr<websocket_session<Secure>> session) : session_(std::move(session)) {}
    ~websocket_handle() override { session_->abandon(); }
    void send(std::string_view text) override { session_->send(text); }
    void close() override { session_->close(); }

private:
    std::shared_ptr<websocket_session<Secure>> session_;
};

}  // namespace detail

/**
 * Connects to a Nostr relay over Boost.Beast: `ws://` in the clear, `wss://` over TLS with `tls`, which must outlive
 * the connection and should verify peers. A URL it cannot parse reports a close, as an unreachable relay does.
 */
inline std::shared_ptr<backend::websocket> connect_websocket(boost::asio::any_io_executor ex, boost::asio::ssl::context& tls, std::string_view url,
                                                            backend::websocket::handlers h) {
    auto parts = detail::parse_url(url);
    if (!parts) parts = detail::url_parts{false, "", "", ""};
    if (parts->secure) {
        auto session = std::make_shared<detail::websocket_session<true>>(ex, tls, std::move(*parts), std::move(h));
        session->start();
        return std::make_shared<detail::websocket_handle<true>>(std::move(session));
    }
    auto session = std::make_shared<detail::websocket_session<false>>(ex, tls, std::move(*parts), std::move(h));
    session->start();
    return std::make_shared<detail::websocket_handle<false>>(std::move(session));
}

}  // namespace webcpp::trystero::native

#endif
