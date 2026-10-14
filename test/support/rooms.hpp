// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_TEST_ROOMS_HPP
#define WEBCPP_TRYSTERO_TEST_ROOMS_HPP

#include "memory_backend.hpp"

#include "../require.hpp"

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/protocol/nostr.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>
#include <boost/json/parse.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The rooms' shared test helpers: a memory network with two relays, an app over them and a join
// that must succeed, a watcher of a room's relay traffic, a flood of announcements, and the virtual
// clock's own waits. A case of room_test.cpp, connection_test.cpp or shared_test.cpp includes this
// header for them.
namespace webcpp::trystero::test {

using namespace std::chrono_literals;

constexpr const char* relay_a = "wss://relay-a.test";
constexpr const char* relay_b = "wss://relay-b.test";
constexpr const char* app_id = "webcpp-trystero-tests";

/** An app of the tests' app id over the two memory relays, with `self_id`; with
 * WEBCPP_TEST_TRYSTERO_VERBOSE in the environment, its log goes to the standard error. */
inline trystero::app_config app_config(std::string_view self_id) {
    trystero::app_config c;
    c.app_id = app_id;
    c.relay_urls = {relay_a, relay_b};
    c.self_id = self_id;
    if (std::getenv("WEBCPP_TEST_TRYSTERO_VERBOSE") != nullptr) {
        c.log = [id = std::pmr::string(self_id)](std::string_view category, std::string_view text) {
            std::cerr << "  [" << id << "] " << category << ": " << text << "\n";
        };
    }
    return c;
}

/** Joins `room_id`, which must succeed: a failure ends the test. */
inline trystero::room join(trystero::app& a, std::string_view room_id,
                           trystero::room_options options = {}) {
    auto r = a.join(room_id, std::move(options));
    webcpp::test::require(BOOST_TEST(r.has_value()));
    return std::move(*r);
}

/** Runs the io_context until `done` holds or `limit` passes; whether `done` held. */
inline bool run_until(boost::asio::io_context& io, const std::function<bool()>& done,
                      std::chrono::milliseconds limit = 10s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < end) {
        io.run_for(20ms);  // lint-run: the memory relays and the rooms' timers post to it
        if (io.stopped()) {
            io.restart();
        }
    }
    return done();
}

struct fixture {
    boost::asio::io_context io;
    memory_network net{io};

    /** A memory network with two relays; with `virtual_time`, on the virtual clock. */
    explicit fixture(bool virtual_time = false) {
        net.virtual_clock = virtual_time;
        net.relay(relay_a);
        net.relay(relay_b);
    }
};

/** Runs until `done` holds, for at most `limit`: of the virtual clock when the fixture has one,
 * moving it from timer to timer, else of the wall clock. On the virtual clock it fires at most
 * `max_steps` timers, and fails the case by name past them: a timer re-armed without delay would
 * otherwise hold the clock still forever. */
inline bool run_until(fixture& f, const std::function<bool()>& done,
                      std::chrono::milliseconds limit = std::chrono::seconds(10)) {
    if (!f.net.virtual_clock) {
        return run_until(f.io, done, limit);
    }
    const double end = f.net.now_ms() + static_cast<double>(limit.count());
    for (std::size_t steps = 0; !done(); ++steps) {
        if (steps == f.net.max_steps) {
            BOOST_ERROR("the virtual clock fired max_steps timers without moving past them");
            return false;
        }
        if (!f.net.poll_ready()) {
            BOOST_ERROR("the virtual clock exhausted its ready-handler budget");
            return false;
        }
        if (done() || !f.net.step(end)) {
            if (f.net.handlers_exhausted) {
                BOOST_ERROR("the virtual clock exhausted its ready-handler budget");
            }
            break;
        }
    }
    return done();
}

/** Moves the virtual clock by `by`, firing every timer due meanwhile; fails the case by name when
 * the clock stopped at its step bound. Called with milliseconds: an argument of
 * another duration finds std::advance by its namespace, which GCC prefers, as an exact match. */
inline void advance(fixture& f, std::chrono::milliseconds by) {
    if (!f.net.advance(by)) {
        BOOST_ERROR("the virtual clock fired max_steps timers without moving past them");
    }
}

/** A resource that counts the bytes it has out, and the most it had out at once. */
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t in_use = 0;
    std::size_t peak = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        in_use += bytes;
        peak = std::max(peak, in_use);
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        in_use -= bytes;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

/** Announcements of fresh peer ids on a room's topic, signed, through a socket of their own. */
class flooder {
public:
    flooder(fixture& f, std::string_view room) : f_(f), topic_(protocol::root_topic(app_id, room)) {
        socket_ = f.net.connect(relay_a,
                                {
                                    .open = [this] { open_ = true; },
                                    .message =
                                        [this](std::string_view text) {
                                            if (on_text) {
                                                on_text(text);
                                            }
                                        },
                                    .close = {},
                                },
                                std::pmr::get_default_resource());
        webcpp::test::require(BOOST_TEST(run_until(f, [this] { return open_; })));
        std::array<std::uint8_t, 32> secret{};
        secret.fill(9);
        signer_.emplace(*protocol::nostr::signer::from_secret(secret));
    }

    /** What the relay sends the flooder: the events of the topics it watches. */
    std::function<void(std::string_view)> on_text;

    /** Subscribes to a topic's events, which then reach `on_text`. */
    void watch(std::string_view topic) {
        const std::array<std::pmr::string, 1> topics{std::pmr::string(topic)};
        socket_->send(
            protocol::nostr::subscribe_message("watch" + std::to_string(next_++), topics, 0));
    }

    /** Publishes a signal on a topic, in any peer's name. */
    void publish(std::string_view topic, const protocol::signal_message& m) {
        const std::array<std::uint8_t, 32> aux{};
        socket_->send(protocol::nostr::event_message(*signer_, f_.net.unix_seconds(), topic,
                                                     protocol::write_signal(m), aux));
    }

    /** `plain` sealed under the room key of `room`, with no password, as the memory sealer seals.
     */
    static std::string sealed(std::string_view room, std::string_view plain) {
        const auto key = protocol::seal_key("", app_id, room);
        return std::string(protocol::format_sealed(
            std::span(key).first(16),
            std::span(reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size())));
    }

    /** Announces one id `times` times. */
    void announce_as(std::string_view id, int times) {
        const std::array<std::uint8_t, 32> aux{};
        for (int i = 0; i < times; ++i) {
            const auto nonce = std::to_string(next_++);
            const auto content = protocol::write_signal({.peer_id = id}, nonce);
            socket_->send(protocol::nostr::event_message(*signer_, f_.net.unix_seconds(), topic_,
                                                         content, aux));
        }
    }

    /** Announces `count` ids, each `prefix` and a number. */
    void announce(std::size_t count, std::string_view prefix) {
        const std::array<std::uint8_t, 32> aux{};
        for (std::size_t i = 0; i < count; ++i) {
            std::pmr::string id(prefix);
            id += std::to_string(next_++);
            const auto content = protocol::write_signal({.peer_id = id});
            socket_->send(protocol::nostr::event_message(*signer_, f_.net.unix_seconds(), topic_,
                                                         content, aux));
        }
    }

private:
    fixture& f_;
    std::pmr::string topic_;
    std::shared_ptr<backend::websocket> socket_;
    std::optional<protocol::nostr::signer> signer_;
    bool open_ = false;
    std::size_t next_ = 0;
};

/** A relay's event as a watcher reads it: its topic and its signal. */
struct relayed {
    std::string topic;
    std::string peer;
    std::string offer_id;
    std::string offer;
    std::string answer;
    bool announcement = false;
};

/** The topic and the signal of a relay's text, when it is an event that carries one. */
inline std::optional<relayed> relayed_signal(std::string_view text) {
    boost::system::error_code ec;
    const auto document = boost::json::parse(text, ec);
    const auto message = ec ? std::nullopt : protocol::nostr::read_relay_message(document);
    const auto* event = message ? std::get_if<protocol::nostr::event>(&*message) : nullptr;
    if (event == nullptr) {
        return std::nullopt;
    }
    const auto content = boost::json::parse(event->content, ec);
    const auto signal = ec ? std::nullopt : protocol::read_signal(content);
    if (!signal) {
        return std::nullopt;
    }
    return relayed{
        .topic = std::string(event->topic),
        .peer = std::string(signal->peer_id),
        .offer_id = std::string(signal->offer_id),
        .offer = std::string(signal->offer),
        .answer = std::string(signal->answer),
        .announcement = signal->is_announcement(),
    };
}

}  // namespace webcpp::trystero::test

#endif
