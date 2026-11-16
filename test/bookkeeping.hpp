// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// What a standard container allocates for itself through its allocator, rebound, whatever it
// holds: the MSVC STL's checked iterators (_ITERATOR_DEBUG_LEVEL above 0, the default of a debug
// build) allocate a proxy for each container when it is constructed and when it is moved; the
// other standard libraries allocate nothing so. A test that counts allocations exactly leaves such
// a block out, as document's tests do: it is no state of what the test measures. Measured once, on
// an empty std::pmr::vector.

#ifndef WEBCPP_TEST_TRYSTERO_BOOKKEEPING_HPP
#define WEBCPP_TEST_TRYSTERO_BOOKKEEPING_HPP

#include <cstddef>
#include <memory_resource>
#include <vector>

namespace webcpp::test {

/** The block one standard container allocates for itself; its size is 0 when there is none. */
struct container_bookkeeping {
    /** The size of the block, or 0. */
    std::size_t bytes = 0;
    /** The alignment of the block, or 0. */
    std::size_t alignment = 0;
};

/** What a standard container allocates for itself on this standard library. */
inline const container_bookkeeping& bookkeeping() {
    /** A resource that records the first block asked of it. */
    class measuring final : public std::pmr::memory_resource {
    public:
        container_bookkeeping seen;

    private:
        void* do_allocate(std::size_t bytes, std::size_t alignment) override {
            if (seen.bytes == 0) {
                seen = {.bytes = bytes, .alignment = alignment};
            }
            return std::pmr::new_delete_resource()->allocate(bytes, alignment);
        }

        void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
            std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
        }

        [[nodiscard]] bool do_is_equal(
            const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    };

    static const container_bookkeeping measured = [] {
        measuring resource;
        {
            [[maybe_unused]] const std::pmr::vector<int> empty(&resource);
        }
        return resource.seen;
    }();
    return measured;
}

/** Whether a block of `bytes` at `alignment` may be a standard container's own; never on a
 * standard library that allocates none. */
inline bool is_bookkeeping(std::size_t bytes, std::size_t alignment) {
    const container_bookkeeping& own = bookkeeping();
    return own.bytes != 0 && bytes == own.bytes && alignment == own.alignment;
}

}  // namespace webcpp::test

#endif  // WEBCPP_TEST_TRYSTERO_BOOKKEEPING_HPP
