// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_BROWSER_EVENT_LOOP_HPP
#define WEBCPP_TRYSTERO_BROWSER_EVENT_LOOP_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/handler.hpp>

#include <emscripten/val.h>
#include <boost/asio/execution.hpp>            // lint-world: posts and polls, never blocks
#include <boost/asio/executor_work_guard.hpp>  // lint-world: posts and polls, never blocks
#include <boost/asio/io_context.hpp>           // lint-world: posts and polls, never blocks
#include <boost/asio/post.hpp>                 // lint-world: posts and polls, never blocks

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
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
     * @return The text.
     */
    std::pmr::string utf8(const emscripten::val& text, std::pmr::memory_resource* memory) const {
        const auto encoded = encoder_.call<emscripten::val>("encode", text);
        std::pmr::string out(encoded["length"].as<std::size_t>(), '\0', memory);
        emscripten::val(
            emscripten::typed_memory_view(out.size(), reinterpret_cast<unsigned char*>(out.data())))
            .call<void>("set", encoded);
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
     * @param id The registration's id.
     * @param slot A number the callback receives, which tells its events apart.
     * @return The function, `trystero_dispatch` bound to this loop, `id` and `slot`.
     */
    [[nodiscard]] emscripten::val handler(int id, int slot) const {
        return emscripten::val::module_property("trystero_dispatch")
            .call<emscripten::val>("bind", emscripten::val::null(), token_,
                                   emscripten::val(reinterpret_cast<std::uintptr_t>(this)),
                                   emscripten::val(id), emscripten::val(slot));
    }

    /**
     * Runs a registration's callback for a JavaScript event, then everything that became ready.
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
            } else if (const auto it = callbacks_.find(id); it != callbacks_.end()) {
                const auto f = it->second;  // the callback may remove itself
                (*f)(slot, std::move(event));
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
        // A microtask's function is called with no argument, and the handler's takes one: bound
        // to undefined, or embind refuses the call with a BindingError and the poll never runs.
        const emscripten::val next_poll = handler(poll_id, 0);
        emscripten::val::global("queueMicrotask")(next_poll.call<emscripten::val>(
            "bind", emscripten::val::null(), emscripten::val::undefined()));
    }

private:
    /**
     * Counts a call into C++ for as long as it runs, however it ends.
     *
     * Tip: a JavaScript exception thrown inside a callback unwinds through the call to the page;
     * the build's WebAssembly exceptions run this destructor on the way, so the depth comes back
     * down and the loop goes on polling. Counted by hand, the depth stayed raised and the loop
     * never polled again: every room of the page stopped.
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
 * Events for a destroyed loop are dropped.
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
    reinterpret_cast<event_loop*>(loop)->dispatch(id, slot, std::move(event));
}

}  // namespace webcpp::trystero::browser

#endif
