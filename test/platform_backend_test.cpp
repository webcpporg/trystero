// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the platform backend itself, on both targets, natively and in a page under node:
//
// - Its own state, its copy of the bind address, its window of constructions and its own
//   webcpp::webrtc runtime, is in `backend_config::memory`, and the default resource sees none of
//   it; all of it is given back when the backend is destroyed. Before, the backend made its window
//   and its runtime in the default resource.
// - Its clocks are the standard library's: the wall clock's whole seconds, as the test reads them,
//   and the steady clock, which a 20 ms wait moves forward.
// - Its randomness, webcpp::crypto's random_device, fills what it is given, differently each time,
//   also beyond a piece of the platform's source (256 bytes of the system's entropy call).
// - Its PBKDF2 of a room's password is 0.27.0's master, derived off the room's thread, natively on
//   a thread of its own and in a page through WebCrypto, and completes on its executor after the
//   call returned; destroyed during a derivation, it waits for it.
// - Its executor is the one it was given, and its relays' TLS context was made.
// - It watches no connectivity natively, where no `online` or `offline` event exists, nor under
//   node, whose global object takes no listener: an app's automatic relays stay online
//   (platform_page_connectivity tests a page's events).
//
// main runs the io_context for the wait; a wait that does not come within 10 s fails by name. A
// derivation's run waits for its completion however long the loaded machine takes.

#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/protocol/room_keys.hpp>
#include <webcpp/trystero/protocol/text.hpp>

#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/steady_timer.hpp>

#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <webcpp/test/exit_status.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "bookkeeping.hpp"
#include "cases.hpp"

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using namespace std::chrono_literals;

namespace {

/** A resource that counts what it gives, and what is still out. */
class counting_resource final : public std::pmr::memory_resource {
public:
    /** How many allocations it made, a standard container's own (bookkeeping.hpp) left out: a
     * moved string's proxy, on MSVC's debug STL, is no state of the backend's. */
    std::size_t allocations = 0;
    /** How many of them are not yet returned, every one counted. */
    std::size_t outstanding = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (!webcpp::test::is_bookkeeping(bytes, alignment)) {
            ++allocations;
        }
        ++outstanding;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        --outstanding;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

/** The default resource, replaced by `with` for the life of the object. */
class default_resource {
public:
    explicit default_resource(std::pmr::memory_resource* with)
        : previous_(std::pmr::set_default_resource(with)) {}

    default_resource(const default_resource&) = delete;
    default_resource& operator=(const default_resource&) = delete;

    ~default_resource() { std::pmr::set_default_resource(previous_); }

private:
    std::pmr::memory_resource* previous_;
};

void its_own_state_is_in_its_memory() {
    asio::io_context io;
    counting_resource memory;
    counting_resource elsewhere;
    {
        // An address longer than a string holds in place, which the backend copies.
        std::pmr::string address("0000:0000:0000:0000:0000:0000:0000:0001", &elsewhere);
        const default_resource guard(&elsewhere);
        const std::size_t before = elsewhere.allocations;
        const trystero::platform::backend backend(
            io.get_executor(), {.bind_address = std::move(address), .memory = &memory});
        BOOST_TEST_EQ(elsewhere.allocations, before);
        // The address, the window and the runtime.
        BOOST_TEST_GE(memory.allocations, 3U);
    }
    BOOST_TEST_EQ(memory.outstanding, 0U);
}

void its_clocks_are_the_standard_librarys() {
    asio::io_context io;
    trystero::platform::backend backend(io.get_executor());
    const auto before = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const std::chrono::sys_seconds read = backend.unix_now();
    BOOST_TEST(before <= read);
    BOOST_TEST(read <= before + 1s);

    const auto start = backend.steady_now();
    asio::steady_timer wait(io, 20ms);
    std::optional<std::chrono::steady_clock::duration> waited;
    wait.async_wait([&](boost::system::error_code) { waited = backend.steady_now() - start; });
    io.run_for(10s);  // lint-run: the test's 20 ms wait
    if (BOOST_TEST(waited.has_value())) {
        BOOST_TEST(*waited >= 19ms);  // a whole millisecond of the steady clock's rounding
    }
}

void its_randomness_fills_and_differs() {
    asio::io_context io;
    trystero::platform::backend backend(io.get_executor());
    std::array<std::uint8_t, 64> first{};
    std::array<std::uint8_t, 64> second{};
    backend.random(first);
    backend.random(second);
    BOOST_TEST(first != second);
    BOOST_TEST(std::ranges::any_of(first, [](std::uint8_t b) { return b != 0; }));
}

// More than one piece of the platform's source, which a page fills at most 65536 bytes a call (and
// the C library's entropy call 256): every 256 bytes, the last few included, are filled, and
// differently each time. On the heap, beyond a page's stack.
void its_randomness_fills_more_than_a_piece() {
    asio::io_context io;
    trystero::platform::backend backend(io.get_executor());
    constexpr std::size_t size = (2 * 65536) + 300;
    constexpr std::size_t stride = 256;
    std::vector<std::uint8_t> first(size);
    std::vector<std::uint8_t> second(size);
    backend.random(first);
    backend.random(second);
    std::size_t unfilled = 0;
    std::size_t repeated = 0;
    for (std::size_t at = 0; at < size; at += stride) {
        const auto end = std::min(size, at + stride);
        const auto a = std::span(first).subspan(at, end - at);
        const auto b = std::span(second).subspan(at, end - at);
        unfilled += std::ranges::all_of(a, [](std::uint8_t byte) { return byte == 0; }) ? 1 : 0;
        repeated += std::ranges::equal(a, b) ? 1 : 0;
    }
    BOOST_TEST_EQ(unfilled, 0U);
    BOOST_TEST_EQ(repeated, 0U);
}

/** The bytes of a text, as a span. */
std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// The master of the password "hunter2" in the room "lobby" of "example.com/app", as 0.27.0
// derived it (fixtures/expected/trystero-0.27.0.json, room_keys_password), after the call
// returned, on the io_context's thread, and computed in no handler of the io_context: natively
// the one handler it runs is the completion, so the 600,000 iterations ran on the backend's own
// thread; in a page the browser's crypto.subtle computes them.
void its_pbkdf2_is_trysteros_and_computed_in_no_handler_of_the_io_context() {
    asio::io_context io;
    trystero::platform::backend backend(io.get_executor());
    const auto salt = trystero::protocol::pbkdf2_salt("example.com/app", "lobby");
    std::array<std::uint8_t, 32> master{};
    bool returned = false;
    std::optional<boost::system::error_code> done;
    std::thread::id ran_on;
    backend.derive_password_master(bytes_of("hunter2"), salt, master,
                                   [&](boost::system::error_code ec) {
                                       BOOST_TEST(returned);
                                       done = ec;
                                       ran_on = std::this_thread::get_id();
                                   });
    returned = true;
#ifndef WEBCPP_TARGET_EMSCRIPTEN
    BOOST_TEST_EQ(io.run_one(), 1U);  // lint-run: the completion, however long the work takes
    BOOST_TEST(done.has_value());
#endif
    io.run();  // lint-run: the derivation's completion, however long it takes
    if (BOOST_TEST(done.has_value())) {
        BOOST_TEST(!*done);
    }
    BOOST_TEST(ran_on == std::this_thread::get_id());
    BOOST_TEST_EQ(trystero::protocol::to_hex(master),
                  "0a9daa6bbfe5e7a974b8ab2030704abcd09c0451a3e189a4bf5cda230e8fa8a7");
}

// A backend destroyed while its thread derives, a second derivation queued behind the first,
// waits for both, whose completions then wait in the io_context, which runs them.
void its_destruction_waits_for_a_derivation() {
    asio::io_context io;
    const auto salt = trystero::protocol::pbkdf2_salt("example.com/app", "lobby");
    std::array<std::uint8_t, 32> first{};
    std::array<std::uint8_t, 32> second{};
    std::optional<boost::system::error_code> first_done;
    std::optional<boost::system::error_code> second_done;
    {
        trystero::platform::backend backend(io.get_executor());
        backend.derive_password_master(bytes_of("hunter2"), salt, first,
                                       [&](boost::system::error_code ec) { first_done = ec; });
        backend.derive_password_master(bytes_of("hunter2"), salt, second,
                                       [&](boost::system::error_code ec) { second_done = ec; });
    }
    io.run();  // lint-run: the derivations' completions
    for (const auto* done : {&first_done, &second_done}) {
        if (BOOST_TEST(done->has_value())) {
            BOOST_TEST(!**done);
        }
    }
    for (const auto* master : {&first, &second}) {
        BOOST_TEST_EQ(trystero::protocol::to_hex(*master),
                      "0a9daa6bbfe5e7a974b8ab2030704abcd09c0451a3e189a4bf5cda230e8fa8a7");
    }
}

void its_executor_and_tls_context_are_there() {
    asio::io_context io;
    trystero::platform::backend backend(io.get_executor());
    BOOST_TEST(backend.get_executor() == asio::any_io_executor(io.get_executor()));
    BOOST_TEST(backend.tls_context().has_value());
}

void it_watches_no_connectivity_without_a_page() {
    asio::io_context io;
    trystero::platform::backend backend(io.get_executor());
    bool called = false;
    const auto watch = backend.watch_connectivity([&called](bool) { called = true; },
                                                  std::pmr::get_default_resource());
    BOOST_TEST(watch == nullptr);
    io.run_for(20ms);  // lint-run: nothing is posted
    BOOST_TEST(!called);
}

}  // namespace

int main() {
    // Each case in a frame of its own, which main's never holds (cases.hpp).
    using webcpp::test::run_case;
    run_case(its_own_state_is_in_its_memory);
    run_case(its_clocks_are_the_standard_librarys);
    run_case(its_randomness_fills_and_differs);
    run_case(its_randomness_fills_more_than_a_piece);
    run_case(its_pbkdf2_is_trysteros_and_computed_in_no_handler_of_the_io_context);
    run_case(its_destruction_waits_for_a_derivation);
    run_case(its_executor_and_tls_context_are_there);
    run_case(it_watches_no_connectivity_without_a_page);
    return webcpp::test::exit_status(boost::report_errors());
}
