// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the lifetimes of a page's calls into C++, `platform::page_call`, under node, in a program
// whose main runs its io_context under a work guard, as a page's does:
//
// - A call's handler is given the page's value as it is when the handler runs: the page passes an
//   object and changes it before the run resumes, and the handler sees it changed.
// - A handler reads a number as a double and checks its range before it makes it an integer, as
//   page.hpp says: 1e10, -1e10, 2.5, NaN, a string and undefined are no 32-bit integer, and none
//   throws. Before the fix the test read `isNumber() ? as<std::int32_t>()`, which for 1e10 throws a
//   TypeError in a build with assertions.
// - A JavaScript function the page kept after its page_call was destroyed reaches nothing and
//   breaks nothing: the page calls it from a timer, while the run is suspended, and the call
//   returns undefined and runs nothing, and the page's next call, of another page_call, reaches
//   C++.
// - A page_call that outlives its io_context is harmless: a call the page made before the
//   io_context was destroyed is destroyed with it, unrun, and gives its memory back to the
//   page_call's resource; then `function()` returns undefined, the function the page kept reaches
//   nothing, and the page_call is destroyed after the io_context.
//
// main ends with the errors counted once the run returns; a step that does not come within 10 s
// fails by name.

#include <webcpp/trystero/platform/page.hpp>

#include <webcpp/asio/executor_work_guard.hpp>
#include <webcpp/asio/io_context.hpp>

#include <emscripten/em_asm.h>
#include <emscripten/val.h>
#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using emscripten::val;
using namespace std::chrono_literals;

namespace {

/** A resource over the default one that counts the bytes it gave and has not taken back. */
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t outstanding = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        outstanding += bytes;
        return std::pmr::get_default_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        outstanding -= bytes;
        std::pmr::get_default_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct program;

/** What a page_call of the test runs: a step of the program, given the page's value. */
struct step {
    program* p;
    void (*run)(program&, const val&);

    void operator()(const val& argument) const { run(*p, argument); }
};

using call = trystero::platform::page_call<step>;

/** What the page keeps alive: the io_context, its guard, and its calls. */
struct program {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    std::optional<call> doomed;
    std::optional<call> next;
    std::size_t stage = 0;
    std::int32_t seen_n = 0;
    std::size_t doomed_calls = 0;
    std::size_t orphan_calls = 0;
    bool judged = false;
};

/**
 * A page_call on an io_context destroyed before it: the call the page made meanwhile is destroyed
 * unrun, with its memory given back, and nothing reaches C++ from then on.
 */
void outlive_the_io_context(program& p) {
    // Declared first, so that it outlives both the io_context and the page_call.
    counting_resource memory;
    std::optional<call> orphan;
    std::optional<asio::io_context> other(std::in_place);
    orphan.emplace(
        other->get_executor(),
        step{.p = &p, .run = [](program& q, const val& /*argument*/) { ++q.orphan_calls; }},
        &memory);
    const std::size_t made = memory.outstanding;
    const val kept = orphan->function();
    // Posted to the other io_context, which never runs.
    BOOST_TEST(kept(1).isUndefined());
    BOOST_TEST_GT(memory.outstanding, made);
    other.reset();
    BOOST_TEST_EQ(memory.outstanding, made);
    BOOST_TEST(orphan->function().isUndefined());
    BOOST_TEST(kept(2).isUndefined());
    orphan.reset();
    BOOST_TEST_EQ(memory.outstanding, 0U);
    BOOST_TEST_EQ(p.orphan_calls, 0U);
}

/**
 * The page's value as a 32-bit integer, or nothing when it is no number in that range: its type is
 * checked first, and its range as a double, since embind's `as<std::int32_t>()` of a number beyond
 * the range, as 1e10, throws a TypeError in a build with assertions.
 */
std::optional<std::int32_t> read_int32(const val& value) {
    if (!value.isNumber()) {
        return std::nullopt;
    }
    const double number = value.as<double>();
    // NaN fails every comparison, and so the first.
    if (!(number >= std::numeric_limits<std::int32_t>::min()) ||
        number > std::numeric_limits<std::int32_t>::max() || number != std::trunc(number)) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(number);
}

/** What a page_call's handler reads of a number, whatever the page passed. */
void numbers_are_read_within_their_range() {
    BOOST_TEST(read_int32(val(2)) == std::optional<std::int32_t>(2));
    BOOST_TEST(read_int32(val(-2147483648.0)) ==
               std::optional<std::int32_t>(std::numeric_limits<std::int32_t>::min()));
    BOOST_TEST(!read_int32(val(1e10)).has_value());
    BOOST_TEST(!read_int32(val(-1e10)).has_value());
    BOOST_TEST(!read_int32(val(2.5)).has_value());
    BOOST_TEST(!read_int32(val::global("NaN")).has_value());
    BOOST_TEST(!read_int32(val("2")).has_value());
    BOOST_TEST(!read_int32(val::undefined()).has_value());
}

/**
 * Each call of the page's `next`: reads the page's object and destroys the doomed page_call, then
 * judges what followed.
 */
void advance(program& p, const val& argument) {
    if (p.stage++ == 0) {
        // The page's value is checked before it is read, as a page_call's handler does.
        if (!argument.isNull() && !argument.isUndefined()) {
            p.seen_n = read_int32(argument["n"]).value_or(-1);
        }
        p.doomed.reset();
        // The page calls the function it kept, then the next.
        EM_ASM({
            setTimeout(
                function() {
                    globalThis.keptReturned = globalThis.kept("late");
                    globalThis.next(null);
                },
                0);
        });
        return;
    }
    BOOST_TEST_EQ(p.seen_n, 2);
    BOOST_TEST(val::global("keptReturned").isUndefined());
    BOOST_TEST_EQ(p.doomed_calls, 0U);
    outlive_the_io_context(p);
    p.judged = true;
    p.guard.reset();
}

}  // namespace

int main() {
    numbers_are_read_within_their_range();
    program p;
    p.doomed.emplace(
        p.io.get_executor(),
        step{.p = &p, .run = [](program& q, const val& /*argument*/) { ++q.doomed_calls; }});
    p.next.emplace(p.io.get_executor(), step{.p = &p, .run = advance});
    // clang-format off
    EM_ASM(({
        globalThis.kept = Emval.toValue($0);
        globalThis.next = Emval.toValue($1);
        globalThis.keptReturned = "not called";
        setTimeout(() => {
            const value = {n: 1};
            globalThis.next(value);
            value.n = 2;
        }, 0);
    }), p.doomed->function().as_handle(), p.next->function().as_handle());
    // clang-format on
    p.io.run_for(10s);  // lint-run: the page's program, held by its work guard until the verdict
    if (!p.judged) {
        BOOST_ERROR("the page's calls stopped before the verdict");
    }
    return webcpp::test::exit_status(boost::report_errors());
}
