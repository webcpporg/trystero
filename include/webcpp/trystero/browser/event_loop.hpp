// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_BROWSER_EVENT_LOOP_HPP
#define WEBCPP_TRYSTERO_BROWSER_EVENT_LOOP_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/browser/js.hpp>
#include <webcpp/trystero/handler.hpp>

#include <emscripten/val.h>
#include <boost/asio/bind_allocator.hpp>       // lint-world: binds an allocator only
#include <boost/asio/execution.hpp>            // lint-world: posts and polls, never blocks
#include <boost/asio/executor_work_guard.hpp>  // lint-world: posts and polls, never blocks
#include <boost/asio/io_context.hpp>           // lint-world: posts and polls, never blocks
#include <boost/asio/post.hpp>                 // lint-world: posts and polls, never blocks

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace webcpp::trystero::browser {

class event_loop;

/**
 * An Asio executor over an `event_loop`: what it is given runs on the loop's io_context.
 *
 * Giving it work while no JavaScript event is being handled schedules one `poll()` in a microtask,
 * so posted work runs without a polling loop and without the application calling `poll()`. A room
 * in the browser is constructed with it, through `boost::asio::any_io_executor`.
 *
 * @see "The browser backend", in the guide.
 */
class executor {
public:
    /**
     * Constructs an executor of `loop`.
     *
     * @param loop The loop, which must outlive the executor and every copy of it.
     */
    explicit executor(event_loop& loop) noexcept : loop_(&loop) {}

    /**
     * The execution context, the property Asio queries.
     *
     * @return The loop's io_context.
     */
    [[nodiscard]] boost::asio::execution_context& query(
        boost::asio::execution::context_t /*context*/) const noexcept;

    /**
     * The blocking property, which Asio queries: `execute` never runs the work inside the call.
     *
     * @return `blocking.never`.
     */
    static constexpr boost::asio::execution::blocking_t::never_t query(
        boost::asio::execution::blocking_t /*blocking*/) noexcept {
        return {};
    }

    /**
     * The executor with `blocking.never` required, which it already is.
     *
     * @return A copy of this executor.
     */
    [[nodiscard]] executor require(
        boost::asio::execution::blocking_t::never_t /*never*/) const noexcept {
        return executor(*loop_);
    }

    /**
     * Posts `f` to the loop's io_context, and schedules a poll unless one is coming.
     *
     * @tparam F A nullary callable.
     * @param f The work, allocated as its associated allocator says.
     */
    template <class F>
    void execute(F&& f) const;

    /**
     * Whether two executors are of one loop.
     *
     * @param a An executor.
     * @param b Another.
     * @return Whether they run work on the same loop.
     */
    friend bool operator==(const executor& a, const executor& b) noexcept {
        return a.loop_ == b.loop_;
    }

    /**
     * Whether two executors are of different loops.
     *
     * @param a An executor.
     * @param b Another.
     * @return Whether they run work on different loops.
     */
    friend bool operator!=(const executor& a, const executor& b) noexcept {
        return a.loop_ != b.loop_;
    }

private:
    event_loop* loop_;
};

/**
 * The bridge between the page's JavaScript event loop and an Asio io_context, which never runs on
 * its own in a browser: it is polled after every JavaScript event that enters C++, and when work is
 * posted from outside one.
 *
 * JavaScript reaches C++ through `handler(id, slot)`: a function bound to this loop, a registration
 * and a slot, passed to `trystero_dispatch` (browser/src.hpp). An object registers a callback with
 * `add` and removes it with `remove` when it is destroyed, so a late event for a destroyed object
 * finds nothing. A loop must outlive the page's use of it; its destructor marks every handler it
 * made as dead.
 *
 * Tip: a loop serves every room of the page, so the entries of its registrations are in the
 * resource it is constructed with; a registered callback stays in its own allocator's resource, a
 * room's for a room's object, until it is removed or, for a promise, until the promise settles.
 *
 * @see "The browser backend", in the guide.
 */
class event_loop {
public:
    /** What a registration calls: with the slot the event came through, and the event. */
    using callback = handler<void(int slot, emscripten::val event)>;

    /**
     * Constructs a loop over `io`, which it keeps from running out of work.
     *
     * @param io The io_context the loop polls, which must outlive the loop.
     * @param memory The resource the entries of the registrations are allocated in; it must
     * outlive the loop.
     */
    explicit event_loop(boost::asio::io_context& io,
                        std::pmr::memory_resource* memory = std::pmr::get_default_resource())
        : io_(io),
          work_(boost::asio::make_work_guard(io)),
          token_(emscripten::val::object()),
          decoder_(emscripten::val::global("TextDecoder").new_()),
          encoder_(emscripten::val::global("TextEncoder").new_()),
          callbacks_(memory) {
        token_.set("alive", true);
    }

    /**
     * A JavaScript string of UTF-8 text, decoded by a `TextDecoder` straight from the wasm memory
     * the view points into.
     *
     * No `std::string` is made to carry it across, as `emscripten::val(std::string)` would.
     *
     * @param utf8 The text, read during the call.
     * @return The JavaScript string.
     */
    [[nodiscard]] emscripten::val text(std::string_view utf8) const {
        return decoder_.call<emscripten::val>(
            "decode", emscripten::val(emscripten::typed_memory_view(utf8.size(), utf8.data())));
    }

    /**
     * The UTF-8 bytes of a JavaScript string, encoded by a `TextEncoder` and copied straight into a
     * string of `memory`.
     *
     * No `std::string` is made to carry them across, as `emscripten::val::as<std::string>()` would.
     *
     * @param text The JavaScript string.
     * @param memory The resource the copy is allocated in.
     * @return The text, or an empty one when the string cannot be encoded.
     */
    std::pmr::string utf8(const emscripten::val& text, std::pmr::memory_resource* memory) const {
        auto out = utf8(text, memory, std::numeric_limits<std::size_t>::max());
        return out ? std::move(*out) : std::pmr::string(memory);
    }

    /**
     * The UTF-8 bytes of a JavaScript string, as the other overload makes them, when they are at
     * most `max_bytes`; nothing is copied into WebAssembly otherwise.
     *
     * The string's length is checked first, in UTF-16 units, each at least one byte, so that a
     * longer string is never encoded; then the encoded bytes are.
     *
     * @param text The JavaScript string.
     * @param memory The resource the copy is allocated in.
     * @param max_bytes The most bytes the text may hold.
     * @return The text, or nothing when it is longer or cannot be encoded.
     */
    std::optional<std::pmr::string> utf8(const emscripten::val& text,
                                         std::pmr::memory_resource* memory,
                                         std::size_t max_bytes) const {
        if (text["length"].as<double>() > static_cast<double>(max_bytes)) {
            return std::nullopt;
        }
        const auto encoded = detail::js_call(encoder_, "encode", text);
        if (!encoded.ok || encoded.value["length"].as<double>() > static_cast<double>(max_bytes)) {
            return std::nullopt;
        }
        std::pmr::string out(encoded.value["length"].as<std::size_t>(), '\0', memory);
        emscripten::val(
            emscripten::typed_memory_view(out.size(), reinterpret_cast<unsigned char*>(out.data())))
            .call<void>("set", encoded.value);
        return out;
    }

    /** Not copyable: the page's functions name this loop by its address. */
    event_loop(const event_loop&) = delete;

    /** Not copy-assignable: the page's functions name this loop by its address. */
    event_loop& operator=(const event_loop&) = delete;

    /** Destroys the loop, and marks every handler it made as dead: their events are dropped. */
    ~event_loop() { token_.set("alive", false); }

    /**
     * The io_context the loop polls.
     *
     * @return The io_context it was constructed with.
     */
    boost::asio::io_context& io() noexcept { return io_; }

    /**
     * An executor of this loop, what a room in the browser is constructed with.
     *
     * @return The executor.
     */
    executor get_executor() noexcept { return executor(*this); }

    /**
     * Registers a callback, which JavaScript reaches through @ref handler.
     *
     * The callback stays in its own allocator's resource (a room's, for a room's object); only the
     * registration's entry is the loop's.
     *
     * @param f The callback.
     * @return The registration's id, which @ref handler and @ref remove take.
     */
    int add(callback f) {
        const int id = next_id_++;
        const auto allocator = f.get_allocator();
        callbacks_.emplace(id, std::allocate_shared<callback>(allocator, std::move(f)));
        return id;
    }

    /**
     * Removes a registration: an event for it from now on finds nothing.
     *
     * @param id The registration's id.
     */
    void remove(int id) { callbacks_.erase(id); }

    /**
     * A JavaScript function of one argument that delivers it to registration `id`, slot `slot`.
     *
     * An exception that escapes the call, which no operation of trystero raises, is reported to
     * the page (`reportError`) and does not stop the loop: the loop recovers, as @ref recover
     * says, and the function returns.
     *
     * @param id The registration's id.
     * @param slot A number the callback receives, which tells its events apart.
     * @return The function, which calls `trystero_dispatch` with this loop, `id`, `slot` and its
     * argument (browser/src.hpp).
     */
    [[nodiscard]] emscripten::val handler(int id, int slot) const {
        // The loop's address crosses as a pointer, and comes back through trystero_dispatch.
        return emscripten::val::take_ownership(
            webcpp_trystero_js_handler(token_.as_handle(), this, id, slot));
    }

    /**
     * How many calls into C++ are running: 0 between events, 1 inside a callback.
     *
     * @return The depth.
     */
    [[nodiscard]] int depth() const noexcept { return depth_; }

    /**
     * Brings the loop back after an exception escaped a call into C++ that entered at `depth`:
     * the calls it unwound no longer count, and a poll runs the work that is ready.
     *
     * Tip: without WebAssembly exceptions no destructor runs while an exception unwinds, so the
     * depth a call raised stays raised; the page's function reads the depth before it enters C++
     * and gives it back here, so that a nested entry that escapes leaves the depth of the call
     * still running below it, never less. The page's function also puts WebAssembly's stack
     * pointer back where it was before the call, which no epilogue of the unwound frames did, but
     * what those frames owned on the heap stays allocated: 312 bytes per escape, measured, for a
     * frame that held a string of 300. With exceptions, `trystero_dispatch` catches a C++
     * exception itself, which frees it and runs every destructor, and recovers the same way.
     *
     * @param depth The depth when the call entered, as `depth()` read it; never below 0.
     */
    void recover(int depth) {
        depth_ = depth > 0 ? depth : 0;
        poll_scheduled_ = false;
        schedule_poll();
    }

    /**
     * Runs a registration's callback for a JavaScript event, then everything that became ready.
     *
     * An event the page delivers while C++ is already running, inside a call C++ made into
     * JavaScript (a WebSocket that fires `close` from inside `close()`, as node's does), is
     * posted rather than run, so that a handler never runs inside the call that caused it.
     *
     * @param id The registration's id; an unknown one runs nothing.
     * @param slot The slot the event came through.
     * @param event The event.
     */
    void dispatch(int id, int slot, emscripten::val event) {
        {
            const running call(depth_);
            if (id == poll_id) {
                poll_scheduled_ = false;
            } else if (const auto it = callbacks_.find(id); it != callbacks_.end() && depth_ > 1) {
                // Delivered when it runs, by id: the registration may be gone by then, and its
                // object with it. Ids are never reused.
                boost::asio::post(io_, boost::asio::bind_allocator(
                                           it->second->get_allocator(),
                                           [this, id, slot, event = std::move(event)]() mutable {
                                               deliver(id, slot, std::move(event));
                                           }));
            } else {
                deliver(id, slot, std::move(event));
            }
        }
        if (depth_ == 0) {
            poll();
        }
    }

    /** Runs every ready handler; what they post runs too. */
    void poll() {
        const running call(depth_);
        if (io_.stopped()) {
            io_.restart();
        }
        io_.poll();
    }

    /** Schedules a `poll()` in a microtask unless one is pending or a poll is running, which will
     * see the work. */
    void schedule_poll() {
        if (depth_ > 0 || poll_scheduled_) {
            return;
        }
        poll_scheduled_ = true;
        emscripten::val::global("queueMicrotask")(handler(poll_id, 0));
    }

private:
    /** Runs a registration's callback, when it still has one. */
    void deliver(int id, int slot, emscripten::val event) {
        if (const auto it = callbacks_.find(id); it != callbacks_.end()) {
            const auto f = it->second;  // the callback may remove itself
            (*f)(slot, std::move(event));
        }
    }

    /**
     * Counts a call into C++ for as long as it runs, however it ends.
     *
     * Tip: with WebAssembly exceptions, an exception that unwinds through the call runs this
     * destructor, and the depth comes back down; without them nothing runs on the way, and
     * `recover` brings the depth back from JavaScript. Neither should be needed: the backend makes
     * every call that can throw through browser/js.hpp.
     */
    class running {
    public:
        explicit running(int& depth) noexcept : depth_(depth) { ++depth_; }

        running(const running&) = delete;
        running& operator=(const running&) = delete;

        ~running() { --depth_; }

    private:
        int& depth_;
    };

    static constexpr int poll_id = 0;

    boost::asio::io_context& io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    emscripten::val token_;
    emscripten::val decoder_;
    emscripten::val encoder_;
    std::pmr::unordered_map<int, std::shared_ptr<callback>> callbacks_;
    int next_id_ = 1;
    int depth_ = 0;
    bool poll_scheduled_ = false;
};

inline boost::asio::execution_context& executor::query(
    boost::asio::execution::context_t /*context*/) const noexcept {
    return loop_->io();
}

template <class F>
void executor::execute(F&& f) const {
    boost::asio::post(loop_->io(), std::forward<F>(f));
    loop_->schedule_poll();
}

/**
 * The one function JavaScript calls, `trystero_dispatch`, which `browser/src.hpp` exports.
 *
 * Events for a destroyed loop are dropped. With exceptions, a C++ exception that escapes the
 * callback stops here: it is reported to the page, as `event_loop::handler` says, and the loop
 * recovers.
 *
 * @param token The loop's token, whose `alive` is false once the loop is destroyed.
 * @param loop The loop's address, as a number.
 * @param id The registration's id.
 * @param slot The slot the event came through.
 * @param event The event.
 * @see "The browser backend", in the guide.
 */
inline void dispatch(const emscripten::val& token, std::uintptr_t loop, int id, int slot,
                     emscripten::val event) {
    if (!token["alive"].as<bool>()) {
        return;
    }
    // The page holds the loop's address as a number, the one form it passes back through embind.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto& target = *reinterpret_cast<event_loop*>(loop);
#ifdef __cpp_exceptions
    // A C++ exception is caught here, where catching it frees it: one that reached the page's
    // function would keep its object, and what the object owns, for good.
    const int entered = target.depth();
    try {
        target.dispatch(id, slot, std::move(event));
    } catch (const std::exception& e) {
        detail::report_escape(emscripten::val::u8string(e.what()));
        target.recover(entered);
    } catch (...) {
        detail::report_escape(emscripten::val("an exception that is not a std::exception"));
        target.recover(entered);
    }
#else
    target.dispatch(id, slot, std::move(event));
#endif
}

/**
 * The function JavaScript calls, `trystero_recover`, when an exception escaped
 * `trystero_dispatch`: the loop recovers, as `event_loop::recover` says.
 *
 * Calls for a destroyed loop do nothing.
 *
 * @param token The loop's token, whose `alive` is false once the loop is destroyed.
 * @param loop The loop's address, as a number.
 * @param depth The loop's depth when the call that escaped entered, as `trystero_depth` read it.
 * @see "The browser backend", in the guide.
 */
inline void recover(const emscripten::val& token, std::uintptr_t loop, int depth) {
    if (!token["alive"].as<bool>()) {
        return;
    }
    // As in dispatch: the page holds the loop's address as a number.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    reinterpret_cast<event_loop*>(loop)->recover(depth);
}

/**
 * The function JavaScript calls, `trystero_depth`, before it enters C++ through
 * `trystero_dispatch`: the depth `trystero_recover` restores if the call escapes.
 *
 * @param token The loop's token, whose `alive` is false once the loop is destroyed.
 * @param loop The loop's address, as a number.
 * @return The loop's depth, or 0 for a destroyed loop.
 * @see "The browser backend", in the guide.
 */
inline int depth(const emscripten::val& token, std::uintptr_t loop) {
    if (!token["alive"].as<bool>()) {
        return 0;
    }
    // As in dispatch: the page holds the loop's address as a number.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<const event_loop*>(loop)->depth();
}

}  // namespace webcpp::trystero::browser

#endif
