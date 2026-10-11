// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// While two rooms over the memory relays exchange a message, every allocation
// they make goes through the resources of their configurations: the counting
// resources see them, and the global operator new, replaced here, sees none,
// nor do the C library's allocation functions where the platform lets a
// program count them (doc: #trystero-allocation).

#include <webcpp/trystero.hpp>

#include "support/memory_backend.hpp"

#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/core/lightweight_test.hpp>
#include <boost/json/storage_ptr.hpp>
#include <boost/json/value.hpp>
#include <boost/throw_exception.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <new>
#include <string_view>
#include <vector>
#ifdef _MSC_VER
#include <malloc.h>
#else
#include <pthread.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#include <poll.h>
#include <sys/select.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif

namespace trystero = webcpp::trystero;

namespace {

// What the replaced operator new and the C library's allocation functions count
// while counting is set: global and mutable, since an operator new can reach
// nothing else.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::size_t global_allocations = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::size_t c_allocations = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
bool counting = false;
// Set while operator new takes its memory from the C library, which then
// counts nothing: that allocation is operator new's, counted once.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
bool inside_operator_new = false;

#ifdef _MSC_VER
/** Whether the calling thread is the one the test counts on: MSVC counts every thread. */
bool on_counted_thread() noexcept {
    return true;
}

void count_this_thread() noexcept {}
#else
// The thread main runs on: the C library's allocations are reported from every thread
// (malloc_logger is process-wide, the replaced functions serve every thread), and a
// system thread allocating during a window is not the rooms', nor may it touch the
// counts. Atomic, since every thread reads it.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<pthread_t> counted_thread{};

/** Whether the calling thread is the one the test counts on. */
bool on_counted_thread() noexcept {
    return pthread_equal(pthread_self(), counted_thread.load(std::memory_order_relaxed)) != 0;
}

void count_this_thread() noexcept {
    counted_thread.store(pthread_self(), std::memory_order_relaxed);
}
#endif

}  // namespace

// AddressSanitizer replaces the C library's allocation functions with its own
// allocator, which none of the hooks below sees, so a build with it counts none.
#ifdef __SANITIZE_ADDRESS__
#define WEBCPP_TEST_TRYSTERO_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WEBCPP_TEST_TRYSTERO_ADDRESS_SANITIZER 1
#endif
#endif

// The C library's allocations, seen where a program can see them: glibc's
// functions are replaced and forward to glibc's own (__libc_*), Emscripten's
// dlmalloc defines its functions weak and keeps emscripten_builtin_*, and
// macOS reports every allocation to malloc_logger. MSVC's are not counted, nor
// any under AddressSanitizer.
#ifdef WEBCPP_TEST_TRYSTERO_ADDRESS_SANITIZER
// Nothing of the C library is counted.
#elif defined(__GLIBC__) || defined(__EMSCRIPTEN__)
#define WEBCPP_TEST_TRYSTERO_COUNTS_C_ALLOCATIONS 1
#ifdef __GLIBC__
extern "C" {
void* __libc_malloc(std::size_t size);
void* __libc_calloc(std::size_t count, std::size_t size);
void* __libc_realloc(void* pointer, std::size_t size);
void* __libc_memalign(std::size_t alignment, std::size_t size);
void __libc_free(void* pointer);
}
#define WEBCPP_TEST_C_MALLOC __libc_malloc
#define WEBCPP_TEST_C_CALLOC __libc_calloc
#define WEBCPP_TEST_C_REALLOC __libc_realloc
#define WEBCPP_TEST_C_MEMALIGN __libc_memalign
#define WEBCPP_TEST_C_FREE __libc_free
#else
#define WEBCPP_TEST_C_MALLOC emscripten_builtin_malloc
#define WEBCPP_TEST_C_CALLOC emscripten_builtin_calloc
#define WEBCPP_TEST_C_REALLOC emscripten_builtin_realloc
#define WEBCPP_TEST_C_MEMALIGN emscripten_builtin_memalign
#define WEBCPP_TEST_C_FREE emscripten_builtin_free
#endif

namespace {

void* counted(void* pointer) {
    if (on_counted_thread() && counting && !inside_operator_new) {
        ++c_allocations;
    }
    return pointer;
}

}  // namespace

// The C library's allocation functions, replaced for this program alone: the C library declares
// them with parameter names of its own, and cert-dcl58-cpp takes a replacement of what <cstdlib>
// declares for a change to namespace std.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name,cert-dcl58-cpp)
extern "C" {
void* malloc(std::size_t size) {
    return counted(WEBCPP_TEST_C_MALLOC(size));
}

void* calloc(std::size_t count, std::size_t size) {
    return counted(WEBCPP_TEST_C_CALLOC(count, size));
}

void* realloc(void* pointer, std::size_t size) {
    return counted(WEBCPP_TEST_C_REALLOC(pointer, size));
}

void free(void* pointer) {
    WEBCPP_TEST_C_FREE(pointer);
}

void* memalign(std::size_t alignment, std::size_t size) {
    return counted(WEBCPP_TEST_C_MEMALIGN(alignment, size));
}

void* aligned_alloc(std::size_t alignment, std::size_t size) {
    return counted(WEBCPP_TEST_C_MEMALIGN(alignment, size));
}

int posix_memalign(void** out, std::size_t alignment, std::size_t size) {
    *out = counted(WEBCPP_TEST_C_MEMALIGN(alignment, size));
    return *out == nullptr ? ENOMEM : 0;
}
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name,cert-dcl58-cpp)

#ifdef __EMSCRIPTEN__
// Emscripten's select() is a poll() over an array of pollfd it callocs on every
// call (system/lib/libc/musl/src/select/select.c), and Asio's reactor calls it on
// every wait of the io_context the test drives: measured, thousands of calls
// while two rooms exchange one payload. This one keeps the array on the stack, so
// that the count sees the rooms' allocations rather than the C library's wait.
extern "C" int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                      timeval* timeout) {
    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    std::array<pollfd, FD_SETSIZE> fds{};
    int n = 0;
    for (int fd = 0; fd < nfds; ++fd) {
        short events = 0;
        events |= readfds != nullptr && FD_ISSET(fd, readfds) ? POLLIN : 0;
        events |= writefds != nullptr && FD_ISSET(fd, writefds) ? POLLOUT : 0;
        events |= exceptfds != nullptr && FD_ISSET(fd, exceptfds) ? POLLPRI : 0;
        if (events != 0) {
            fds[n++] = pollfd{fd, events, 0};
        }
    }
    const int milliseconds =
        timeout == nullptr ? -1
                           : static_cast<int>(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
    const int ready = poll(fds.data(), static_cast<nfds_t>(n), milliseconds);
    if (ready < 0) {
        return ready;
    }
    if (readfds != nullptr) {
        FD_ZERO(readfds);
    }
    if (writefds != nullptr) {
        FD_ZERO(writefds);
    }
    if (exceptfds != nullptr) {
        FD_ZERO(exceptfds);
    }
    int count = 0;
    for (int i = 0; i < n && ready > 0; ++i) {
        const short got = fds[i].revents;
        if (readfds != nullptr && (got & (POLLIN | POLLHUP | POLLERR)) != 0) {
            FD_SET(fds[i].fd, readfds);
            ++count;
        }
        if (writefds != nullptr && (got & (POLLOUT | POLLERR)) != 0) {
            FD_SET(fds[i].fd, writefds);
            ++count;
        }
        if (exceptfds != nullptr && (got & POLLPRI) != 0) {
            FD_SET(fds[i].fd, exceptfds);
            ++count;
        }
    }
    return count;
}
#endif

#elif defined(__APPLE__)
#define WEBCPP_TEST_TRYSTERO_COUNTS_C_ALLOCATIONS 1

// libsystem_malloc's hook, which it calls on every allocation and release: a global of its own,
// which the test sets.
extern "C" {
using malloc_logger_type = void(std::uint32_t type, std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                std::uintptr_t, std::uint32_t);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern malloc_logger_type* malloc_logger;
}

namespace {

constexpr std::uint32_t malloc_log_allocate = 2;

void count_c_allocation(std::uint32_t type, std::uintptr_t /*zone*/, std::uintptr_t /*size*/,
                        std::uintptr_t /*other*/, std::uintptr_t /*result*/,
                        std::uint32_t /*frames*/) {
    if (on_counted_thread() && counting && !inside_operator_new &&
        (type & malloc_log_allocate) != 0) {
        ++c_allocations;
    }
}

}  // namespace
#endif

namespace {

/** Memory for operator new, from the C library, uncounted there. */
void* take(std::size_t size, std::size_t alignment) {
#ifdef _MSC_VER
    return _aligned_malloc(size, alignment);
#elif defined(WEBCPP_TEST_C_MEMALIGN)
    return WEBCPP_TEST_C_MEMALIGN(alignment, size);
#else
    inside_operator_new = true;
    void* pointer = std::aligned_alloc(alignment, size);
    inside_operator_new = false;
    return pointer;
#endif
}

void release(void* pointer) noexcept {
#ifdef _MSC_VER
    _aligned_free(pointer);
#elif defined(WEBCPP_TEST_C_FREE)
    WEBCPP_TEST_C_FREE(pointer);
#else
    inside_operator_new = true;
    std::free(pointer);
    inside_operator_new = false;
#endif
}

/**
 * What the replaced operator new does: counts, then takes the memory.
 *
 * Tip: std::aligned_alloc refuses an alignment below sizeof(void*) (macOS 1, 2
 * and 4, Emscripten 1 and 2) and a size that is not a multiple of the
 * alignment, and libstdc++'s new_delete_resource always asks for the aligned
 * form, so the alignment is raised and the size rounded up.
 */
void* allocate(std::size_t size, std::size_t alignment) {
    if (on_counted_thread() && counting) {
        ++global_allocations;
    }
    alignment = alignment < sizeof(void*) ? sizeof(void*) : alignment;
    const std::size_t rounded =
        size == 0 ? alignment : (size + alignment - 1) / alignment * alignment;
    void* pointer = take(rounded, alignment);
    if (pointer == nullptr) {
        boost::throw_exception(std::bad_alloc());
    }
    return pointer;
}

/**
 * A resource that counts what it gives, from a buffer made before the exchange, and what is
 * given back: how many bytes are still out, and how many returns name memory it never gave,
 * which a pmr object freed through the wrong resource would do.
 */
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations = 0;
    std::size_t outstanding = 0;
    std::size_t foreign = 0;

private:
    std::vector<std::byte> storage_ = std::vector<std::byte>(std::size_t{64} << 20U);
    std::pmr::monotonic_buffer_resource buffer_{storage_.data(), storage_.size(),
                                                std::pmr::null_memory_resource()};

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        outstanding += bytes;
        return buffer_.allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t /*alignment*/) override {
        const auto* at = static_cast<const std::byte*>(pointer);
        if (at < storage_.data() || at >= storage_.data() + storage_.size()) {
            ++foreign;
            return;
        }
        outstanding -= bytes;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

/** Runs io in slices of 10 ms until done holds, for 5 s at most. */
template <class Predicate>
bool run_until(boost::asio::io_context& io, Predicate done) {
    for (int slice = 0; slice < 500 && !done(); ++slice) {
        io.restart();
        io.run_for(std::chrono::milliseconds(10));  // lint-run: the memory relays post to it
    }
    return done();
}

/**
 * The replaced operators and what they stand on, before anything is counted:
 * every alignment from 1 to 64 through both forms of operator new and through
 * new_delete_resource, which libstdc++ always asks in the aligned form.
 */
void the_replaced_operators_serve_every_alignment() {
    for (std::size_t alignment = 1; alignment <= 64; alignment *= 2) {
        for (const std::size_t size : {std::size_t{1}, std::size_t{24}, std::size_t{100}}) {
            void* aligned = ::operator new(size, std::align_val_t{alignment});
            BOOST_TEST_EQ(reinterpret_cast<std::uintptr_t>(aligned) % alignment, 0U);
            ::operator delete(aligned, std::align_val_t{alignment});
            void* resource = std::pmr::new_delete_resource()->allocate(size, alignment);
            BOOST_TEST_EQ(reinterpret_cast<std::uintptr_t>(resource) % alignment, 0U);
            std::pmr::new_delete_resource()->deallocate(resource, size, alignment);
        }
    }
    void* plain = ::operator new(0);
    BOOST_TEST(plain != nullptr);
    ::operator delete(plain);
}

/**
 * What the counts can see, so that a zero below means something: a counted
 * operator new, and a counted call of the C library where it is counted.
 */
void the_counts_see_an_allocation() {
    counting = true;
    void* pointer = ::operator new(40);
    counting = false;
    ::operator delete(pointer);
    BOOST_TEST_EQ(global_allocations, 1U);
#ifdef WEBCPP_TEST_TRYSTERO_COUNTS_C_ALLOCATIONS
    // The C library's allocation, on purpose.
    void* volatile memory = nullptr;
    counting = true;
    memory = std::malloc(40);
    counting = false;
    std::free(memory);
    BOOST_TEST_EQ(c_allocations, 1U);
#endif
    global_allocations = 0;
    c_allocations = 0;
    // A return of memory a resource never gave, and what is still out.
    counting_resource giver;
    counting_resource other;
    void* given = giver.allocate(48, 8);
    BOOST_TEST_EQ(giver.outstanding, 48U);
    other.deallocate(given, 48, 8);
    BOOST_TEST_EQ(other.foreign, 1U);
    giver.deallocate(given, 48, 8);
    BOOST_TEST_EQ(giver.outstanding, 0U);
    BOOST_TEST_EQ(giver.foreign, 0U);
}

/** Two rooms exchange two payloads that make builds; the second is counted. */
template <class Payload, class Make>
void exchange_allocates_through_the_rooms_resources(Make make) {
    // The resources outlive the io_context, whose pending operations they hold.
    counting_resource a_memory;
    counting_resource z_memory;
    boost::asio::io_context io;
    trystero::test::memory_network network{io};
    // The rooms meet on a relay the memory network has: one that names none
    // derives Trystero's public relays, which it refuses.
    constexpr std::string_view relay = "wss://relay.test";
    network.relay(relay);
    const boost::json::storage_ptr json_storage =
        boost::json::make_shared_resource<trystero::json_resource>(&a_memory);
    trystero::room a(io.get_executor(), network,
                     {
                         .app_id = "allocation",
                         .relay_urls = {std::pmr::string(relay)},
                         .self_id = "AAAAAAAAAAAAAAAAAAAA",
                         .memory = &a_memory,
                     },
                     "room");
    trystero::room z(io.get_executor(), network,
                     {
                         .app_id = "allocation",
                         .relay_urls = {std::pmr::string(relay)},
                         .self_id = "zzzzzzzzzzzzzzzzzzzz",
                         .memory = &z_memory,
                     },
                     "room");
    std::size_t received = 0;
    auto from_a = a.make_action<Payload>("chat");
    z.make_action<Payload>("chat").on_message(
        [&](const Payload& /*payload*/, const trystero::message_info& /*info*/) { ++received; });
    const auto send = [&] {
        from_a.async_send(make(&a_memory, json_storage),
                          boost::asio::bind_allocator(std::pmr::polymorphic_allocator<>(&a_memory),
                                                      [](boost::system::error_code /*ec*/) {}));
    };
    // Joined, connected and one payload through, before the window opens: a
    // payload sent before the peers meet reaches nobody.
    if (!BOOST_TEST(
            run_until(io, [&] { return a.peers().size() == 1 && z.peers().size() == 1; }))) {
        return;
    }
    send();
    if (!BOOST_TEST(run_until(io, [&] { return received == 1; }))) {
        return;
    }
    const std::size_t before = a_memory.allocations + z_memory.allocations;
    counting = true;
    send();
    const bool arrived = run_until(io, [&] { return received == 2; });
    counting = false;
    BOOST_TEST(arrived);
    BOOST_TEST_EQ(global_allocations, 0U);
    BOOST_TEST_EQ(c_allocations, 0U);
    BOOST_TEST_GT(a_memory.allocations + z_memory.allocations, before);
    BOOST_TEST_EQ(a_memory.foreign + z_memory.foreign, 0U);
    global_allocations = 0;
    c_allocations = 0;
}

/** Runs io for about `milliseconds`, whatever happens. */
void run_for(boost::asio::io_context& io, int milliseconds) {
    for (int slice = 0; slice < milliseconds / 10; ++slice) {
        io.restart();
        io.run_for(std::chrono::milliseconds(10));  // lint-run: the memory relays post to it
    }
}

/**
 * Asio's own once-per-context allocations, made before anything is counted:
 * the timer service and the reactor, the scheduler's first operation, and room
 * in the timer queue for more waits at once than two rooms keep.
 */
void warm(boost::asio::io_context& io) {
    std::vector<std::unique_ptr<boost::asio::steady_timer>> timers;
    for (int i = 0; i < 256; ++i) {
        timers.push_back(std::make_unique<boost::asio::steady_timer>(io, std::chrono::hours(1)));
        timers.back()->async_wait([](boost::system::error_code /*ec*/) {});
    }
    for (auto& timer : timers) {
        timer->cancel();
    }
    boost::asio::post(io, [] {});
    run_for(io, 20);
}

// tag::config[]
/** A room's configuration, every part of it in `memory`, made before the window. */
trystero::room_config config_in(counting_resource& memory, std::string_view self_id,
                                std::string_view relay, std::size_t& lines) {
    return trystero::room_config{
        .app_id = std::pmr::string("allocation", &memory),
        .password = std::pmr::string("a password", &memory),
        .relay_urls =
            std::pmr::vector<std::pmr::string>({std::pmr::string(relay, &memory)}, &memory),
        .self_id = std::pmr::string(self_id, &memory),
        .log =
            {
                std::allocator_arg,
                std::pmr::polymorphic_allocator<>(&memory),
                [&lines](std::string_view /*category*/, std::string_view /*text*/) { ++lines; },
            },
        .memory = &memory,
    };
}

// end::config[]

/**
 * Two rooms with a password, a log and a relay of their own are made one after
 * the other, join, exchange a payload that travels in chunks, measure a round
 * trip, and one says goodbye; both are destroyed and the io_context drained. From the first
 * construction to the drain, operator new sees the two rooms' JSON storage
 * counters alone, which Boost.JSON allocates once per room, and the C library
 * nothing (doc: #trystero-allocation).
 */
void a_room_allocates_through_its_resource_from_join_to_destruction() {
    counting_resource network_memory;
    counting_resource a_memory;
    counting_resource z_memory;
    boost::asio::io_context io;
    trystero::test::memory_network network{io, 1, &network_memory};
    constexpr std::string_view relay = "wss://relay.test";
    network.relay(relay);
    warm(io);
    std::size_t a_lines = 0;
    std::size_t z_lines = 0;
    std::size_t files = 0;
    std::size_t pings = 0;
    std::size_t left = 0;
    bool joined = false;
    bool exchanged = false;
    bool farewell = false;
    // tag::allocator[]
    const auto in = [](counting_resource& memory) {
        return std::pmr::polymorphic_allocator<>(&memory);
    };
    // end::allocator[]
    counting = true;
    {
        // z is subscribed and announcing before a arrives, so that z, whose id is
        // the higher, also asks a to offer, as Trystero's counter-announcement does.
        // Each configuration is a temporary, which leaves no moved-from object behind:
        // MSVC's debug standard library keeps, in a moved-from std::pmr::string or
        // vector, 16 bytes of its resource for the container's debug proxy, 64 bytes
        // of a moved-from configuration's four, which the test would count as the
        // room's until the end of the function.
        trystero::room z(io.get_executor(), network,
                         config_in(z_memory, "zzzzzzzzzzzzzzzzzzzz", relay, z_lines), "room");
        run_for(io, 100);
        {
            trystero::room a(io.get_executor(), network,
                             config_in(a_memory, "AAAAAAAAAAAAAAAAAAAA", relay, a_lines), "room");
            a.on_peer_leave([&left](std::string_view /*peer*/) { ++left; });
            z.make_action<trystero::pmr_bytes>("file").on_message(
                [&files](const trystero::pmr_bytes& data, const trystero::message_info& /*info*/) {
                    files += data.size() == 100'000 ? 1 : 0;
                });
            joined = run_until(io, [&] { return a.peers().size() == 1 && z.peers().size() == 1; });
            // tag::bind-allocator[]
            a.make_action<trystero::pmr_bytes>("file").async_send(
                trystero::pmr_bytes(100'000, 7, &a_memory),
                boost::asio::bind_allocator(in(a_memory), [](boost::system::error_code /*ec*/) {}));
            a.async_ping(z.self_id(),
                         boost::asio::bind_allocator(
                             in(a_memory),
                             [&pings](boost::system::error_code ec,
                                      std::chrono::milliseconds /*rtt*/) { pings += ec ? 0 : 1; }));
            // end::bind-allocator[]
            exchanged = run_until(io, [&] { return files == 1 && pings == 1; });
            bool gone = false;
            z.async_leave(boost::asio::bind_allocator(
                in(z_memory), [&gone](boost::system::error_code /*ec*/) { gone = true; }));
            farewell = run_until(io, [&] { return gone && left == 1; });
        }
    }
    // Both rooms are gone, and operations they left are still pending on the io_context,
    // holding memory of their resources: why a resource must outlive them (doc:
    // #trystero-allocation). The order that holds:
    //
    //     std::pmr::unsynchronized_pool_resource memory;  // first: destroyed last
    //     boost::asio::io_context io;                     // destroyed before the resource
    //     {
    //         trystero::room room(io.get_executor(), factory, {.memory = &memory}, "id");
    //         ...
    //     }
    //     io.run();  // lint-run: the program's I/O; or destroy io: pending operations free
    const std::size_t left_behind = a_memory.outstanding + z_memory.outstanding;
    run_for(io, 300);
    counting = false;
    BOOST_TEST_GT(left_behind, 0U);
    BOOST_TEST_EQ(a_memory.outstanding, 0U);
    BOOST_TEST_EQ(z_memory.outstanding, 0U);
    BOOST_TEST_EQ(a_memory.foreign + z_memory.foreign + network_memory.foreign, 0U);
    BOOST_TEST(joined);
    BOOST_TEST(exchanged);
    BOOST_TEST(farewell);
    BOOST_TEST_GT(a_lines, 0U);
    BOOST_TEST_GT(z_lines, 0U);
    BOOST_TEST_EQ(global_allocations, 2U);
    BOOST_TEST_EQ(c_allocations, 0U);
    BOOST_TEST_GT(network_memory.allocations, 0U);
    global_allocations = 0;
    c_allocations = 0;
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
    release(pointer);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer, std::size_t /*size*/) noexcept {
    release(pointer);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer, std::align_val_t /*alignment*/) noexcept {
    release(pointer);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    release(pointer);
}

int main() {
    count_this_thread();
#if defined(WEBCPP_TEST_TRYSTERO_COUNTS_C_ALLOCATIONS) && defined(__APPLE__) && \
    !defined(__EMSCRIPTEN__)
    malloc_logger = &count_c_allocation;
#endif
    the_replaced_operators_serve_every_alignment();
    the_counts_see_an_allocation();
    exchange_allocates_through_the_rooms_resources<std::pmr::string>(
        [](std::pmr::memory_resource* memory, const boost::json::storage_ptr& /*storage*/) {
            return std::pmr::string("a text that no small string holds", memory);
        });
    exchange_allocates_through_the_rooms_resources<std::pmr::vector<std::uint8_t>>(
        [](std::pmr::memory_resource* memory, const boost::json::storage_ptr& /*storage*/) {
            return std::pmr::vector<std::uint8_t>(4096, 7, memory);
        });
    exchange_allocates_through_the_rooms_resources<boost::json::value>(
        [](std::pmr::memory_resource* /*memory*/, const boost::json::storage_ptr& storage) {
            return boost::json::value(std::string_view("a text that no small string holds"),
                                      storage);
        });
    a_room_allocates_through_its_resource_from_join_to_destruction();
    return boost::report_errors();
}
