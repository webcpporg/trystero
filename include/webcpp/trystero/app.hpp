// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_APP_HPP
#define WEBCPP_TRYSTERO_APP_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/app_impl.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/options.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/any_io_executor.hpp>  // lint-world: names the executor only
#include <boost/assert.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace webcpp::trystero {

/**
 * This program in one app: one peer id, and one connection per remote peer, shared by the rooms it
 * joins, as a Trystero JS page shares them.
 *
 * Tip: the `app` and every room of it share one state; destroying the `app` while rooms remain
 * changes nothing for them. Every allocation of the app goes through `app_config::memory`, which
 * must outlive the app, its rooms and every operation they leave pending.
 *
 * @see "Rooms and actions", in the guide.
 */
class app {
public:
    /** The executor every handler and completion of the app and its rooms runs on. */
    using executor_type = boost::asio::any_io_executor;

    /** The most rooms an app holds at once: a peer drops a connection that advertises more
     * (Trystero's literal 64, C/shared-peer.mjs:457). */
    static constexpr std::size_t max_rooms = 64;

    /**
     * Makes an app: draws its id unless the configuration names one, and keeps the configuration.
     *
     * What no peer could use (an id longer than any peer accepts, a negative idle time, an ICE
     * server WebRTC refuses) is set aside, and reported to `on_error`.
     *
     * @param ex The executor of the app and of its rooms.
     * @param factory The backend, which must outlive the app and its rooms.
     * @param config The configuration, copied into `config.memory`.
     * @pre `config.memory` is not null.
     */
    app(executor_type ex, backend::factory& factory, app_config config)
        : impl_(make(std::move(ex), factory, std::move(config))) {
        impl_->add_user();
    }

    /**
     * Takes the state of the app moved from, which may then only be destroyed or assigned.
     *
     * @param other The app moved from.
     */
    app(app&& other) noexcept = default;

    // Releasing the last user ends every connection, which logs in the app's resource: a resource
    // that fails then ends the program, as in the destructor.
    /**
     * Releases the state it held, then takes the state of the app moved from, which may then only
     * be destroyed or assigned; the rooms of the state released go on.
     *
     * @param other The app moved from.
     * @return This app.
     */
    app& operator=(app&& other) noexcept {  // NOLINT(bugprone-exception-escape)
        if (this != &other) {
            if (impl_) {
                impl_->release_user();
            }
            impl_ = std::move(other.impl_);
        }
        return *this;
    }

    /** Not copyable: an app is one peer id and its connections. */
    app(const app&) = delete;

    /** Not copy-assignable: an app is one peer id and its connections. */
    app& operator=(const app&) = delete;

    // Releasing the last user ends every connection, which logs in the app's resource: a resource
    // that fails then ends the program, as in any destructor.
    /** Releases the state; its rooms go on, and their connections with them. */
    ~app() {  // NOLINT(bugprone-exception-escape)
        if (impl_) {
            impl_->release_user();
        }
    }

    /**
     * The executor of the app and of its rooms.
     *
     * @return The executor every handler and completion of the app and its rooms runs on.
     */
    [[nodiscard]] const executor_type& get_executor() const {
        BOOST_ASSERT(impl_);
        return impl_->get_executor();
    }

    /**
     * The application's id, Trystero's `appId`.
     *
     * @return The id, valid as long as the app.
     */
    [[nodiscard]] std::string_view app_id() const {
        BOOST_ASSERT(impl_);
        return impl_->app_id();
    }

    /**
     * This peer's id in every room of the app, Trystero's `selfId`.
     *
     * @return The id, valid as long as the app.
     */
    [[nodiscard]] std::string_view self_id() const {
        BOOST_ASSERT(impl_);
        return impl_->self_id();
    }

    /**
     * The Nostr relays every room of the app uses, derived from the app id unless the
     * configuration named them.
     *
     * @return The relays' URLs, valid as long as the app.
     */
    [[nodiscard]] std::span<const std::pmr::string> relay_urls() const {
        BOOST_ASSERT(impl_);
        return impl_->relay_urls();
    }

    /**
     * Sets what is called with each part of the configuration the app set aside, with an empty
     * peer id and `error::invalid_configuration`; the reports made before it is set are posted to
     * it, once the caller has returned.
     *
     * @tparam F A callable with the peer's id, the @ref error and a description, the views valid
     * for the call.
     * @param f The handler, kept in the app's resource; it replaces the one set before.
     */
    template <class F>
        requires std::is_invocable_v<F&, std::string_view, boost::system::error_code,
                                     std::string_view>
    void on_error(F&& f) {
        BOOST_ASSERT(impl_);
        impl_->on_error(
            impl_->bind<void(std::string_view, boost::system::error_code, std::string_view)>(
                std::forward<F>(f)));
    }

    /**
     * Joins a room of the app, Trystero's `joinRoom` for an app already configured: connects to
     * the relays and announces this peer on the room's topic.
     *
     * @param room_id The room's id, Trystero's `roomId`, read during the call.
     * @param options The password, the handshake's timeout and the receive bound of the room.
     * @return The room, or `error::already_joined` while a room of that id is joined in the app,
     * or `error::too_many_rooms` when `max_rooms` rooms count.
     */
    boost::system::result<room> join(std::string_view room_id, room_options options = {}) {
        BOOST_ASSERT(impl_);
        if (impl_->find_room(room_id)) {
            return error::already_joined;
        }
        if (impl_->counted_rooms() >= max_rooms) {
            return error::too_many_rooms;
        }
        return room(detail::room_impl::make(impl_, std::move(options), room_id));
    }

private:
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    static std::shared_ptr<detail::app_impl> make(executor_type ex, backend::factory& factory,
                                                  app_config config) {
        // NOLINTEND(performance-unnecessary-value-param)
        std::pmr::memory_resource* const memory = config.memory;
        BOOST_ASSERT_MSG(memory != nullptr, "app_config::memory names no resource");
        return std::allocate_shared<detail::app_impl>(std::pmr::polymorphic_allocator<>(memory),
                                                      std::move(ex), factory, std::move(config));
    }

    std::shared_ptr<detail::app_impl> impl_;
};

}  // namespace webcpp::trystero

#endif
