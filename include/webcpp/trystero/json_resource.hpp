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

/**
 * A `boost::container::pmr::memory_resource` that allocates from a `std::pmr::memory_resource`.
 *
 * A room makes one over `room_config::memory`, as the JSON storage every JSON value of the room is
 * allocated in: what it parses from a relay, a signal or the handshake, and the
 * `boost::json::value` an action's handler receives. The storage is counted, so a value a handler
 * keeps keeps it alive; Boost.JSON allocates that count with `operator new`, once per room.
 *
 * @see "Allocation", in the guide.
 */
class json_resource final : public boost::container::pmr::memory_resource {
public:
    /**
     * Constructs a resource that allocates from `upstream`.
     *
     * @param upstream The resource every allocation goes to; it must outlive this resource and
     * every value allocated in it.
     */
    explicit json_resource(std::pmr::memory_resource* upstream) noexcept : upstream_(upstream) {}

    /**
     * The resource this one allocates from.
     *
     * @return The `upstream` it was constructed with.
     */
    [[nodiscard]] std::pmr::memory_resource* upstream() const noexcept { return upstream_; }

protected:
    /**
     * Allocates from the upstream resource.
     *
     * @param bytes The size.
     * @param alignment The alignment.
     * @return The storage, from the upstream resource.
     */
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return upstream_->allocate(bytes, alignment);
    }

    /**
     * Gives storage back to the upstream resource.
     *
     * @param pointer The storage, which `do_allocate` returned.
     * @param bytes The size it was allocated with.
     * @param alignment The alignment it was allocated with.
     */
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes, alignment);
    }

    /**
     * Whether `other` is this resource: it is equal to itself alone.
     *
     * Tip: another adapter over the same upstream would do too, but telling one apart from another
     * resource needs RTTI, which a user may build without; an unequal resource costs Boost.JSON a
     * copy where it could have moved.
     *
     * @param other The resource to compare with.
     * @return Whether `other` is this resource.
     */
    [[nodiscard]] bool do_is_equal(
        const boost::container::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    std::pmr::memory_resource* upstream_;
};

}  // namespace webcpp::trystero

#endif
