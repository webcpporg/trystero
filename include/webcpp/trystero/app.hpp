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
#include <webcpp/trystero/room_error.hpp>
#include <webcpp/trystero/subscription.hpp>

#include <webcpp/asio/any_io_executor.hpp>  // lint-world: names the executor only

#include <boost/assert.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace webcpp::trystero {

class app;

namespace detail {

/** Reaches the state an `app` handle shares with its rooms, for what the handle does not offer. */
struct app_access {
    /** The state of `a`, which must hold one. */
    [[nodiscard]] static app_impl& impl(const app& a);

    /** An app as `app`'s constructor makes it, but whose self id is `self_id` in place of one
     * drawn: the library's own tests name their peers, so that which of two is the lower id is
     * known. `self_id` is at most `protocol::max_peer_id_bytes` of UTF-8, and not empty. */
    [[nodiscard]] static app make(webcpp::asio::any_io_executor ex, backend::factory& factory,
                                  app_config config, std::string_view self_id);
};

}  // namespace detail

/**
 * This program in one app: one peer id, the relays and the configuration its rooms share, as a
 * Trystero JS page's rooms share them; each room has its own connection to each peer, as 0.27.0's
 * rooms do.
 *
 * Every room uses the app's immutable password;
 * different passwords require separate apps with distinct peer ids.
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
    using executor_type = webcpp::asio::any_io_executor;

    /**
     * Makes an app: draws its id, 20 random base62 characters, and keeps the configuration.
     *
     * What no peer could use (an ICE server WebRTC refuses) is set aside, and reported to
     * `on_error`.
     *
     * @param ex The executor of the app and of its rooms.
     * @param factory The backend, which must outlive the app and its rooms.
     * @param config The configuration, copied into `config.memory`.
     * @pre `config.memory` is not null.
     */
    app(executor_type ex, backend::factory& factory, app_config config)
        : app(make(std::move(ex), factory, std::move(config))) {}

    /**
     * Takes the state of the app moved from, which may then only be destroyed or assigned.
     *
     * @param other The app moved from.
     */
    app(app&& other) noexcept = default;

    /**
     * Releases the state it held, then takes the state of the app moved from, which may then only
     * be destroyed or assigned; the rooms of the state released go on.
     *
     * @param other The app moved from.
     * @return This app.
     */
    app& operator=(app&& other) noexcept {
        if (this != &other) {
            if (impl_) {
                impl_->release_user();
            }
            impl_ = std::move(other.impl_);
        }
        return *this;
    }

    /** Not copyable: an app is one peer id. */
    app(const app&) = delete;

    /** Not copy-assignable: an app is one peer id. */
    app& operator=(const app&) = delete;

    /** Releases the state; its rooms go on, and their connections with them. */
    ~app() {
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
     * This peer's id in every room of the app, Trystero's `selfId`: drawn when the app is made, as
     * 0.27.0 has no custom one.
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
     * Sets how the relays of every room of the app connect, Trystero's `setRelayConnectivity`,
     * for this app alone (a process may hold several).
     *
     * `offline` closes every relay socket of the app's rooms and holds them closed, those of a room
     * joined meanwhile too; `online` reconnects at once every relay not connected, as each call
     * does, which then subscribes and announces again; `automatic`, the default, follows the
     * page's `online` and `offline` events, and natively, where there are none, is online: set
     * after `offline`, it reconnects the relays. Unlike 0.27.0, `automatic` does not reconnect the
     * relays when the page becomes visible again: the port does not follow `visibilitychange`.
     *
     * Tip: what the relays carry is how peers find each other; a connection already made goes on
     * while the relays are offline.
     *
     * @param mode The connectivity to set.
     * @return The connectivity now in place, `mode`.
     */
    trystero::relay_connectivity set_relay_connectivity(trystero::relay_connectivity mode) {
        BOOST_ASSERT(impl_);
        return impl_->set_connectivity(mode);
    }

    /**
     * How the relays of every room of the app connect, Trystero's `setRelayConnectivity()`
     * without an argument.
     *
     * @return The connectivity last set, `relay_connectivity::automatic` until one is.
     */
    [[nodiscard]] trystero::relay_connectivity relay_connectivity() const {
        BOOST_ASSERT(impl_);
        return impl_->connectivity();
    }

    /**
     * Adds a listener of each part of the configuration the app set aside, reported as a
     * @ref room_error of `error::invalid_configuration` with the app's id and no room nor peer.
     *
     * The reports are delivered once the caller has returned, each once, to the listeners in
     * place when they are delivered: one added after then gets none.
     *
     * @tparam F A callable with a `const room_error&`, whose views are valid for the call.
     * @param f The listener, kept in the app's resource beside those added before.
     * @param options `once` removes it before its first run; `replay` is ignored.
     * @return Its subscription, which requires the app's resource to outlive it; destroying it
     * leaves the listener.
     */
    template <class F>
        requires std::is_invocable_v<F&, const room_error&>
    subscription on_error(F&& f, listen_options options = {}) {
        BOOST_ASSERT(impl_);
        return impl_->on_error(impl_->bind<void(const room_error&)>(std::forward<F>(f)), options);
    }

    /**
     * Joins a room of the app, Trystero's `joinRoom` for an app already configured: connects to
     * the relays and announces this peer on the room's topic.
     *
     * @param room_id The room's id, Trystero's `roomId`, read during the call.
     * @param options The receive bounds and the resource of the room.
     * @return The room, or `error::already_joined` while a room of that id is joined in the app,
     * as 0.27.0 refuses a second join.
     */
    [[nodiscard]] boost::system::result<room> join(std::string_view room_id,
                                                   room_options options = {}) {
        BOOST_ASSERT(impl_);
        if (const auto existing = impl_->find_room(room_id)) {
            if (!existing->leaving()) {
                return error::already_joined;
            }
            // A rejoin during the farewell ends the old room first, as 0.27.0's joinRoom
            // finalizes a leaving slot's cleanup (strategy.ts:180-182).
            existing->finish_farewell();
        }
        return room(detail::room_impl::make(impl_, options, room_id));
    }

private:
    /** Takes the state `make` made, as its first user. */
    explicit app(std::shared_ptr<detail::app_impl> impl) : impl_(std::move(impl)) {
        impl_->add_user();
    }

    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    static std::shared_ptr<detail::app_impl> make(executor_type ex, backend::factory& factory,
                                                  app_config config,
                                                  std::string_view self_id = {}) {
        // NOLINTEND(performance-unnecessary-value-param)
        std::pmr::memory_resource* const memory = config.memory;
        BOOST_ASSERT_MSG(memory != nullptr, "app_config::memory names no resource");
        return std::allocate_shared<detail::app_impl>(std::pmr::polymorphic_allocator<>(memory),
                                                      std::move(ex), factory, std::move(config),
                                                      self_id);
    }

    friend struct detail::app_access;

    std::shared_ptr<detail::app_impl> impl_;
};

inline detail::app_impl& detail::app_access::impl(const app& a) {
    BOOST_ASSERT(a.impl_);
    return *a.impl_;
}

// NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
inline app detail::app_access::make(webcpp::asio::any_io_executor ex, backend::factory& factory,
                                    app_config config, std::string_view self_id) {
    // NOLINTEND(performance-unnecessary-value-param)
    BOOST_ASSERT(!self_id.empty());
    return app(app::make(std::move(ex), factory, std::move(config), self_id));
}

}  // namespace webcpp::trystero

#endif
