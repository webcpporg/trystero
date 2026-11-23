// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests what a relay's WebSocket allocates outside the room's resource in a page, under node, over
// a stand-in for the page's WebSocket: the global operator new, replaced here, and the C library's
// allocation functions, which Emscripten's dlmalloc defines weak and the test replaces, count
// exactly what the guide's "What stays outside the resource" lists, no more and no fewer, so that a
// change either way is seen:
//
// - a connection, from the call that makes it to its open: one C library allocation, embind's for
//   the type of the open event, which webcpp::beast's page stream reads as a `std::string`, and no
//   `operator new`, whatever the host's length: webcpp::asio's page resolver holds its results'
//   one entry, the host and the service, in place;
// - each message the relay sends, to its arrival in the room: two C library allocations, embind's
//   for the event's type and for the type of its data, which the stream reads as `std::string`s;
// - the close, from the call to the page's close event, delivered after the room was told: one C
//   library allocation, embind's for the event's type.
//
// The room's resource sees the session's own allocations in each. A first round, uncounted, makes
// the io_context's once-per-context allocations (the bridge, its map, the scheduler's first
// operation, the timer queue). Each embind allocation is freed before the call that made it
// returns. Two allocations of the platform's are kept out of the count, as allocation_test does:
// the test replaces select(), which Asio's reactor calls on every wait of the io_context and which
// Emscripten implements with an array it callocs on every call; and under Asyncify, where the run
// suspends while it waits for the page, Asyncify allocates the unwind data of the suspension with
// the C library's malloc, which the test marks and counts apart.
//
// main runs the io_context under a work guard, which the last round releases; each step starts from
// the one before. A step that does not come within 10 s fails by name.

#include <webcpp/trystero/platform/backend.hpp>

#include <webcpp/asio/bind_allocator.hpp>
#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/steady_timer.hpp>

#include <emscripten/em_asm.h>
#include <emscripten/em_js.h>
#include <emscripten/heap.h>
#include <poll.h>
#include <sys/select.h>
#include <boost/core/lightweight_test.hpp>
#include <boost/system/error_code.hpp>
#include <boost/throw_exception.hpp>
#include <webcpp/test/exit_status.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <string_view>
#include <vector>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using namespace std::chrono_literals;

// The stand-in: it opens on the next turn; relayEmit(n) has the socket of the last construction
// send n texts, each in a turn of its own; close() closes it on the next turn, once.
// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, install_relay, (), {
    globalThis.WebSocket = class {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;
        constructor(url) {
            globalThis.relaySocket = this;
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

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,modernize-use-trailing-return-type)
EM_JS(void, relay_emit, (int count), {
    const socket = globalThis.relaySocket;
    for (let i = 0; i < count; ++i) {
        setTimeout(() => {
            if (socket.onmessage) socket.onmessage({type: "message", data: '["EOSE","sub"]'});
        }, 0);
    }
});
// clang-format on

namespace {

// What the replaced operator new and the C library's allocation functions count while counting is
// set: global and mutable, since an operator new can reach nothing else.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::size_t global_allocations = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::size_t c_allocations = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
bool counting = false;
// Set, from JavaScript, while Asyncify allocates the unwind data of a call that suspends: that
// allocation is the platform's (webcpp::asio's page "Allocation"), counted apart. A char, which
// JavaScript writes through HEAPU8.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::uint8_t inside_runtime = 0;
// The C library's allocations made while inside_runtime is set.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::size_t runtime_allocations = 0;

void* counted(void* pointer) {
    if (counting) {
        ++(inside_runtime != 0 ? runtime_allocations : c_allocations);
    }
    return pointer;
}

}  // namespace

// The C library's allocation functions, replaced for this program alone, forwarding to
// Emscripten's own: the C library declares them with parameter names of its own, and cert-dcl58-cpp
// takes a replacement of what <cstdlib> declares for a change to namespace std.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name,cert-dcl58-cpp)
extern "C" {
void* malloc(std::size_t size) {
    return counted(emscripten_builtin_malloc(size));
}

void* calloc(std::size_t count, std::size_t size) {
    return counted(emscripten_builtin_calloc(count, size));
}

void* realloc(void* pointer, std::size_t size) {
    return counted(emscripten_builtin_realloc(pointer, size));
}

void free(void* pointer) {
    emscripten_builtin_free(pointer);
}

void* memalign(std::size_t alignment, std::size_t size) {
    return counted(emscripten_builtin_memalign(alignment, size));
}

void* aligned_alloc(std::size_t alignment, std::size_t size) {
    return counted(emscripten_builtin_memalign(alignment, size));
}

int posix_memalign(void** out, std::size_t alignment, std::size_t size) {
    *out = counted(emscripten_builtin_memalign(alignment, size));
    return *out == nullptr ? ENOMEM : 0;
}
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name,cert-dcl58-cpp)

// Emscripten's select() is a poll() over an array of pollfd it callocs on every call
// (system/lib/libc/musl/src/select/select.c), and Asio's reactor calls it on every wait of the
// io_context: the C library's wait, not the relay's. This one keeps the array on the stack, as
// webcpp::asio's allocation test does, so that the count sees the relay's allocations.
namespace {

/** The poll() event `event` when `set` holds `fd`, else none. */
std::uint32_t requested_event(int fd, const fd_set* set, std::uint32_t event) {
    return set != nullptr && FD_ISSET(fd, set) ? event : 0U;
}

void clear_set(fd_set* set) {
    if (set != nullptr) {
        FD_ZERO(set);
    }
}

/** Adds the descriptor to `set` when poll() reported one of `events` on it; 1 when it did. */
int ready_event(const pollfd& descriptor, fd_set* set, std::uint32_t events) {
    if (set != nullptr && (static_cast<std::uint32_t>(descriptor.revents) & events) != 0) {
        FD_SET(descriptor.fd, set);
        return 1;
    }
    return 0;
}

}  // namespace

// select's C declaration requires a mutable timeval pointer, although this implementation
// does not change it.
extern "C" int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                      timeval* timeout) {  // NOLINT(misc-const-correctness)
    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    std::array<pollfd, FD_SETSIZE> fds{};
    std::size_t n = 0;
    for (int fd = 0; fd < nfds; ++fd) {
        const std::uint32_t events = requested_event(fd, readfds, POLLIN) |
                                     requested_event(fd, writefds, POLLOUT) |
                                     requested_event(fd, exceptfds, POLLPRI);
        if (events != 0) {
            fds[n++] = pollfd{.fd = fd, .events = static_cast<short>(events), .revents = 0};
        }
    }
    const int milliseconds =
        timeout == nullptr ? -1
                           : static_cast<int>((timeout->tv_sec * 1000) + (timeout->tv_usec / 1000));
    const int ready = poll(fds.data(), static_cast<nfds_t>(n), milliseconds);
    if (ready < 0) {
        return ready;
    }
    clear_set(readfds);
    clear_set(writefds);
    clear_set(exceptfds);
    int count = 0;
    for (std::size_t i = 0; i < n && ready > 0; ++i) {
        count += ready_event(fds[i], readfds, POLLIN | POLLHUP | POLLERR);
        count += ready_event(fds[i], writefds, POLLOUT | POLLERR);
        count += ready_event(fds[i], exceptfds, POLLPRI);
    }
    return count;
}

namespace {

/** What the replaced operator new does: counts, then takes the memory from the C library, uncounted
 * there. */
void* allocate(std::size_t size, std::size_t alignment) {
    if (counting) {
        ++global_allocations;
    }
    alignment = alignment < sizeof(void*) ? sizeof(void*) : alignment;
    void* pointer = emscripten_builtin_memalign(alignment, size == 0 ? alignment : size);
    if (pointer == nullptr) {
        boost::throw_exception(std::bad_alloc());
    }
    return pointer;
}

}  // namespace

// The standard library declares the replaceable allocation functions with parameter names reserved
// to the implementation, which a program may not use, hence the exemption on each.

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size) {
    return allocate(size, alignof(std::max_align_t));
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<std::size_t>(alignment));
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer) noexcept {
    emscripten_builtin_free(pointer);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer, std::size_t /*size*/) noexcept {
    emscripten_builtin_free(pointer);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer, std::align_val_t /*alignment*/) noexcept {
    emscripten_builtin_free(pointer);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    emscripten_builtin_free(pointer);
}

namespace {

/**
 * The room's resource: a buffer made before anything is counted, which counts what it gives.
 */
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations = 0;

private:
    std::vector<std::byte> storage_ = std::vector<std::byte>(std::size_t{1} << 20U);
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

/** What one counted phase saw, from its start to its end. */
struct counts {
    bool completed = false;
    std::size_t global = 0;
    std::size_t c_library = 0;
    std::size_t room = 0;
};

/** What one connection's phases saw. */
struct connection_counts {
    counts open;
    counts one_message;
    counts ten_messages;
    counts close;
};

/**
 * The relays of the rounds: the first warms up, uncounted; the third's host is longer than a short
 * string holds (10 bytes in a page), which the resolver's results hold in place too.
 */
constexpr std::array<std::string_view, 3> relays{
    "wss://relay.test",
    "wss://relay.test",
    "wss://relay.example.org",
};

enum class phase : std::uint8_t { open, one_message, ten_messages, close };

struct state {
    counting_resource room;
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    trystero::platform::backend backend{io.get_executor()};
    asio::steady_timer settle{io};
    std::shared_ptr<trystero::backend::websocket> socket;
    std::size_t round = 0;
    phase at = phase::open;
    std::size_t messages = 0;
    std::size_t expected = 0;
    std::array<connection_counts, relays.size()> rounds;
    bool done = false;
};

void start_counting(const state& s, counts& into) {
    global_allocations = 0;
    c_allocations = 0;
    into.room = s.room.allocations;
    counting = true;
}

void stop_counting(const state& s, counts& into) {
    counting = false;
    into.completed = true;
    into.global = global_allocations;
    into.c_library = c_allocations;
    into.room = s.room.allocations - into.room;
}

/** Has the relay send `count` texts, counting into `into` until the last reaches the room. */
void emit(state& s, std::size_t count, counts& into) {
    s.messages = 0;
    s.expected = count;
    start_counting(s, into);
    relay_emit(static_cast<int>(count));
}

void on_open(state& s) {
    auto& counted = s.rounds.at(s.round);
    stop_counting(s, counted.open);
    s.at = phase::one_message;
    emit(s, 1, counted.one_message);
}

void on_message(state& s) {
    if (++s.messages != s.expected) {
        return;
    }
    auto& counted = s.rounds.at(s.round);
    if (s.at == phase::one_message) {
        stop_counting(s, counted.one_message);
        s.at = phase::ten_messages;
        emit(s, 10, counted.ten_messages);
        return;
    }
    stop_counting(s, counted.ten_messages);
    s.at = phase::close;
    start_counting(s, counted.close);
    s.socket->close();
}

void connect(state& s);

/**
 * The close is reported: the count goes on until the page's close event, which comes after it, has
 * been delivered, behind a wait of the room's.
 */
void on_close(state& s) {
    s.settle.expires_after(20ms);
    s.settle.async_wait(asio::bind_allocator(std::pmr::polymorphic_allocator<>(&s.room),
                                             [&s](boost::system::error_code /*ec*/) {
                                                 stop_counting(s, s.rounds.at(s.round).close);
                                                 s.socket.reset();
                                                 if (++s.round == relays.size()) {
                                                     s.done = true;
                                                     s.guard.reset();
                                                     return;
                                                 }
                                                 connect(s);
                                             }));
}

/** Connects to the round's relay, counting from the call to the open. */
void connect(state& s) {
    s.at = phase::open;
    start_counting(s, s.rounds.at(s.round).open);
    s.socket = s.backend.connect(relays.at(s.round),
                                 {
                                     .open = [&s] { on_open(s); },
                                     .message = [&s](std::string_view /*text*/) { on_message(s); },
                                     .close = [&s] { on_close(s); },
                                 },
                                 &s.room);
}

/**
 * Marks Asyncify's allocation of a suspension's unwind data, on emscripten under Asyncify, as
 * allocation_test does: the C library's allocations made during it are counted apart.
 */
void mark_the_runtime_allocations() {
    // clang-format off
    EM_ASM({
        if (typeof Asyncify !== "object" || typeof Asyncify.allocateData !== "function") {
            return;
        }
        const flag = $0;
        const allocate = Asyncify.allocateData;
        Asyncify.allocateData = function(...values) {
            const previous = HEAPU8[flag];
            HEAPU8[flag] = 1;
            try {
                return allocate.apply(this, values);
            } finally {
                HEAPU8[flag] = previous;
            }
        };
    }, &inside_runtime);
    // clang-format on
}

/** Checks what one phase allocated outside the room's resource, and that the room saw some. */
void expect(const counts& got, std::size_t global, std::size_t c_library, const char* name) {
    if (!got.completed) {
        BOOST_ERROR(name);
        return;
    }
    BOOST_TEST_EQ(got.global, global);
    BOOST_TEST_EQ(got.c_library, c_library);
    BOOST_TEST_GT(got.room, 0U);
}

/** Checks a counted round. */
void expect_round(const connection_counts& got) {
    expect(got.open, 0, 1, "a connection did not open");
    expect(got.one_message, 0, 2, "a message did not reach the room");
    expect(got.ten_messages, 0, 20, "ten messages did not reach the room");
    expect(got.close, 0, 1, "a connection did not close");
}

}  // namespace

int main() {
    mark_the_runtime_allocations();
    install_relay();
    state s;
    connect(s);
    s.io.run_for(10s);  // lint-run: the test's event loop, held by its work guard until the close
    counting = false;
    if (!s.done) {
        BOOST_ERROR("the rounds stopped before the last");
    }
    expect_round(s.rounds.at(1));  // relay.test, a short string
    expect_round(s.rounds.at(2));  // relay.example.org, longer
    return webcpp::test::exit_status(boost::report_errors());
}
