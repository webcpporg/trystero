// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests that natively a relay's message allocates nothing outside the room's resource: from the
// send of a text to the receipt of its echo, ten times over, the global operator new, replaced
// here, counts none on the room's thread, while the room's resource sees the session's texts and
// operations. The relay is an echo server of Boost.Beast's own, on a thread of its own, which the
// count leaves out; the first three echoes, uncounted, make what a connection makes once (the
// WebSocket's write buffer, Asio's per-thread cache of the operations it recycles).
//
// Before the fix the session's reads and writes completed into Asio's type-erased handler,
// `any_completion_handler`, whose executor is of another type than the stream's: for each
// operation Asio made an `any_completion_executor` around the stream's executor, with `new`, and
// dispatched each completion through it in a function object of its own. Measured: six
// `operator new` for a text sent and its echo received, and four of Asio's recycled blocks.
//
// A relay that refuses the upgrade with a body of 4 MiB has the handshake read its header alone:
// from the call that makes the socket to its close, reported without an open, operator new gives
// the room's thread less than 64 KiB (measured: 5,144 bytes, the connection's own state among
// them). Before the session's next layer was webcpp::beast's bounded handshake layer, Beast's own
// parser read the body whole, up to its default limit for a response, 8 MiB, outside the room's
// resource (measured: 4,199,353 bytes).
//
// main runs an io_context for each case: in the first each echo sends the next text, and the last
// closes the socket; in the second the refusal ends the connection.

#include <webcpp/trystero/platform/websocket.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <boost/throw_exception.hpp>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace trystero = webcpp::trystero;
using asio::ip::tcp;

namespace {

// What the replaced operator new counts while counting is set, on the thread that set it: global
// and mutable, since an operator new can reach nothing else.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local bool counting = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local std::size_t global_allocations = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local std::size_t global_bytes = 0;

/** What the replaced operator new does: counts, then takes the memory from the C library. */
void* allocate(std::size_t size) {
    if (counting) {
        ++global_allocations;
        global_bytes += size;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc): the global operator new's own storage
    void* pointer = std::malloc(size == 0 ? 1 : size);
    if (pointer == nullptr) {
        boost::throw_exception(std::bad_alloc());
    }
    return pointer;
}

}  // namespace

// The standard library declares the replaceable allocation functions with parameter names reserved
// to the implementation, which a program may not use, hence the exemption on each. Asio's
// type-erased executors take their storage with the nothrow form.

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size) {
    return allocate(size);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    if (counting) {
        ++global_allocations;
        global_bytes += size;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc): the global operator new's own storage
    return std::malloc(size == 0 ? 1 : size);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer) noexcept {
    std::free(pointer);  // NOLINT(cppcoreguidelines-no-malloc): what operator new took
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer, std::size_t /*size*/) noexcept {
    std::free(pointer);  // NOLINT(cppcoreguidelines-no-malloc): what operator new took
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer, const std::nothrow_t& /*tag*/) noexcept {
    std::free(pointer);  // NOLINT(cppcoreguidelines-no-malloc): what operator new took
}

namespace {

/** The room's resource: a buffer made before anything is counted, which counts what it gives. */
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations = 0;

private:
    std::vector<std::byte> storage_ = std::vector<std::byte>(std::size_t{4} << 20U);
    std::pmr::monotonic_buffer_resource buffer_{storage_.data(), storage_.size(),
                                                std::pmr::null_memory_resource()};

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return buffer_.allocate(bytes, alignment);
    }

    void do_deallocate(void* /*pointer*/, std::size_t /*bytes*/,
                       std::size_t /*alignment*/) override {}

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

/**
 * A relay that sends every text back, on a thread of its own, with Boost.Beast's synchronous
 * stream: one connection, until it closes.
 */
class echo_relay {
public:
    echo_relay() : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}) {
        thread_ = std::thread([this] { serve(); });
    }

    echo_relay(const echo_relay&) = delete;
    echo_relay& operator=(const echo_relay&) = delete;
    echo_relay(echo_relay&&) = delete;
    echo_relay& operator=(echo_relay&&) = delete;

    ~echo_relay() { thread_.join(); }

    [[nodiscard]] std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port()) + "/";
    }

private:
    void serve() {
        boost::system::error_code ec;
        tcp::socket socket(io_);
        if (acceptor_.accept(socket, ec)) {
            return;
        }
        beast::websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept(ec);
        beast::flat_buffer buffer;
        while (!ec) {
            ws.read(buffer, ec);
            if (ec) {
                return;
            }
            ws.text(true);
            ws.write(buffer.data(), ec);
            buffer.consume(buffer.size());
        }
    }

    asio::io_context io_;
    tcp::acceptor acceptor_;
    std::thread thread_;
};

/**
 * A relay that refuses the upgrade, on a thread of its own: it reads the request's header and
 * answers 403 with a body of `body` bytes, then closes.
 */
class refusing_relay {
public:
    explicit refusing_relay(std::size_t body)
        : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}), body_(body) {
        thread_ = std::thread([this] { serve(); });
    }

    refusing_relay(const refusing_relay&) = delete;
    refusing_relay& operator=(const refusing_relay&) = delete;
    refusing_relay(refusing_relay&&) = delete;
    refusing_relay& operator=(refusing_relay&&) = delete;

    ~refusing_relay() { thread_.join(); }

    [[nodiscard]] std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port()) + "/";
    }

private:
    void serve() {
        boost::system::error_code ec;
        tcp::socket socket(io_);
        if (acceptor_.accept(socket, ec)) {
            return;
        }
        std::string request;
        asio::read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", ec);
        if (ec) {
            return;
        }
        const std::string response =
            "HTTP/1.1 403 Forbidden\r\nContent-Length: " + std::to_string(body_) + "\r\n\r\n" +
            std::string(body_, 'r');
        // The client may close once it has read what it reads: the write then fails, as it may,
        // and the socket is closed all the same.
        asio::write(socket, asio::buffer(response), ec);
        if (socket.close(ec)) {
            return;
        }
    }

    asio::io_context io_;
    tcp::acceptor acceptor_;
    std::size_t body_;
    std::thread thread_;
};

constexpr std::size_t warm_up = 3;
constexpr std::size_t counted = 10;

struct state {
    asio::io_context io;
    counting_resource room;
    asio::ssl::context tls{asio::ssl::context::tls_client};
    std::shared_ptr<trystero::backend::websocket> socket;
    const std::string text = std::string(200, 't');
    std::size_t echoes = 0;
    std::size_t room_before = 0;
    std::size_t room_counted = 0;
    std::size_t global_counted = 0;
    bool done = false;
};

/** Sends the next text, counting from the first counted one; the last echo closes the socket. */
void on_echo(state& s) {
    ++s.echoes;
    if (s.echoes == warm_up) {
        global_allocations = 0;
        s.room_before = s.room.allocations;
        counting = true;
    }
    if (s.echoes == warm_up + counted) {
        counting = false;
        s.global_counted = global_allocations;
        s.room_counted = s.room.allocations - s.room_before;
        s.done = true;
        s.socket->close();
        return;
    }
    s.socket->send(s.text);
}

/** A text sent and its echo received, ten times, allocate nothing outside the room's resource. */
void messages_allocate_nothing_outside_the_room() {
    const echo_relay relay;
    state s;
    s.socket = trystero::platform::connect_websocket(
        s.io.get_executor(), &s.tls, relay.url(),
        {
            .open = [&s] { s.socket->send(s.text); },
            .message = [&s](std::string_view /*text*/) { on_echo(s); },
            .close = {},
        },
        &s.room);
    s.io.run();  // lint-run: the session and its relay do loopback network I/O, until the close
    counting = false;
    if (!BOOST_TEST(s.done)) {
        return;
    }
    BOOST_TEST_EQ(s.global_counted, 0U);
    // The session's texts and operations, at least one of each a message.
    BOOST_TEST_GE(s.room_counted, counted);
    s.socket.reset();
}

/**
 * A relay that refuses the upgrade with a body of 4 MiB: the connection reports its close without
 * opening, and the room's thread allocated less than 64 KiB with operator new meanwhile, its
 * resolver's results and its streams' state among them, the refusal's body none of it.
 */
void a_refusals_body_is_not_read() {
    constexpr std::size_t body = std::size_t{4} << 20U;
    const refusing_relay relay(body);
    asio::io_context io;
    counting_resource room;
    asio::ssl::context tls{asio::ssl::context::tls_client};
    std::size_t opens = 0;
    std::size_t closes = 0;
    global_bytes = 0;
    counting = true;
    auto socket = trystero::platform::connect_websocket(io.get_executor(), &tls, relay.url(),
                                                        {
                                                            .open = [&opens] { ++opens; },
                                                            .message = {},
                                                            .close = [&closes] { ++closes; },
                                                        },
                                                        &room);
    io.run();  // lint-run: the session and its relay do loopback network I/O, until the refusal
    counting = false;
    BOOST_TEST_EQ(opens, 0U);
    BOOST_TEST_EQ(closes, 1U);
    BOOST_TEST_LT(global_bytes, std::size_t{64} << 10U);
    socket.reset();
}

}  // namespace

int main() {
    messages_allocate_nothing_outside_the_room();
    a_refusals_body_is_not_read();
    return boost::report_errors();
}
