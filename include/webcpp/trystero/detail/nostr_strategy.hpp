// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_NOSTR_STRATEGY_HPP
#define WEBCPP_TRYSTERO_DETAIL_NOSTR_STRATEGY_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/nostr.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <boost/json/storage_ptr.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/**
 * Trystero's Nostr strategy: one subscription per relay to the room's root and self topics,
 * announcements on the root topic at Trystero's cadence, and signals published on the relay that
 * carried the message they answer.
 *
 * | Rule               | Value                                                                    |
 * | announce cadence   | at once, then after 233, 533, 1333 and 5333 ms, then every 60 s          |
 * | rate-limited relay | skipped for 60 s, doubling to 15 min, while "rate-limited:" replies      |
 * |                    | persist                                                                  |
 * | terminal rejection | "blocked:", "restricted:", "auth-required:" or "pow:" retires the relay  |
 * | reconnect          | after a random share of 3.3 s, doubling to 60 s; back to 3.3 s once a    |
 * |                    | connection stayed open for 60 s                                          |
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

    /** Every allocation goes through `memory`, the parsed relay messages through `json`, a storage
     * of that resource. */
    nostr_strategy(backend::factory& factory, std::span<const std::pmr::string> urls,
                   std::pmr::vector<std::pmr::string> topics, std::string_view announce_topic,
                   handler<std::pmr::string()> announcement, message_handler on_message, logger log,
                   std::pmr::memory_resource* memory, boost::json::storage_ptr json)
        : factory_(factory),
          memory_(memory),
          json_(std::move(json)),
          topics_(std::move(topics), memory),
          announce_topic_(announce_topic, memory),
          announcement_(std::move(announcement)),
          on_message_(std::move(on_message)),
          log_(std::move(log)),
          signer_(make_signer(factory, memory)),
          relays_(memory),
          seen_(memory),
          seen_order_(memory) {
        relays_.reserve(urls.size());
        for (const auto& url : urls) {
            relays_.emplace_back(url, memory);
        }
    }

    /** Connects to every relay; each subscribes and announces once it opens. */
    void start() {
        for (std::size_t i = 0; i < relays_.size(); ++i) {
            connect(i);
        }
    }

    /** Publishes a signal to a peer's topic on one relay, unless that relay is closed or backing
     * off; Trystero drops it too. */
    void publish(std::size_t index, std::string_view topic, std::string_view content) {
        if (index >= relays_.size()) {
            return;
        }
        const auto& r = relays_[index];
        if (!r.open || backoff_remaining(r) > 0) {
            return;
        }
        r.socket->send(event_for(topic, content));
    }

    /** Starts the announce cadence over, as Trystero does when a peer leaves. */
    void restart_announcements() {
        for (std::size_t i = 0; i < relays_.size(); ++i) {
            relays_[i].announce_attempt = 0;
            if (relays_[i].open) {
                announce(i);
            }
        }
    }

    /** How many relays the strategy uses. */
    std::size_t relay_count() const { return relays_.size(); }

    /** Stops announcing and reconnecting, closes each subscription and each connection. */
    void stop() {
        stopped_ = true;
        for (auto& r : relays_) {
            r.announce_timer.reset();
            r.reconnect_timer.reset();
            if (r.socket) {
                if (r.open) {
                    r.socket->send(protocol::nostr::close_message(r.subscription, memory_));
                }
                r.socket->close();
                r.socket.reset();
            }
            r.open = false;
        }
    }

private:
    static constexpr std::array<int, 4> warmup_ms{233, 533, 1333, 5333};
    static constexpr int steady_announce_ms = 60'000;
    static constexpr double max_backoff_ms = 15 * 60'000;
    static constexpr double first_retry_ms = 3333;
    static constexpr double max_retry_ms = 60'000;
    static constexpr double healthy_ms = 60'000;

    struct relay_state {
        relay_state(std::string_view address, std::pmr::memory_resource* memory)
            : url(address, memory), subscription(memory) {}

        std::pmr::string url;
        std::shared_ptr<backend::websocket> socket;
        std::pmr::string subscription;
        bool open = false;
        bool retired = false;
        bool opened_before = false;
        int announce_attempt = 0;
        double backoff_ms = 0;
        double backoff_until_ms = 0;
        double retry_ms = first_retry_ms;
        double opened_ms = 0;
        std::shared_ptr<backend::timer> announce_timer;
        std::shared_ptr<backend::timer> reconnect_timer;
    };

    static protocol::nostr::signer make_signer(backend::factory& factory,
                                               std::pmr::memory_resource* memory) {
        std::array<std::uint8_t, 32> secret{};
        for (;;) {
            factory.random(secret);
            if (auto s = protocol::nostr::signer::from_secret(secret, memory)) {
                return std::move(*s);
            }
        }
    }

    std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    std::pmr::string make_subscription_id() {
        std::array<std::uint8_t, 64> random{};
        factory_.random(random);
        std::pmr::string id(random.size(), '0', memory_);
        for (std::size_t i = 0; i < random.size(); ++i) {
            id[i] = protocol::id_alphabet[random[i] % protocol::id_alphabet.size()];
        }
        return id;
    }

    std::pmr::string event_for(std::string_view topic, std::string_view content) {
        std::array<std::uint8_t, 32> aux{};
        factory_.random(aux);
        return protocol::nostr::event_message(signer_, factory_.unix_seconds(), topic, content, aux,
                                              memory_);
    }

    double backoff_remaining(const relay_state& r) const {
        return std::max(0.0, r.backoff_until_ms - factory_.now_ms());
    }

    void connect(std::size_t index) {
        if (stopped_ || relays_[index].retired) {
            return;
        }
        const auto weak = weak_from_this();
        const auto a = allocator();
        relays_[index].socket =
            factory_.connect(relays_[index].url,
                             {
                                 .open =
                                     {
                                         std::allocator_arg,
                                         a,
                                         [weak, index] {
                                             if (const auto self = weak.lock()) {
                                                 self->on_open(index);
                                             }
                                         },
                                     },
                                 .message =
                                     {
                                         std::allocator_arg,
                                         a,
                                         [weak, index](std::string_view text) {
                                             if (const auto self = weak.lock()) {
                                                 self->on_relay_message(index, text);
                                             }
                                         },
                                     },
                                 .close =
                                     {
                                         std::allocator_arg,
                                         a,
                                         [weak, index] {
                                             if (const auto self = weak.lock()) {
                                                 self->on_close(index);
                                             }
                                         },
                                     },
                             },
                             memory_);
    }

    void on_open(std::size_t index) {
        auto& r = relays_[index];
        r.open = true;
        r.opened_ms = factory_.now_ms();
        if (r.subscription.empty()) {
            r.subscription = make_subscription_id();
        }
        r.socket->send(protocol::nostr::subscribe_message(r.subscription, topics_,
                                                          factory_.unix_seconds(), memory_));
        log_("nostr", r.opened_before ? "reconnected to " : "connected to ", r.url,
             ", subscribed to the room's topics");
        r.opened_before = true;
        r.announce_attempt = 0;
        announce(index);
    }

    void on_close(std::size_t index) {
        auto& r = relays_[index];
        const bool was_open = r.open;
        r.open = false;
        r.announce_timer.reset();
        if (stopped_ || r.retired) {
            return;
        }
        if (was_open && factory_.now_ms() - r.opened_ms >= healthy_ms) {
            r.retry_ms = first_retry_ms;
        }
        log_("nostr", was_open ? "lost " : "could not reach ", r.url, ", next attempt within ",
             static_cast<long long>(r.retry_ms), " ms");
        std::array<std::uint8_t, 2> random{};
        factory_.random(random);
        const double share = ((unsigned{random[0]} << 8U) | random[1]) / 65536.0;
        const auto delay = std::chrono::milliseconds(static_cast<long long>(share * r.retry_ms));
        r.retry_ms = std::min(r.retry_ms * 2, max_retry_ms);
        r.reconnect_timer = factory_.make_timer(memory_);
        const auto weak = weak_from_this();
        r.reconnect_timer->start(delay, {std::allocator_arg, allocator(), [weak, index] {
                                             if (const auto self = weak.lock()) {
                                                 self->connect(index);
                                             }
                                         }});
    }

    void announce(std::size_t index) {
        auto& r = relays_[index];
        if (stopped_ || r.retired) {
            return;
        }
        int next_ms = steady_announce_ms;
        if (const double remaining = backoff_remaining(r); !r.open || remaining > 0) {
            next_ms = std::max(steady_announce_ms, static_cast<int>(remaining));
        } else {
            r.socket->send(event_for(announce_topic_, announcement_()));
            const int attempt = r.announce_attempt++;
            if (std::cmp_less(attempt, warmup_ms.size())) {
                next_ms = warmup_ms[attempt];
            }
        }
        if (!r.announce_timer) {
            r.announce_timer = factory_.make_timer(memory_);
        }
        const auto weak = weak_from_this();
        r.announce_timer->start(std::chrono::milliseconds(next_ms),
                                {std::allocator_arg, allocator(), [weak, index] {
                                     if (const auto self = weak.lock()) {
                                         self->announce(index);
                                     }
                                 }});
    }

    void back_off(relay_state& r) {
        r.backoff_ms =
            r.backoff_ms == 0
                ? steady_announce_ms
                : std::min(std::max<double>(steady_announce_ms, r.backoff_ms * 2), max_backoff_ms);
        r.backoff_until_ms = factory_.now_ms() + r.backoff_ms;
        log_("nostr", r.url, " rate-limits us, pausing it for ",
             static_cast<int>(r.backoff_ms / 1000), " s");
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

    void retire(std::size_t index, std::string_view reason) {
        auto& r = relays_[index];
        if (r.retired) {
            return;
        }
        r.retired = true;
        log_("nostr", "retiring ", r.url, ": ", reason);
        r.announce_timer.reset();
        if (r.socket) {
            r.socket->close();
        }
    }

    /** Whether an event id is new; a relay may repeat one, and several relays carry the same. */
    bool remember(std::string_view id) {
        if (id.empty()) {
            return true;
        }
        if (seen_.contains(id)) {
            return false;
        }
        const auto& stored = seen_order_.emplace_back(id);  // made in the deque's resource
        seen_.insert(stored);
        if (seen_order_.size() > remembered_events) {
            seen_.erase(seen_order_.front());
            seen_order_.pop_front();
        }
        return true;
    }

    void on_relay_message(std::size_t index, std::string_view text) {
        boost::system::error_code ec;
        const auto document = protocol::json::parse(text, json_, ec);
        if (ec) {
            return;
        }
        const auto message = protocol::nostr::read_relay_message(document);
        if (!message) {
            return;
        }
        auto& r = relays_[index];
        if (const auto* e = std::get_if<protocol::nostr::event>(&*message)) {
            if (e->subscription != r.subscription || e->topic.empty()) {
                return;
            }
            if (std::ranges::find(topics_, e->topic) == topics_.end() || !remember(e->id)) {
                return;
            }
            on_message_(index, e->topic, e->content);
        } else if (const auto* o = std::get_if<protocol::nostr::ok>(&*message)) {
            if (o->accepted) {
                if (backoff_remaining(r) == 0) {
                    r.backoff_ms = 0;
                }
            } else if (o->reason.starts_with("rate-limited:")) {
                back_off(r);
            } else if (terminal(o->reason)) {
                retire(index, o->reason);
            } else if (!o->reason.starts_with("duplicate:")) {
                log_("nostr", r.url, " refused an event: ", o->reason);
            }
        } else if (const auto* c = std::get_if<protocol::nostr::closed>(&*message)) {
            if (terminal(c->reason)) {
                retire(index, c->reason);
                return;
            }
            if (c->reason.starts_with("rate-limited:")) {
                back_off(r);
            }
            log_("nostr", r.url, " closed our subscription: ", c->reason);
            resubscribe_later(index);
        } else if (const auto* n = std::get_if<protocol::nostr::notice>(&*message)) {
            log_("nostr", r.url, " notice: ", n->text);
        }
    }

    void resubscribe_later(std::size_t index) {
        auto& r = relays_[index];
        if (!r.reconnect_timer) {
            r.reconnect_timer = factory_.make_timer(memory_);
        }
        const auto delay = std::chrono::milliseconds(
            static_cast<long long>(std::max(backoff_remaining(r), 5333.0)));
        const auto weak = weak_from_this();
        r.reconnect_timer->start(delay,
                                 {std::allocator_arg, allocator(), [weak, index] {
                                      const auto self = weak.lock();
                                      if (!self || self->stopped_) {
                                          return;
                                      }
                                      const auto& relay = self->relays_[index];
                                      if (relay.open) {
                                          relay.socket->send(protocol::nostr::subscribe_message(
                                              relay.subscription, self->topics_,
                                              self->factory_.unix_seconds(), self->memory_));
                                      }
                                  }});
    }

    /** Transparent hashing, so an id read as a view is looked up without building a string. */
    struct view_hash {
        using is_transparent = void;

        std::size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

    backend::factory& factory_;
    std::pmr::memory_resource* memory_;
    boost::json::storage_ptr json_;
    std::pmr::vector<std::pmr::string> topics_;
    std::pmr::string announce_topic_;
    handler<std::pmr::string()> announcement_;
    message_handler on_message_;
    logger log_;
    protocol::nostr::signer signer_;
    std::pmr::vector<relay_state> relays_;
    // The ids are owned by `seen_order_`, a deque, which never moves what it holds; `seen_` indexes
    // them by view.
    std::pmr::unordered_set<std::string_view, view_hash, std::equal_to<>> seen_;
    std::pmr::deque<std::pmr::string> seen_order_;
    bool stopped_ = false;
};

}  // namespace webcpp::trystero::detail

#endif
