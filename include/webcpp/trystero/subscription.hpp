// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_SUBSCRIPTION_HPP
#define WEBCPP_TRYSTERO_SUBSCRIPTION_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/handler.hpp>

#include <boost/assert.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>
#include <vector>

namespace webcpp::trystero {

/**
 * How a listener is added, Trystero's `ListenerOptions` and `ReplayListenerOptions` without their
 * `signal`: `subscription::unsubscribe` takes the place of an `AbortSignal`.
 */
struct listen_options {
    /** Whether the listener runs once: it is removed before its first run. */
    bool once = false;
    /**
     * Whether the listener is first called, within the call that adds it, with what is current: the
     * peers or the users already joined. Only the room's join events replay; the other events
     * ignore it, as Trystero's do.
     */
    bool replay = false;
};

namespace detail {

/** What a subscription refers to: whether its listener is still in place. */
struct listener_state {
    /** False once the listener was removed, by its subscription or by its `once`. */
    bool active = true;
};

template <class Signature>
class listeners;

}  // namespace detail

/**
 * A listener in place, what an `on_` function returns, Trystero's unsubscribe function.
 *
 * `unsubscribe` removes the listener. Destroying the subscription leaves the listener in place, as
 * dropping the function Trystero returns does: a program that never removes a listener need not
 * keep it. A listener goes with the room or the action it listens to.
 *
 * Tip: it refers to its listener weakly, through storage of the room's resource, which must
 * outlive the subscription.
 */
class subscription {
public:
    /** A subscription of no listener: `active` is false and `unsubscribe` does nothing. */
    subscription() noexcept = default;

    /** Not copyable: one subscription removes its listener. */
    subscription(const subscription&) = delete;

    /** Not copy-assignable: one subscription removes its listener. */
    subscription& operator=(const subscription&) = delete;

    /** Takes the listener of the subscription moved from, which then refers to none. */
    subscription(subscription&&) noexcept = default;

    /**
     * Takes the listener of the subscription moved from, which then refers to none; the listener
     * this one referred to stays in place, as on destruction.
     *
     * @return This subscription.
     */
    subscription& operator=(subscription&&) noexcept = default;

    /** Leaves the listener in place. */
    ~subscription() = default;

    /**
     * Removes the listener: it does not run again, not even for an event that is being reported
     * and has not reached it yet. Does nothing once the listener is gone.
     */
    void unsubscribe() noexcept {
        if (const auto state = state_.lock()) {
            state->active = false;
        }
        state_.reset();
    }

    /**
     * Whether the listener is still in place: not removed, not run once with `once`, and its room
     * or action still there.
     *
     * @return Whether it is.
     */
    [[nodiscard]] bool active() const noexcept {
        const auto state = state_.lock();
        return state && state->active;
    }

private:
    template <class Signature>
    friend class detail::listeners;

    explicit subscription(std::weak_ptr<detail::listener_state> state) noexcept
        : state_(std::move(state)) {}

    std::weak_ptr<detail::listener_state> state_;
};

namespace detail {

/**
 * The listeners of one event, Trystero's event emitter: each added with its options, each removed
 * through the @ref subscription that `add` returns, all in the resource of the listeners' owner.
 *
 * | Rule                       | What it does                                                    |
 * | order                      | listeners run in the order they were added                      |
 * | added while emitting       | does not run for that emission                                  |
 * | removed while emitting     | does not run, if its turn had not come                          |
 * | `once`                     | removed before it runs                                          |
 * | `replay`                   | called with each item at once, within `add`, before it is in    |
 * |                            | place, so that an emission meanwhile does not reach it          |
 *
 * @tparam R The listeners' result: `void` for an event, a value for a listener taken one at a
 * time (`call_first`).
 * @tparam Args What each listener is called with.
 */
template <class R, class... Args>
class listeners<R(Args...)> {
public:
    /** What the listeners are kept with. */
    using allocator_type = std::pmr::polymorphic_allocator<>;

    /** No listener, in `alloc`'s resource. */
    explicit listeners(allocator_type alloc) : entries_(alloc) {}

    /**
     * Adds a listener, after those in place.
     *
     * @param f The listener, which must not be empty.
     * @param options Its options; `replay` is ignored, nothing being current.
     * @return Its subscription.
     */
    subscription add(handler<R(Args...)> f, listen_options options) {
        return keep(make(std::move(f), options));
    }

    /**
     * Adds a listener, after those in place; with `options.replay`, first calls it with each item
     * of `replay` in turn, until it is removed (`once` removes it before the first).
     *
     * @tparam Items A range whose elements the listener takes as its one argument.
     * @param f The listener, which must not be empty.
     * @param options Its options.
     * @param replay What is current, read only with `options.replay`.
     * @return Its subscription, inactive when the listener was removed during the replay.
     */
    template <class Items>
        requires(sizeof...(Args) == 1)
    subscription add(handler<R(Args...)> f, listen_options options, const Items& replay) {
        auto e = make(std::move(f), options);
        if (options.replay) {
            for (const auto& item : replay) {
                if (!e->active) {
                    break;
                }
                if (e->once) {
                    e->active = false;
                }
                e->f(item);
            }
        }
        return keep(std::move(e));
    }

    /**
     * Calls every listener in place when the call starts, in order: one added meanwhile waits for
     * the next emission, one removed meanwhile does not run, and a `once` one is removed first.
     *
     * @param args What each listener is called with.
     */
    void emit(Args... args)  // NOLINT(performance-unnecessary-value-param): views and references
        requires std::is_void_v<R>
    {
        ++depth_;
        const std::size_t n = entries_.size();
        for (std::size_t i = 0; i < n; ++i) {
            // A copy: a listener may add others, which moves the entries.
            const std::shared_ptr<entry> e = entries_[i];
            if (!e->active) {
                continue;
            }
            if (e->once) {
                e->active = false;
            }
            e->f(args...);
        }
        if (--depth_ == 0) {
            compact();
        }
    }

    /**
     * Calls the first listener in place, as Trystero calls a handler taken one at a time; a `once`
     * one is removed first.
     *
     * @param args What the listener is called with.
     * @return What it returns.
     */
    R call_first(Args... args) {  // NOLINT(performance-unnecessary-value-param): views, references
        const auto it = std::ranges::find_if(entries_, [](const auto& e) { return e->active; });
        BOOST_ASSERT(it != entries_.end());
        const std::shared_ptr<entry> e = *it;
        if (e->once) {
            e->active = false;
        }
        return e->f(args...);
    }

    /**
     * Whether no listener is in place.
     *
     * @return Whether none is.
     */
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /**
     * How many listeners are in place.
     *
     * @return Their number.
     */
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(
            std::ranges::count_if(entries_, [](const auto& e) { return e->active; }));
    }

private:
    struct entry : listener_state {
        entry(handler<R(Args...)> fn, bool o) : f(std::move(fn)), once(o) {}

        handler<R(Args...)> f;
        bool once;
    };

    std::shared_ptr<entry> make(handler<R(Args...)> f, listen_options options) {
        BOOST_ASSERT(f);
        return std::allocate_shared<entry>(entries_.get_allocator(), std::move(f), options.once);
    }

    /** Puts a listener in place, unless a replay removed it, and returns its subscription. */
    subscription keep(std::shared_ptr<entry> e) {
        subscription out{std::weak_ptr<listener_state>(e)};
        if (e->active) {
            if (depth_ == 0) {
                compact();
            }
            entries_.push_back(std::move(e));
        }
        return out;
    }

    /** Forgets the listeners removed, once no emission walks them. */
    void compact() {
        std::erase_if(entries_, [](const auto& e) { return !e->active; });
    }

    std::pmr::vector<std::shared_ptr<entry>> entries_;
    /** The emissions under way, nested when a listener reports another of the same event. */
    std::size_t depth_ = 0;
};

}  // namespace detail

}  // namespace webcpp::trystero

#endif
