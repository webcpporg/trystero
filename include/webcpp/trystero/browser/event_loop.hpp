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
#include <boost/asio/execution.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

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
 * An Asio executor over an `event_loop`: what it is given runs on the loop's io_context, and giving
 * it work while no JavaScript event is being handled schedules one `poll()` in a microtask. So
 * posted work runs without a polling loop and without the application calling `poll()` (the
 * documentation's chapter on the browser).
 */
class executor {
public:
    explicit executor(event_loop& loop) noexcept : loop_(&loop) {}

    [[nodiscard]] boost::asio::execution_context& query(
        boost::asio::execution::context_t /*context*/) const noexcept;

    static constexpr boost::asio::execution::blocking_t::never_t query(
        boost::asio::execution::blocking_t /*blocking*/) noexcept {
        return {};
    }

    [[nodiscard]] executor require(
        boost::asio::execution::blocking_t::never_t /*never*/) const noexcept {
        return executor(*loop_);
    }

    template <class F>
    void execute(F&& f) const;

    friend bool operator==(const executor& a, const executor& b) noexcept {
        return a.loop_ == b.loop_;
    }

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
 */
class event_loop {
public:
    using callback = handler<void(int slot, emscripten::val event)>;

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
     * A JavaScript string of UTF-8 text, decoded straight from the wasm memory the view points
     * into: no std::string is made to carry it across, as `emscripten::val(std::string)` would.
     */
    [[nodiscard]] emscripten::val text(std::string_view utf8) const {
        return decoder_.call<emscripten::val>(
            "decode", emscripten::val(emscripten::typed_memory_view(utf8.size(), utf8.data())));
    }

    /**
     * The UTF-8 bytes of a JavaScript string, copied straight into a string of `memory`: no
     * std::string is made to carry them across, as `emscripten::val::as<std::string>()` would.
     */
    std::pmr::string utf8(const emscripten::val& text, std::pmr::memory_resource* memory) const {
        const auto encoded = encoder_.call<emscripten::val>("encode", text);
        std::pmr::string out(encoded["length"].as<std::size_t>(), '\0', memory);
        emscripten::val(
            emscripten::typed_memory_view(out.size(), reinterpret_cast<unsigned char*>(out.data())))
            .call<void>("set", encoded);
        return out;
    }

    event_loop(const event_loop&) = delete;
    event_loop& operator=(const event_loop&) = delete;

    ~event_loop() { token_.set("alive", false); }

    boost::asio::io_context& io() noexcept { return io_; }

    executor get_executor() noexcept { return executor(*this); }

    /**
     * Registers `f`, which stays in its own allocator's resource (a room's, for a room's object);
     * only the registration's entry is the loop's.
     */
    int add(callback f) {
        const int id = next_id_++;
        const auto allocator = f.get_allocator();
        callbacks_.emplace(id, std::allocate_shared<callback>(allocator, std::move(f)));
        return id;
    }

    void remove(int id) { callbacks_.erase(id); }

    /** A JavaScript function of one argument that delivers it to registration `id`, slot `slot`. */
    [[nodiscard]] emscripten::val handler(int id, int slot) const {
        return emscripten::val::module_property("trystero_dispatch")
            .call<emscripten::val>("bind", emscripten::val::null(), token_,
                                   emscripten::val(reinterpret_cast<std::uintptr_t>(this)),
                                   emscripten::val(id), emscripten::val(slot));
    }

    /** Runs a registration's callback for a JavaScript event, then everything that became ready. */
    void dispatch(int id, int slot, emscripten::val event) {
        ++depth_;
        if (id == poll_id) {
            poll_scheduled_ = false;
        } else if (const auto it = callbacks_.find(id); it != callbacks_.end()) {
            const auto f = it->second;  // the callback may remove itself
            (*f)(slot, std::move(event));
        }
        --depth_;
        if (depth_ == 0) {
            poll();
        }
    }

    /** Runs every ready handler; what they post runs too. */
    void poll() {
        ++depth_;
        if (io_.stopped()) {
            io_.restart();
        }
        io_.poll();
        --depth_;
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

/** The one function JavaScript calls; `browser/src.hpp` exports it. Events for a destroyed loop are
 * dropped. */
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
