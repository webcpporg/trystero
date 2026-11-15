// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests that a page's sealer allocates nothing outside the room's resource while it seals and
// opens, under node: from the start of a seal to its completion, and from the start of an open to
// its, the global operator new, replaced here, and the C library's allocation functions, which
// Emscripten's dlmalloc defines weak and the test replaces, count none, while the room's resource
// sees each Web Crypto wait and its completion. A first seal and open come before, uncounted: they
// import the key and make the io_context's once-per-context allocations (the bridge, its map, the
// scheduler's first operation). Before the fix the sealer awaited each Web Crypto promise through
// its type-erased executor, `webcpp::asio::any_io_executor`, through which Asio posts the
// settlement in a function object and an operation of `std::allocator`, which Asio takes from the
// C library when its per-run cache has none (measured: two for a seal, two for an open).
//
// Two allocations of the platform's are kept out of the count, as allocation_test does: the test
// replaces select(), which Asio's reactor calls on every wait of the io_context and which
// Emscripten implements with an array it callocs on every call; and under Asyncify, where the run
// suspends while a promise is pending and Asyncify allocates the unwind data of the suspension with
// the C library's malloc, the test marks that allocation and counts it apart.
//
// main runs the io_context; each step starts from the completion of the one before, and the run
// ends when the last completes. A step that does not come within 10 s fails by name.

#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/platform/sealer.hpp>
#include <webcpp/trystero/protocol/signal.hpp>

#include <webcpp/asio/io_context.hpp>

#include <emscripten/em_asm.h>
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
// io_context, once in each seal and each open (measured): the C library's wait, not the sealer's.
// This one keeps the array on the stack, as webcpp::asio's allocation test does, so that the count
// sees the sealer's allocations.
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

/** What one counted operation saw, from its start to its completion. */
struct counts {
    bool completed = false;
    boost::system::error_code ec;
    std::size_t global = 0;
    std::size_t c_library = 0;
    std::size_t room = 0;
};

/** The plaintext sealed and opened: a candidate's JSON, as a room seals it. */
constexpr std::string_view plaintext =
    R"({"candidate":"candidate:1 1 udp 2122260223 192.0.2.1 54321 typ host"})";

struct state {
    counting_resource room;
    asio::io_context io;
    std::shared_ptr<trystero::platform::sealer> sealer =
        std::allocate_shared<trystero::platform::sealer>(
            std::pmr::polymorphic_allocator<>(&room), io.get_executor(),
            trystero::protocol::seal_key("secret", "my-app", "room-1"), &room);
    std::pmr::string sealed{&room};
    std::pmr::string opened{&room};
    bool warmed_up = false;
    counts seal;
    counts open;
};

using done_handler = trystero::platform::sealer::done_handler;

/** A completion of the sealer's, allocated in the room's resource. */
template <class F>
done_handler completion(state& s, F f) {
    return {std::allocator_arg, std::pmr::polymorphic_allocator<>(&s.room), f};
}

/** Starts counting what `into` will report, from zero. */
void start_counting(const state& s, counts& into) {
    global_allocations = 0;
    c_allocations = 0;
    into.room = s.room.allocations;
    counting = true;
}

/** Stops counting, and records what was counted since start_counting into `into`. */
void stop_counting(const state& s, counts& into, boost::system::error_code ec) {
    counting = false;
    into.completed = true;
    into.ec = ec;
    into.global = global_allocations;
    into.c_library = c_allocations;
    into.room = s.room.allocations - into.room;
}

/** Opens the counted seal's text, counting from the start of the open to its completion. */
void counted_open(state& s) {
    start_counting(s, s.open);
    s.sealer->open(s.sealed,
                   completion(s, [&s](boost::system::error_code ec, std::string_view text) {
                       stop_counting(s, s.open, ec);
                       s.opened.assign(text);
                   }));
}

/** Seals, counting from the start of the seal to its completion. */
void counted_seal(state& s) {
    start_counting(s, s.seal);
    s.sealer->seal(plaintext,
                   completion(s, [&s](boost::system::error_code ec, std::string_view text) {
                       stop_counting(s, s.seal, ec);
                       s.sealed.assign(text);
                       counted_open(s);
                   }));
}

/** A first seal and open, uncounted, which import the key and make what an io_context makes once.
 */
void warm_up(state& s) {
    s.sealer->seal(
        plaintext, completion(s, [&s](boost::system::error_code ec, std::string_view text) {
            BOOST_TEST(!ec);
            s.sealed.assign(text);
            s.sealer->open(s.sealed,
                           completion(s, [&s](boost::system::error_code ec, std::string_view text) {
                               BOOST_TEST(!ec);
                               BOOST_TEST_EQ(text, plaintext);
                               s.warmed_up = true;
                               counted_seal(s);
                           }));
        }));
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

/** Checks that one counted operation completed with no allocation outside the room's resource. */
void expect_none_outside(const counts& got, const char* name) {
    if (!got.completed) {
        BOOST_ERROR(name);
        return;
    }
    BOOST_TEST(!got.ec);
    BOOST_TEST_EQ(got.global, 0U);
    BOOST_TEST_EQ(got.c_library, 0U);
    // The Web Crypto wait, at least, is the room's.
    BOOST_TEST_GT(got.room, 0U);
}

}  // namespace

int main() {
    mark_the_runtime_allocations();
    state s;
    warm_up(s);
    s.io.run_for(10s);  // lint-run: the test's event loop, held by the sealer's Web Crypto waits
    counting = false;
    if (!s.warmed_up) {
        BOOST_ERROR("the first seal and open did not complete");
    }
    expect_none_outside(s.seal, "the counted seal did not complete");
    expect_none_outside(s.open, "the counted open did not complete");
    BOOST_TEST_EQ(s.opened, plaintext);
    return webcpp::test::exit_status(boost::report_errors());
}
