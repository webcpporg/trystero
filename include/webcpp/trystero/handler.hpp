// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_HANDLER_HPP
#define WEBCPP_TRYSTERO_HANDLER_HPP

#include <webcpp/trystero/config.hpp>

#include <boost/assert.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>

/**
 * A callable that allocates through a memory resource: what trystero holds every handler in, where
 * `std::function`, which takes no allocator since C++17, would allocate with `operator new`.
 */
namespace webcpp::trystero {

/**
 * A move-only callable of a signature, whose target is allocated through a memory resource; see the
 * partial specialization `handler<R(Args...)>`.
 *
 * @tparam Signature A function type, `R(Args...)`.
 * @see "Allocation", in the guide.
 */
template <class Signature>
class handler;

/**
 * A move-only callable of the signature `R(Args...)`, which holds its target inline when the target
 * fits in three pointers and is nothrow-movable, and otherwise in storage of its allocator.
 *
 * It replaces `std::function` wherever trystero keeps a handler (the backend's, the room's and
 * its logger's sink), since `std::function` takes no allocator since C++17. A room gives each
 * handler it keeps its `std::pmr::polymorphic_allocator<>`, over `room_config::memory`; one
 * constructed from a callable alone uses the default resource. The resource must outlive the
 * handler.
 *
 * Tip: a move assignment carries the source's allocator with its target, as moving the target there
 * does; the allocator-extended move, which a `std::pmr` container uses, moves the target into
 * another resource when the two differ.
 *
 * @tparam R The result type.
 * @tparam Args The parameter types.
 * @see "Allocation", in the guide.
 */
template <class R, class... Args>
class handler<R(Args...)> {
public:
    /** What the handler allocates its target with when the target does not fit inline. */
    using allocator_type = std::pmr::polymorphic_allocator<>;

    /** Constructs an empty handler, which must not be called. */
    handler() noexcept = default;

    // Implicit, as std::function's is: nullptr is passed where a handler is expected.
    /** Constructs an empty handler, which must not be called, from `nullptr`. */
    handler(std::nullptr_t) noexcept {}  // NOLINT(misc-explicit-constructor)

    // Implicit, as std::function's is: a lambda is passed where a handler is expected.
    /**
     * Constructs a handler of `f`, allocated with the default resource when it does not fit inline.
     *
     * @tparam F The callable's type, invocable with `Args...` and returning something convertible
     * to `R`.
     * @param f The callable, moved or copied into the handler.
     */
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, handler> &&
                 std::is_invocable_r_v<R, std::decay_t<F>&, Args...> &&
                 std::is_move_constructible_v<std::decay_t<F>>)
    handler(F&& f)  // NOLINT(misc-explicit-constructor)
        : handler(std::allocator_arg, allocator_type(), std::forward<F>(f)) {}

    /**
     * Constructs a handler of `f`, allocated with `allocator` when it does not fit inline.
     *
     * @tparam F The callable's type, invocable with `Args...` and returning something convertible
     * to `R`.
     * @param allocator What the target is allocated with, and what the handler keeps; its resource
     * must outlive the handler.
     * @param f The callable, moved or copied into the handler.
     */
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, handler> &&
                 std::is_invocable_r_v<R, std::decay_t<F>&, Args...> &&
                 std::is_move_constructible_v<std::decay_t<F>>)
    handler(std::allocator_arg_t /*tag*/, allocator_type allocator, F&& f)
        : resource_(allocator.resource()) {
        using target = std::decay_t<F>;
        if constexpr (fits_inline<target>) {
            ::new (static_cast<void*>(storage_.data())) target(std::forward<F>(f));
        } else {
            ::new (static_cast<void*>(storage_.data()))
                target*(make<target>(allocator, std::forward<F>(f)));
        }
        operations_ = &operations_of<target>;
    }

    /**
     * Constructs a handler of `other`'s target, moved into `allocator`'s resource when it differs
     * from `other`'s: the allocator-extended move a `std::pmr` container uses.
     *
     * @param allocator What the handler keeps, and allocates the target with when it moves it.
     * @param other The handler whose target is taken; it is left empty.
     */
    handler(std::allocator_arg_t /*tag*/, allocator_type allocator, handler&& other)
        : resource_(allocator.resource()) {
        if (other.operations_ == nullptr) {
            return;
        }
        if (resource_->is_equal(*other.resource_)) {
            other.operations_->move(other.storage_.data(), storage_.data());
        } else {
            other.operations_->rehome(other.storage_.data(), *other.resource_, storage_.data(),
                                      *resource_);
        }
        operations_ = std::exchange(other.operations_, nullptr);
    }

    /**
     * Constructs a handler of `other`'s target and allocator.
     *
     * @param other The handler whose target is taken; it is left empty.
     */
    handler(handler&& other) noexcept : resource_(other.resource_) {
        if (other.operations_ != nullptr) {
            other.operations_->move(other.storage_.data(), storage_.data());
            operations_ = std::exchange(other.operations_, nullptr);
        }
    }

    /**
     * Destroys the target, then takes `other`'s target and allocator.
     *
     * @param other The handler whose target is taken; it is left empty.
     * @return This handler.
     */
    handler& operator=(handler&& other) noexcept {
        if (this != &other) {
            reset();
            resource_ = other.resource_;
            if (other.operations_ != nullptr) {
                other.operations_->move(other.storage_.data(), storage_.data());
                operations_ = std::exchange(other.operations_, nullptr);
            }
        }
        return *this;
    }

    /**
     * Destroys the target, which leaves the handler empty.
     *
     * @return This handler.
     */
    handler& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    /** Not copyable: a handler owns its target, which may not be copyable. */
    handler(const handler&) = delete;

    /** Not copy-assignable: a handler owns its target, which may not be copyable. */
    handler& operator=(const handler&) = delete;

    /** Destroys the target, and gives its storage back to the allocator. */
    ~handler() { reset(); }

    // The parameters are the signature's, as std::function's call operator takes them, and are
    // forwarded to the target as they are.
    /**
     * Calls the target.
     *
     * @param args The arguments, forwarded to the target.
     * @return What the target returns, converted to `R`.
     * @pre The handler is not empty.
     */
    R operator()(Args... args) {  // NOLINT(performance-unnecessary-value-param)
        BOOST_ASSERT(operations_ != nullptr);
        return operations_->invoke(storage_.data(), std::forward<Args>(args)...);
    }

    /**
     * Whether the handler has a target.
     *
     * @return Whether it may be called.
     */
    explicit operator bool() const noexcept { return operations_ != nullptr; }

    /**
     * The allocator the target is allocated with when it does not fit inline.
     *
     * @return The handler's allocator.
     */
    [[nodiscard]] allocator_type get_allocator() const noexcept {
        return allocator_type(resource_);
    }

private:
    static constexpr std::size_t inline_size = 3 * sizeof(void*);

    template <class T>
    static constexpr bool fits_inline = sizeof(T) <= inline_size && alignof(T) <= alignof(void*) &&
                                        std::is_nothrow_move_constructible_v<T>;

    /**
     * What a target's type does, one table per type.
     *
     * Tip: each member has an initializer, else MSVC 14.3 and 14.5 warn (C4268, an error under
     * /WX) that `operations_of`, which sets every member, is filled with zeros.
     */
    struct operations {
        R (*invoke)(unsigned char* storage, Args&&... args) = nullptr;
        void (*move)(unsigned char* from, unsigned char* to) noexcept = nullptr;
        void (*rehome)(unsigned char* from, std::pmr::memory_resource& from_resource,
                       unsigned char* to, std::pmr::memory_resource& to_resource) = nullptr;
        void (*destroy)(unsigned char* storage,
                        std::pmr::memory_resource& resource) noexcept = nullptr;
    };

    /** A target made in `allocator`'s storage; the storage is given back if the construction fails.
     */
    template <class T, class F>
    static T* make(allocator_type allocator, F&& f) {
        class release_unless_built {
        public:
            release_unless_built(allocator_type allocator, T* p) noexcept
                : allocator_(allocator), p_(p) {}

            release_unless_built(const release_unless_built&) = delete;
            release_unless_built& operator=(const release_unless_built&) = delete;

            ~release_unless_built() {
                if (p_ != nullptr) {
                    allocator_.deallocate_object(p_);
                }
            }

            T* built() noexcept { return std::exchange(p_, nullptr); }

        private:
            allocator_type allocator_;
            T* p_;
        };

        T* const p = allocator.template allocate_object<T>();
        release_unless_built guard(allocator, p);
        ::new (static_cast<void*>(p)) T(std::forward<F>(f));
        return guard.built();
    }

    template <class T>
    static T* inline_target(unsigned char* storage) noexcept {
        return std::launder(reinterpret_cast<T*>(storage));
    }

    template <class T>
    static T* allocated_target(unsigned char* storage) noexcept {
        return *std::launder(reinterpret_cast<T**>(storage));
    }

    // A target is destroyed with std::destroy_at: MSVC 14.3 and 14.5 refuse `p->~T()` in these
    // functions when T is a closure type (C2300, "class does not have a destructor called '~T'").
    template <class T>
    static constexpr operations operations_of = [] {
        if constexpr (fits_inline<T>) {
            return operations{
                .invoke = [](unsigned char* storage, Args&&... args) -> R {
                    return static_cast<R>(
                        (*inline_target<T>(storage))(std::forward<Args>(args)...));
                },
                // Each of these functions constructs a target in to's storage with placement new,
                // which readability-non-const-parameter does not count as a write, from the
                // source, which it moves: misc-const-correctness, seeing a target that a move
                // copies (a function pointer), would make the source const and every move a copy.
                .move =
                    [](unsigned char* from,
                       unsigned char* to) noexcept {  // NOLINT(readability-non-const-parameter)
                        T* const source = inline_target<T>(from);  // NOLINT(misc-const-correctness)
                        ::new (static_cast<void*>(to)) T(std::move(*source));
                        std::destroy_at(source);
                    },
                .rehome =
                    [](unsigned char* from, std::pmr::memory_resource& /*from_resource*/,
                       unsigned char* to,  // NOLINT(readability-non-const-parameter)
                       std::pmr::memory_resource& /*to_resource*/) {
                        T* const source = inline_target<T>(from);  // NOLINT(misc-const-correctness)
                        ::new (static_cast<void*>(to)) T(std::move(*source));
                        std::destroy_at(source);
                    },
                .destroy =
                    [](unsigned char* storage, std::pmr::memory_resource& /*resource*/) noexcept {
                        std::destroy_at(inline_target<T>(storage));
                    },
            };
        } else {
            return operations{
                .invoke = [](unsigned char* storage, Args&&... args) -> R {
                    return static_cast<R>(
                        (*allocated_target<T>(storage))(std::forward<Args>(args)...));
                },
                // As above, each constructs in to's storage with placement new.
                .move =
                    [](unsigned char* from,
                       unsigned char* to) noexcept {  // NOLINT(readability-non-const-parameter)
                        ::new (static_cast<void*>(to)) T*(allocated_target<T>(from));
                    },
                .rehome =
                    [](unsigned char* from, std::pmr::memory_resource& from_resource,
                       unsigned char* to,  // NOLINT(readability-non-const-parameter)
                       std::pmr::memory_resource& to_resource) {
                        T* const source = allocated_target<T>(from);
                        ::new (static_cast<void*>(to))
                            T*(make<T>(allocator_type(&to_resource), std::move(*source)));
                        std::destroy_at(source);
                        allocator_type(&from_resource).deallocate_object(source);
                    },
                .destroy =
                    [](unsigned char* storage, std::pmr::memory_resource& resource) noexcept {
                        T* const target = allocated_target<T>(storage);
                        std::destroy_at(target);
                        allocator_type(&resource).deallocate_object(target);
                    },
            };
        }
    }();

    void reset() noexcept {
        if (operations_ != nullptr) {
            std::exchange(operations_, nullptr)->destroy(storage_.data(), *resource_);
        }
    }

    alignas(void*) std::array<unsigned char, inline_size> storage_{};
    const operations* operations_ = nullptr;
    std::pmr::memory_resource* resource_ = std::pmr::get_default_resource();
};

}  // namespace webcpp::trystero

#endif
