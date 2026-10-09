// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_JSON_RESOURCE_HPP
#define WEBCPP_TRYSTERO_JSON_RESOURCE_HPP

#include <webcpp/trystero/config.hpp>

#include <boost/container/pmr/memory_resource.hpp>

#include <cstddef>
#include <memory_resource>

/**
 * Boost.JSON's memory resource over a `std::pmr` one: what a room's JSON values allocate through.
 *
 * Tip: Boost.JSON's `storage_ptr` holds a `boost::container::pmr::memory_resource`, whose base
 * class is header-only, while `boost::container::pmr`'s global resources are compiled; the adapter
 * needs only the base.
 */
namespace webcpp::trystero {

/** A `boost::container::pmr::memory_resource` that allocates from a `std::pmr::memory_resource`. */
class json_resource final : public boost::container::pmr::memory_resource {
public:
    /** Allocates from `upstream`, which must outlive this resource and every value made in it. */
    explicit json_resource(std::pmr::memory_resource* upstream) noexcept : upstream_(upstream) {}

    /** The resource this one allocates from. */
    [[nodiscard]] std::pmr::memory_resource* upstream() const noexcept { return upstream_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return upstream_->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes, alignment);
    }

    /**
     * Equal to itself alone.
     *
     * Tip: another adapter over the same upstream would do too, but telling one apart from another
     * resource needs RTTI, which a user may build without; an unequal resource costs Boost.JSON a
     * copy where it could have moved.
     */
    [[nodiscard]] bool do_is_equal(
        const boost::container::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
};

}  // namespace webcpp::trystero

#endif
