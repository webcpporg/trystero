// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_NOSTR_STRATEGY_HPP
#define WEBCPP_TRYSTERO_DETAIL_NOSTR_STRATEGY_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/clock.hpp>
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/detail/random_id.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/nostr.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <webcpp/document/parse.hpp>

#include <boost/assert.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/hash2/get_integral_result.hpp>
#include <boost/hash2/siphash.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/**
 * The ids of the newest Nostr events a strategy received, so that it acts on each event once: a
 * relay may repeat one, and every relay of the room carries it.
 *
 * The strategy gives it only an id it checked to be its event's own
 * (`protocol::nostr::has_own_id`), so that a relay cannot hide an event behind a forged one under
 * its id: an event with any other id is acted on every time, as Trystero, which remembers none,
 * acts on every event. Only an id NIP-01 defines, 64 lowercase hexadecimal digits, is remembered,
 * so that what a relay makes the room hold is `capacity` ids of 64 bytes.
 *
 * Tip: an id is whatever a relay sends, so the index hashes it with SipHash under a key drawn when
 * the strategy is made. The standard library's hashes are unseeded: ids a relay chose to fall in
 * one bucket of a `std::unordered_set` would make each of its lookups walk all of them.
 */
class recent_event_ids {
public:
    /** The size of the hash's key, SipHash's 128 bits. */
    static constexpr std::size_t key_size = 16;

    /** The size of an id the index remembers: NIP-01's, the SHA-256 of the event in hexadecimal. */
    static constexpr std::size_t id_size = 64;

    /** SipHash of an id under a key, which Boost.Unordered uses as it is. */
    class keyed_hash {
    public:
        /** SipHash's output is uniform, so Boost.Unordered takes it without mixing it again. */
        using is_avalanching = std::true_type;

        /** Hashes under `key`. */
        explicit keyed_hash(std::span<const std::uint8_t, key_size> key)
            : hasher_(key.data(), key.size()) {}

        /** The hash of `id`. */
        std::size_t operator()(std::string_view id) const noexcept {
            auto h = hasher_;
            h.update(id.data(), id.size());
            return boost::hash2::get_integral_result<std::size_t>(h);
        }

    private:
        boost::hash2::siphash_64 hasher_;
    };

    /** The index of the remembered ids, views of the strings the order owns. */
    using index_type = boost::unordered_flat_set<std::string_view, keyed_hash, std::equal_to<>,
                                                 std::pmr::polymorphic_allocator<std::string_view>>;

    /** Remembers nothing yet: up to `capacity` ids, hashed under `key`, allocated in `memory`. */
    recent_event_ids(std::size_t capacity, std::span<const std::uint8_t, key_size> key,
                     std::pmr::memory_resource* memory)
        : index_(0, keyed_hash(key), std::equal_to<>(), memory),
          order_(memory),
          capacity_(capacity) {}

    /** Whether `id` is new; a new one is remembered in place of the oldest beyond `capacity`, and
     * one that is not NIP-01's is always new and never remembered. */
    bool remember(std::string_view id) {
        if (!protocol::nostr::detail::is_nip01_hex(id)) {
            return true;
        }
        if (index_.contains(id)) {
            return false;
        }
        // A deque never moves what it holds, so the view stays valid until pop_front.
        const std::string_view stored = order_.emplace_back(id);
        index_.insert(stored);
        if (order_.size() > capacity_) {
            index_.erase(order_.front());
            order_.pop_front();
        }
        return true;
    }

    /** How many ids are remembered. */
    [[nodiscard]] std::size_t size() const { return order_.size(); }

    /** The index's hash, under its key. */
    [[nodiscard]] keyed_hash hash_function() const { return index_.hash_function(); }

private:
    index_type index_;
    std::pmr::deque<std::pmr::string> order_;
    std::size_t capacity_;
};

/**
 * Trystero's Nostr strategy, as 0.27.0's `\@trystero-p2p/nostr` runs it: subscriptions on each
 * relay to the room's topics, announcements on the root topic at Trystero's cadence, and signals
 * published on the relay that carried the message they answer, every event signed with the room's
 * own key.
 *
 * | Rule               | Value                                                                    |
 * | signer             | one per room, made on its first event, dropped when the room stops       |
 * | announce cadence   | at once, then after 233, 533, 1333 and 5333 ms, then every 60 s; each    |
 * |                    | announcement with a fresh nonce; one the relay cannot take now is        |
 * |                    | skipped, and the cadence goes on                                         |
 * | subscription       | at most 250 topics per REQ, since a minute ago                           |
 * | relay health       | healthy; cooldown 60 s, doubling to 15 min, on "rate-limited:"; then     |
 * |                    | probation, which an accepted event ends; retired on "blocked:",          |
 * |                    | "restricted:", "auth-required:" or "pow:"                                |
 * | slow subscription  | an EOSE 1 s or more after the REQ replays the announcements              |
 * | closed subscription| asked again after 5333 ms, doubling to 60 s, or after the cooldown       |
 * | reconnect          | after a random share of 3.3 s, doubling to 60 s; back to 3.3 s once a    |
 * |                    | connection stayed open for 60 s                                          |
 * | reopened relay     | its subscriptions asked again and the announcement replayed once, or     |
 * |                    | both after its cooldown; the cadence is not started over                 |
 * | every relay failed | reported once, when each relay's first connection closed before it      |
 * |                    | opened and none opened yet, as 0.27.0 reports `relay-failed`            |
 * | offline            | every socket closed, no reconnection; the cadence goes on, skipping      |
 * | online             | each relay not connected connects at once, its delay bound back to 3.3 s |
 *
 * Tip: the bound of the reconnection delay comes back down only after a connection that lasted,
 * so that a relay that closes us at once after each open, as one that sends a text longer than we
 * read does, is tried less and less often rather than every 1.7 s, on average, forever.
 */
class nostr_strategy : public std::enable_shared_from_this<nostr_strategy> {
public:
    /** How many event ids the strategy remembers, the newest, to drop one a relay repeats or
     * another relay carries too. */
    static constexpr std::size_t remembered_events = 4096;

    /** A message on one of the subscribed topics, with the index of the relay that carried it;
     * views valid for the call. */
    using message_handler =
        handler<void(std::size_t relay, std::string_view topic, std::string_view content)>;

    /** The text of an announcement, given the nonce the strategy drew for it, as Trystero adds a
     * `nonce` to the announcement's payload each time it publishes it. */
    using announcement_handler = handler<std::pmr::string(std::string_view nonce)>;

    /** Every allocation goes through `memory`, the parsed relay messages' documents included;
     * `all_failed`, when set, is called once every relay failed (see the table). */
    nostr_strategy(backend::factory& factory, std::span<const std::pmr::string> urls,
                   std::pmr::vector<std::pmr::string> topics, std::string_view announce_topic,
                   announcement_handler announcement, message_handler on_message, logger log,
                   std::pmr::memory_resource* memory, handler<void()> all_failed = {})
        : factory_(factory),
          memory_(memory),
          topics_(std::move(topics), memory),
          announce_topic_(announce_topic, memory),
          announcement_(std::move(announcement)),
          on_message_(std::move(on_message)),
          all_failed_(std::move(all_failed)),
          log_(std::move(log)),
          relays_(memory),
          seen_(remembered_events, draw_key(factory), memory) {
        relays_.reserve(urls.size());
        for (const auto& url : urls) {
            relays_.emplace_back(url, memory);
        }
    }

    /** Connects to every relay, unless they are held closed; each subscribes and announces once
     * it opens. */
    void start() {
        started_ = true;
        for (std::size_t i = 0; i < relays_.size(); ++i) {
            connect(i);
        }
    }

    /**
     * Closes every relay's socket and holds it closed, reconnecting none until @ref reconnect,
     * 0.27.0's `holdRelaySockets`: a socket closed so is no failure, and its close is not
     * reported. The announce cadence goes on, each announcement skipped as on a closed relay.
     */
    void hold() {
        if (stopped_ || std::exchange(held_, true)) {
            return;
        }
        log_("nostr", "holding the relays closed");
        for (auto& r : relays_) {
            ++r.attempt;  // what the closed socket still reports reaches nothing
            r.reconnect_timer.reset();
            if (r.subscription_timer) {
                r.subscription_timer->cancel();
            }
            r.subscription_retry_pending = false;
            r.pending.clear();
            r.subscribed = sync::unsynced;
            r.open = false;
            if (r.socket) {
                r.socket->close();
                r.socket.reset();
            }
        }
    }

    /**
     * Connects at once every relay that has no socket, whether held closed or waiting to
     * reconnect, each delay bound back at its least, 0.27.0's `reconnectRelaySockets`; a relay
     * that opened before subscribes and announces again once it reopens.
     */
    void reconnect() {
        if (stopped_) {
            return;
        }
        held_ = false;
        if (!started_) {
            return;  // the room's keys are not there yet: start() connects
        }
        for (std::size_t i = 0; i < relays_.size(); ++i) {
            auto& r = relays_[i];
            r.retry = first_retry;
            if (!r.socket) {
                r.reconnect_timer.reset();
                connect(i);
            }
        }
    }

    /** Publishes a signal to a peer's topic on one relay, unless that relay is closed, cooling
     * down or retired; Trystero drops it too. */
    void publish(std::size_t index, std::string_view topic, std::string_view content) {
        if (index >= relays_.size() || blocked(relays_[index])) {
            return;
        }
        send_event(relays_[index], topic, content);
    }

    /** Starts the announce cadence over on each relay where it started, as Trystero's
     * `requeueAnnounce` does when a peer leaves. */
    void restart_announcements() {
        for (std::size_t i = 0; i < relays_.size(); ++i) {
            if (relays_[i].announced) {
                relays_[i].announce_attempt = 0;
                announce(i);
            }
        }
    }

    /** How many relays the strategy uses. */
    std::size_t relay_count() const { return relays_.size(); }

    /** The ids of the events it acted on, which it remembers to drop their repeats. */
    [[nodiscard]] const recent_event_ids& seen() const { return seen_; }

    /** The room's signer, which the strategy makes at its first event and drops when it stops;
     * null before the one and after the other. */
    [[nodiscard]] const protocol::nostr::signer* current_signer() const {
        return signer_ ? &*signer_ : nullptr;
    }

    /** Stops announcing and reconnecting, closes each subscription and each connection, and drops
     * the room's signer. */
    void stop() {
        stopped_ = true;
        for (auto& r : relays_) {
            r.announce_timer.reset();
            r.reconnect_timer.reset();
            r.subscription_timer.reset();
            if (r.socket) {
                if (r.open) {
                    for (const auto& id : r.subscriptions) {
                        r.socket->send(protocol::nostr::close_message(id, memory_));
                    }
                }
                r.socket->close();
                r.socket.reset();
            }
            r.open = false;
        }
        signer_.reset();
    }

private:
    /** The delays between the first announcements on a relay, Trystero's. */
    static constexpr std::array<std::chrono::milliseconds, 4> warmup{
        std::chrono::milliseconds(233),
        std::chrono::milliseconds(533),
        std::chrono::milliseconds(1333),
        std::chrono::milliseconds(5333),
    };
    /** The delay between announcements after the warm-up, and a relay's first cooldown:
     * Trystero's `steadyAnnounceIntervalMs`. */
    static constexpr auto steady_announce = std::chrono::milliseconds(60'000);
    /** The longest cooldown of a relay, Trystero's `maxRelayBackoffMs`. */
    static constexpr auto max_backoff = std::chrono::milliseconds(15 * 60'000);
    /** The first bound of the reconnection delay, Trystero's `defaultRetryMs`. */
    static constexpr auto first_retry = std::chrono::milliseconds(3333);
    /** The largest bound of the reconnection delay, Trystero's `maxRetryMs`. */
    static constexpr auto max_retry = std::chrono::milliseconds(60'000);
    /** How long a connection stays open before the reconnection bound comes back down. */
    static constexpr auto healthy_connection = std::chrono::milliseconds(60'000);
    /** The least delay before a closed subscription is asked for again, Trystero's
     * `subscriptionRetryMs`; it doubles up to @ref steady_announce. */
    static constexpr auto subscription_retry = std::chrono::milliseconds(5333);
    /** How late an EOSE may come before the strategy announces again, as Trystero does once its
     * subscriptions are slow to install. */
    static constexpr auto slow_eose = std::chrono::milliseconds(1000);

    /** A relay's health, Trystero's `RelayHealthState`. */
    enum class health {
        /** Nothing refused. */
        healthy,
        /** Rate-limited: nothing is published until `cooldown_until`. */
        cooldown,
        /** A cooldown ended: an accepted event makes the relay healthy, another rate limit cools
         * it down for twice as long. */
        probation,
        /** Refused for good: never used again. */
        retired,
    };

    /** Where a relay's subscriptions are, Trystero's `BatchSyncState`. */
    enum class sync {
        /** Not asked for, or closed by the relay. */
        unsynced,
        /** Asked for: the relay has not ended the stored events of each yet. */
        awaiting_eose,
        /** The relay sent an EOSE for each. */
        synced,
    };

    struct relay_state {
        relay_state(std::string_view address, std::pmr::memory_resource* memory)
            : url(address, memory), subscriptions(memory), pending(memory) {}

        std::pmr::string url;
        // The socket, from its connection until it closes; null meanwhile.
        std::shared_ptr<backend::websocket> socket;
        // Which connection the socket's handlers report: one a hold closed reports nothing.
        std::uint64_t attempt = 0;
        // The ids of the subscriptions, one per max_topics_per_subscription topics, kept across
        // reconnections, as Trystero's batcher keeps its subIds.
        std::pmr::vector<std::pmr::string> subscriptions;
        bool open = false;
        bool opened_before = false;
        // Whether its first connection closed before it opened, 0.27.0's `boot-failed`.
        bool boot_failed = false;
        // How many announcements the cadence made since it started, sent or skipped, which picks
        // the next delay.
        std::size_t announce_attempt = 0;
        // Whether the announce cadence started on this relay: a reopening or an EOSE that came late
        // then replays the announcement.
        bool announced = false;
        health state = health::healthy;
        // The cooldown's length, which a probation remembers as its last.
        std::chrono::milliseconds cooldown{};
        time_point cooldown_until{};
        sync subscribed = sync::unsynced;
        // The subscriptions still awaiting their EOSE, and when they were asked for.
        std::pmr::vector<std::pmr::string> pending;
        time_point requested{};
        // The delay before a closed subscription is asked for again, and whether that is pending.
        std::chrono::milliseconds subscription_delay = subscription_retry;
        bool subscription_retry_pending = false;
        // The bound of the next reconnection delay.
        std::chrono::milliseconds retry = first_retry;
        time_point opened{};
        std::shared_ptr<backend::timer> announce_timer;
        std::shared_ptr<backend::timer> reconnect_timer;
        std::shared_ptr<backend::timer> subscription_timer;
    };

    static std::array<std::uint8_t, recent_event_ids::key_size> draw_key(
        backend::factory& factory) {
        std::array<std::uint8_t, recent_event_ids::key_size> key{};
        factory.random(key);
        return key;
    }

    std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    /** The room's signer, made at its first event, as Trystero's `getRoomSigner`; null, logged,
     * when it cannot be made. */
    const protocol::nostr::signer* room_signer() {
        if (!signer_) {
            auto made = protocol::nostr::signer::generate(memory_);
            if (!made) {
                log_("nostr", "cannot make the room's signing key: ", made.error());
                return nullptr;
            }
            signer_.emplace(std::move(*made));
        }
        return &*signer_;
    }

    /** Signs an event of `content` on `topic` and sends it on `r`; one that cannot be signed is
     * logged and dropped. */
    void send_event(relay_state& r, std::string_view topic, std::string_view content) {
        const auto* key = room_signer();
        if (key == nullptr) {
            return;
        }
        const auto message =
            protocol::nostr::event_message(*key, unix_now(factory_), topic, content, memory_);
        if (!message) {
            log_("nostr", "cannot sign an event: ", message.error());
            return;
        }
        r.socket->send(*message);
    }

    /** `r`'s health now: a cooldown that ended is a probation, as `getRelayHealth` reads it. */
    health health_of(relay_state& r) const {
        if (r.state == health::cooldown && steady_now(factory_) >= r.cooldown_until) {
            r.state = health::probation;
        }
        return r.state;
    }

    /** How long `r` still cools down; zero when it does not. */
    time_point::duration cooldown_remaining(relay_state& r) const {
        if (health_of(r) != health::cooldown) {
            return time_point::duration::zero();
        }
        return std::max(time_point::duration::zero(), r.cooldown_until - steady_now(factory_));
    }

    /** Whether nothing is published on `r` now, Trystero's `getPublishBlockedResult`. */
    bool blocked(relay_state& r) const {
        return !r.open || health_of(r) == health::retired ||
               cooldown_remaining(r) > time_point::duration::zero();
    }

    void connect(std::size_t index) {
        if (stopped_ || held_ || relays_[index].state == health::retired) {
            return;
        }
        const auto weak = weak_from_this();
        const auto a = allocator();
        const auto attempt = ++relays_[index].attempt;
        relays_[index].socket =
            factory_.connect(relays_[index].url,
                             {
                                 .open =
                                     {
                                         std::allocator_arg,
                                         a,
                                         [weak, index, attempt] {
                                             if (const auto self = weak.lock();
                                                 self && self->current(index, attempt)) {
                                                 self->on_open(index);
                                             }
                                         },
                                     },
                                 .message =
                                     {
                                         std::allocator_arg,
                                         a,
                                         [weak, index, attempt](std::string_view text) {
                                             if (const auto self = weak.lock();
                                                 self && self->current(index, attempt)) {
                                                 self->on_relay_message(index, text);
                                             }
                                         },
                                     },
                                 .close =
                                     {
                                         std::allocator_arg,
                                         a,
                                         [weak, index, attempt] {
                                             if (const auto self = weak.lock();
                                                 self && self->current(index, attempt)) {
                                                 self->on_close(index);
                                             }
                                         },
                                     },
                             },
                             memory_);
    }

    /** Whether `attempt` is the connection of relay `index` now. */
    [[nodiscard]] bool current(std::size_t index, std::uint64_t attempt) const {
        return relays_[index].attempt == attempt;
    }

    void on_open(std::size_t index) {
        auto& r = relays_[index];
        r.open = true;
        any_opened_ = true;
        r.opened = steady_now(factory_);
        log_("nostr", r.opened_before ? "reconnected to " : "connected to ", r.url);
        if (std::exchange(r.opened_before, true)) {
            // Trystero's resubscribeOnReconnect: the cadence that started at the first opening
            // goes on, and the relay gets the subscriptions and the announcement once, after its
            // cooldown when it still cools down.
            resubscribe(index);
            return;
        }
        subscribe(index);
        r.announce_attempt = 0;
        announce(index);
    }

    /**
     * Asks `r` for the room's topics, Trystero's `flushBatch`: one REQ per
     * `max_topics_per_subscription` topics, under the ids the relay's subscriptions had, since a
     * minute ago; each then awaits its EOSE.
     */
    void subscribe(std::size_t index) {
        auto& r = relays_[index];
        if (r.subscription_timer) {
            r.subscription_timer->cancel();
        }
        r.subscription_retry_pending = false;
        constexpr std::size_t chunk = protocol::nostr::max_topics_per_subscription;
        const std::size_t requests = (topics_.size() + chunk - 1) / chunk;
        // The topics are the strategy's for its whole life, so the subscriptions never outnumber
        // the REQs: Trystero's CLOSE of the surplus ids, for a batcher whose topics leave, has no
        // case here.
        BOOST_ASSERT(r.subscriptions.size() <= requests);
        const auto since = protocol::nostr::subscribe_since(unix_now(factory_));
        r.pending.clear();
        for (std::size_t i = 0; i < requests; ++i) {
            if (i == r.subscriptions.size()) {
                r.subscriptions.push_back(
                    random_id(factory_, protocol::subscription_id_size, memory_));
            }
            const std::span<const std::pmr::string> topics(
                topics_.data() + (i * chunk), std::min(chunk, topics_.size() - (i * chunk)));
            r.socket->send(
                protocol::nostr::subscribe_message(r.subscriptions[i], topics, since, memory_));
            r.pending.push_back(r.subscriptions[i]);
        }
        r.subscribed = sync::awaiting_eose;
        r.requested = steady_now(factory_);
    }

    /** Asks `r` for its subscriptions again, unless it still cools down, and replays the room's
     * announcement there: Trystero's `resubscribeOnReconnect`. */
    void resubscribe(std::size_t index) {
        auto& r = relays_[index];
        if (stopped_ || !r.open || r.state == health::retired) {
            return;
        }
        if (cooldown_remaining(r) > time_point::duration::zero()) {
            retry_subscription(index, true);
            return;
        }
        subscribe(index);
        replay_announcement(index);
    }

    /**
     * Asks for `r`'s subscriptions again later, Trystero's `retrySubscription`: after the cooldown
     * of a rate-limited relay, otherwise after a delay of 5333 ms that doubles up to 60 s; never
     * sooner than 5333 ms. A retry already pending stays, unless the relay rate-limits us.
     */
    void retry_subscription(std::size_t index, bool rate_limited) {
        auto& r = relays_[index];
        if (!r.open || (r.subscription_retry_pending && !rate_limited)) {
            return;
        }
        const auto delay =
            rate_limited
                ? std::chrono::duration_cast<std::chrono::milliseconds>(cooldown_remaining(r))
                : r.subscription_delay;
        r.subscription_delay = std::min(r.subscription_delay * 2, steady_announce);
        if (r.subscribed == sync::synced) {
            r.subscribed = sync::unsynced;
        }
        if (!r.subscription_timer) {
            r.subscription_timer = factory_.make_timer(memory_);
        }
        r.subscription_retry_pending = true;
        const auto weak = weak_from_this();
        r.subscription_timer->start(std::max(delay, subscription_retry),
                                    {std::allocator_arg, allocator(), [weak, index] {
                                         if (const auto self = weak.lock()) {
                                             self->relays_[index].subscription_retry_pending =
                                                 false;
                                             self->resubscribe(index);
                                         }
                                     }});
    }

    /** Publishes the room's announcement on `r` again, with a fresh nonce, outside its cadence,
     * as Trystero's `replayAnnouncements` does; only once the room announced there. */
    void replay_announcement(std::size_t index) {
        auto& r = relays_[index];
        if (!r.announced || blocked(r)) {
            return;
        }
        send_event(r, announce_topic_, announcement());
    }

    /** The announcement's text, with a nonce of `protocol::nonce_size` characters drawn now. */
    std::pmr::string announcement() {
        const auto nonce = random_id(factory_, protocol::nonce_size, memory_);
        return announcement_(nonce);
    }

    void on_close(std::size_t index) {
        auto& r = relays_[index];
        const bool was_open = r.open;
        r.open = false;
        r.socket.reset();
        if (stopped_ || r.state == health::retired) {
            return;
        }
        if (was_open && steady_now(factory_) - r.opened >= healthy_connection) {
            r.retry = first_retry;
        }
        log_("nostr", was_open ? "lost " : "could not reach ", r.url, ", next attempt within ",
             r.retry.count(), " ms");
        if (!r.opened_before && !std::exchange(r.boot_failed, true)) {
            report_if_all_failed();
            // The report reaches the user's log sink, which may leave the room and so stop this.
            if (stopped_) {
                return;
            }
        }
        std::array<std::uint8_t, 2> random{};
        factory_.random(random);
        // A share of the bound in [0, 1), in steps of 1/65536, and the delay in whole
        // milliseconds below it.
        const std::uint16_t draw = boost::endian::load_big_u16(random.data());
        const double share = draw / 65536.0;
        const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(share * r.retry);
        r.retry = std::min(r.retry * 2, max_retry);
        r.reconnect_timer = factory_.make_timer(memory_);
        const auto weak = weak_from_this();
        r.reconnect_timer->start(delay, {std::allocator_arg, allocator(), [weak, index] {
                                             if (const auto self = weak.lock()) {
                                                 self->connect(index);
                                             }
                                         }});
    }

    /** Calls `all_failed` once, when no relay opened yet and the first connection of every relay
     * closed before it opened, as 0.27.0's `maybeReportAllBootFailed` reports `relay-failed`. */
    void report_if_all_failed() {
        if (any_opened_ || all_failed_reported_ || relays_.empty() ||
            !std::ranges::all_of(relays_, [](const relay_state& r) { return r.boot_failed; })) {
            return;
        }
        all_failed_reported_ = true;
        log_("nostr", "every relay failed");
        if (all_failed_) {
            all_failed_();
        }
    }

    void announce(std::size_t index) {
        auto& r = relays_[index];
        if (stopped_ || r.state == health::retired) {
            return;
        }
        r.announced = true;
        // As Trystero's queueAnnounce: an announcement the relay cannot take now waits for the
        // cooldown, at least 60 s, and the warm-up's delays bound the wait either way.
        auto next = steady_announce;
        if (const auto remaining = cooldown_remaining(r);
            !r.open || remaining > time_point::duration::zero()) {
            next = std::max(steady_announce,
                            std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
        } else {
            send_event(r, announce_topic_, announcement());
        }
        if (const auto attempt = r.announce_attempt++; attempt < warmup.size()) {
            next = std::min(next, warmup[attempt]);
        }
        if (!r.announce_timer) {
            r.announce_timer = factory_.make_timer(memory_);
        }
        const auto weak = weak_from_this();
        r.announce_timer->start(next, {std::allocator_arg, allocator(), [weak, index] {
                                           if (const auto self = weak.lock()) {
                                               self->announce(index);
                                           }
                                       }});
    }

    /** Cools `r` down, Trystero's `backoffRelay`: 60 s from health, twice the last cooldown from a
     * cooldown or a probation, at most 15 min. */
    void back_off(relay_state& r) {
        switch (health_of(r)) {
            case health::retired: return;
            case health::healthy: r.cooldown = steady_announce; break;
            case health::cooldown:
            case health::probation:
                r.cooldown = std::min(std::max(steady_announce, r.cooldown * 2), max_backoff);
                break;
        }
        r.state = health::cooldown;
        r.cooldown_until = steady_now(factory_) + r.cooldown;
        log_("nostr", r.url, " rate-limits us, pausing it for ",
             std::chrono::duration_cast<std::chrono::seconds>(r.cooldown).count(), " s");
    }

    static bool terminal(std::string_view reason) {
        constexpr std::array<std::string_view, 4> prefixes{
            "blocked:",
            "restricted:",
            "auth-required:",
            "pow:",
        };
        return std::ranges::any_of(
            prefixes, [reason](std::string_view prefix) { return reason.starts_with(prefix); });
    }

    /** Retires a relay for good, Trystero's `retireRelay`; false when it was retired already. */
    bool retire(std::size_t index, std::string_view reason) {
        auto& r = relays_[index];
        if (r.state == health::retired) {
            return false;
        }
        r.state = health::retired;
        log_("nostr", "retiring ", r.url, ": ", reason);
        r.announce_timer.reset();
        r.subscription_timer.reset();
        r.subscription_retry_pending = false;
        if (r.socket) {
            r.socket->close();
        }
        return true;
    }

    /** Whether `id` is one of `r`'s subscriptions. */
    static bool owns(const relay_state& r, std::string_view id) {
        return std::ranges::find(r.subscriptions, id) != r.subscriptions.end();
    }

    void on_relay_message(std::size_t index, std::string_view text) {
        // A text past its bound is dropped as a malformed one is, before it is read.
        const auto document = webcpp::document::parse(
            text, protocol::json::protocol_limits(protocol::nostr::max_parsed_relay_text_bytes),
            std::pmr::polymorphic_allocator<std::byte>(memory_));
        if (!document.has_value()) {
            return;
        }
        const auto message = protocol::nostr::read_relay_message(document->root());
        if (!message) {
            return;
        }
        if (const auto* e = std::get_if<protocol::nostr::event>(&*message)) {
            on_event(index, *e);
        } else {
            on_feedback(index, *message);
        }
    }

    void on_event(std::size_t index, const protocol::nostr::event& e) {
        if (!owns(relays_[index], e.subscription) || e.topic.empty() ||
            std::ranges::find(topics_, e.topic) == topics_.end()) {
            return;
        }
        // A repeat is dropped only under an id checked to be its event's own: a relay could
        // otherwise send a forged event under a genuine event's id ahead of the relay that carries
        // the genuine one, and have the room drop it.
        if (protocol::nostr::has_own_id(e, memory_) && !seen_.remember(e.id)) {
            return;
        }
        on_message_(index, e.topic, e.content);
    }

    /** A relay's OK, EOSE, CLOSED or NOTICE, as 0.27.0's relay handler reads it. */
    void on_feedback(std::size_t index, const protocol::nostr::relay_message& message) {
        auto& r = relays_[index];
        const auto* o = std::get_if<protocol::nostr::ok>(&message);
        const auto* c = std::get_if<protocol::nostr::closed>(&message);
        // A CLOSED of a subscription not ours says nothing of this room's.
        if (c != nullptr && !owns(r, c->subscription)) {
            return;
        }
        const bool rejected = o != nullptr && !o->accepted;
        std::string_view reason;
        if (c != nullptr) {
            reason = c->reason;
        } else if (o != nullptr) {
            reason = o->reason;
        }
        const bool refused = rejected || c != nullptr;
        const bool rate_limited = refused && reason.starts_with("rate-limited:");
        const bool is_terminal = terminal(reason);
        if (o != nullptr && o->accepted && health_of(r) == health::probation) {
            r.state = health::healthy;
        }
        if (refused && is_terminal) {
            retire(index, reason);
            return;
        }
        if (rate_limited) {
            back_off(r);
        } else if (const auto* end = std::get_if<protocol::nostr::eose>(&message)) {
            on_eose(index, end->subscription);
        }
        if (c != nullptr) {
            retry_subscription(index, rate_limited);
            log_("nostr", r.url, " closed our subscription: ", reason);
        } else if (rejected && !reason.starts_with("duplicate:")) {
            log_("nostr", r.url, " refused an event: ", reason);
        } else if (const auto* n = std::get_if<protocol::nostr::notice>(&message)) {
            log_("nostr", r.url, " notice: ", n->text);
        }
    }

    /** The end of a subscription's stored events: once the last pending one ends, the relay is
     * synced, and one that took 1 s or more gets the room's announcement again. */
    void on_eose(std::size_t index, std::string_view subscription) {
        auto& r = relays_[index];
        if (r.subscribed != sync::awaiting_eose) {
            return;
        }
        const auto it = std::ranges::find(r.pending, subscription);
        if (it == r.pending.end()) {
            return;
        }
        r.pending.erase(it);
        if (!r.pending.empty()) {
            return;
        }
        if (r.subscription_timer) {
            r.subscription_timer->cancel();
        }
        r.subscription_retry_pending = false;
        r.subscription_delay = subscription_retry;
        r.subscribed = sync::synced;
        if (steady_now(factory_) - r.requested >= slow_eose) {
            replay_announcement(index);
        }
    }

    backend::factory& factory_;
    std::pmr::memory_resource* memory_;
    std::pmr::vector<std::pmr::string> topics_;
    std::pmr::string announce_topic_;
    announcement_handler announcement_;
    message_handler on_message_;
    handler<void()> all_failed_;
    logger log_;
    std::optional<protocol::nostr::signer> signer_;
    std::pmr::vector<relay_state> relays_;
    recent_event_ids seen_;
    bool stopped_ = false;
    // Whether start() ran, and whether the relays are held closed.
    bool started_ = false;
    bool held_ = false;
    // Whether a relay ever opened, and whether the failure of every relay was reported.
    bool any_opened_ = false;
    bool all_failed_reported_ = false;
};

}  // namespace webcpp::trystero::detail

#endif
