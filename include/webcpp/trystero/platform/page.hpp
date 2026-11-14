// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_PLATFORM_PAGE_HPP
#define WEBCPP_TRYSTERO_PLATFORM_PAGE_HPP

#include <webcpp/trystero/config.hpp>

#ifndef WEBCPP_TARGET_EMSCRIPTEN
#error "<webcpp/trystero/platform/page.hpp> is a page's: it builds on emscripten only"
#endif

#include <webcpp/trystero/detail/post.hpp>

#include <webcpp/asio/any_io_executor.hpp>    // lint-world: names the executor only
#include <webcpp/asio/browser/bridge.hpp>     // lint-world: the page's calls
#include <webcpp/asio/execution/context.hpp>  // lint-world: the bridge of the executor's context
#include <webcpp/asio/query.hpp>              // lint-world: the bridge of the executor's context

#include <emscripten/em_asm.h>
#include <emscripten/val.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <utility>

namespace webcpp::trystero::platform {

namespace detail {

// clang-format off
/** The JavaScript string of `size` bytes of UTF-8 at `data`, its NUL characters included. */
inline emscripten::EM_VAL js_text(const char* data, std::size_t size) {
    return reinterpret_cast<emscripten::EM_VAL>(EM_ASM_PTR(({
    const data = $0 >>> 0;
    const size = $1 >>> 0;

    return Emval.toHandle(UTF8ToString(data, size, true));
    }), data, size));
}
// clang-format on

/** The bridge of the io_context an executor runs on, which delivers the page's calls to it. */
inline webcpp::asio::browser::bridge& bridge_of(const webcpp::asio::any_io_executor& ex) {
    return webcpp::asio::use_service<webcpp::asio::browser::bridge>(
        webcpp::asio::query(ex, webcpp::asio::execution::context));
}

}  // namespace detail

/**
 * A JavaScript string of UTF-8 text, decoded straight from the WebAssembly memory the view points
 * into.
 *
 * No `std::string` is made to carry it across, as `emscripten::val(std::string)` would; a NUL
 * character is kept, and a byte that is not UTF-8 becomes U+FFFD, as `TextDecoder` makes it.
 *
 * @param utf8 The text, read during the call.
 * @return The JavaScript string.
 */
inline emscripten::val js_string(std::string_view utf8) {
    return emscripten::val::take_ownership(detail::js_text(utf8.data(), utf8.size()));
}

/**
 * A function the page calls into C++ with, made by webcpp::asio's bridge, each call of which posts
 * `f` with the value the page passed to the executor's io_context.
 *
 * It is how a page reaches a program whose `main` runs its io_context: the call returns at once,
 * having run nothing, and `f` runs when the run family does, never inside the page's event, in
 * either suspension variant. An embind function would run its work inside the event, and under
 * Asyncify, while `main` is suspended in its run, returns a promise that never settles, and aborts
 * a debug build at its second call (measured with Emscripten 6.0.11). A call made once the
 * page_call is destroyed, or once its work is posted and the page_call destroyed before it runs,
 * reaches nothing.
 *
 * @tparam F A callable of one `const emscripten::val&`, the value the page passed.
 * @see "The program runs its io_context", in the guide.
 */
template <class F>
// The bridge indexes the call by its registration, a private base: its one owner is the call.
class page_call final : private webcpp::asio::browser::intrusive_registration {
public:
    /**
     * Makes the function, registered with the bridge of the io_context of `ex`.
     *
     * @param ex The executor `f` is posted to, an io_context's of webcpp::asio, which must outlive
     * the page_call or be destroyed first, which ends it.
     * @param f What each call runs, on the executor.
     * @param memory The resource `f` and each posted call are allocated in.
     */
    page_call(webcpp::asio::any_io_executor ex, F f,
              std::pmr::memory_resource* memory = std::pmr::get_default_resource())
        : ex_(std::move(ex)),
          memory_(memory),
          f_(std::allocate_shared<F>(std::pmr::polymorphic_allocator<>(memory), std::move(f))),
          bridge_(&detail::bridge_of(ex_)),
          key_(bridge_->add(*this)) {}

    /** Not copyable: the bridge delivers to it by its address. */
    page_call(const page_call&) = delete;

    /** Not copy-assignable: the bridge delivers to it by its address. */
    page_call& operator=(const page_call&) = delete;

    /** Destroys the function: a call from now on, or one posted and not yet run, reaches nothing.
     */
    ~page_call() {
        if (bridge_ != nullptr) {
            bridge_->remove(key_);
        }
    }

    /**
     * The JavaScript function, of one argument, which the program hands to the page.
     *
     * @return The function; undefined once the io_context is destroyed.
     */
    [[nodiscard]] emscripten::val function() const {
        return bridge_ != nullptr ? bridge_->function(key_) : emscripten::val::undefined();
    }

private:
    bool deliver(const emscripten::val& argument) override {
        trystero::detail::post(ex_, memory_, [weak = std::weak_ptr<F>(f_), argument] {
            if (const auto f = weak.lock()) {
                (*f)(argument);
            }
        });
        return false;
    }

    void shutdown() noexcept override { bridge_ = nullptr; }

    webcpp::asio::any_io_executor ex_;
    std::pmr::memory_resource* memory_;
    std::shared_ptr<F> f_;
    webcpp::asio::browser::bridge* bridge_;
    std::uint64_t key_;
};

}  // namespace webcpp::trystero::platform

#endif
