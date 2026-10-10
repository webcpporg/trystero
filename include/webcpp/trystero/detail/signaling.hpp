// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_SIGNALING_HPP
#define WEBCPP_TRYSTERO_DETAIL_SIGNALING_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/construction_window.hpp>
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/detail/replay_cache.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <boost/json/storage_ptr.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/** A copy of `servers` whose every string is in `memory`. */
inline std::pmr::vector<backend::ice_server> copy_ice_servers(
    std::span<const backend::ice_server> servers, std::pmr::memory_resource* memory) {
    std::pmr::vector<backend::ice_server> out(memory);
    out.reserve(servers.size());
    for (const auto& server : servers) {
        out.push_back({
            .urls = std::pmr::vector<std::pmr::string>(server.urls, memory),
            .username = std::pmr::string(server.username, memory),
            .credential = std::pmr::string(server.credential, memory),
        });
    }
    return out;
}

/**
 * The negotiation with each peer seen on the signaling topics, after Trystero's signal handler.
 *
 * | Message      | What this side does                                                            |
 * | announcement | the lower peer id offers on the relay that carried it; the higher one asks the |
 * |              | other to offer by announcing itself on the other's topic, at most once a       |
 * |              | second per peer and once in 5.3 s per relay for the whole room                 |
 * | offer        | answers it, unless it already answers one, or offers itself and its id is      |
 * |              | lower                                                                          |
 * | answer       | applies it to the matching offer                                               |
 * | candidate    | adds it to the connection its offer id names, or keeps it until that           |
 * |              | connection has a remote description                                            |
 *
 * An offer, an answer and every candidate travel sealed (protocol/signal.hpp). A negotiation ends
 * when the data channel opens: the connection is handed to the room and signals from that peer are
 * ignored until it leaves.
 *
 * Announcements are not sealed, so anyone who knows the app and room ids can announce any number
 * of peer ids, and anyone who reads the relays can send any signal in any peer's name. Nothing a
 * peer sends changes a negotiation before it opens with the room key: a signal that does not open
 * is counted, reported once per negotiation, and changes nothing else; and since a flood of fresh
 * ids makes each a negotiation, the reports are paced too: the first of a quiet interval at once,
 * the rest together when it ends (`failed_open_report_interval`). A sealed offer or answer holds
 * neither its offer id nor a peer id (Trystero's wire), so one recorded from the relays still opens
 * when it is resent in its sender's name: the room remembers the offers and answers it made or
 * acted on (`remembered_descriptions`) and refuses one that comes to it as a peer's, which it logs:
 * a resent one, or its own sent back. One it neither made nor acted on still opens and is acted
 * on, and every peer seals a fresh offer for any id announced above its own: so anyone who reads
 * and publishes on the relays, without the key, can keep two peers apart from their first meeting
 * for as long as it keeps at it, though it can neither read nor join anything, nor touch joined
 * peers. Only a wire that seals the offer id and both peer ids closes this. The negotiations under
 * way are bounded (`max_unconnected_peers`): a negotiation is dropped when its offer and its answer
 * expire without a connection, an idle one gives its place to a new peer, and beyond the bound a
 * new peer is refused, logged once. The connections made are bounded too
 * (`max_connections_per_window`), however soon they end, since a browser counts a closed one until
 * its garbage collector runs. Joined peers, memory and connections stay safe under a flood; new
 * peers do not: a flooder who keeps sending fresh ids takes each place, and each connection of the
 * window, as it frees, and keeps new peers out for as long as the flood lasts. A joined peer whose
 * connection drops is a new peer when it comes back, with the same id, and is kept out the same
 * way: there is no renegotiation of a joined peer's connection. Trystero bounds none of this.
 *
 * Tip: a peer id is copied once, into the map's key, and every asynchronous handler captures its
 * own copy, in the room's resource, because it may outlive the entry. The entry points a handler
 * calls copy their arguments first, since they may reset the very handler whose captures the
 * arguments refer to.
 */
class signaling : public std::enable_shared_from_this<signaling> {
public:
    /**
     * The most negotiations a room holds at once with peers it is not connected to, each with at
     * most two pending connections: a peer beyond it waits for its next announcement.
     */
    static constexpr std::size_t max_unconnected_peers = 32;

    /**
     * The most connections a room makes in any 57 s (`offer_ttl`, the longest a negotiation holds
     * one): those its unconnected negotiations hold at most at once. A place freed early is not
     * taken again until the window has passed, so no signal, however soon it ends a negotiation,
     * makes a room add more than this to what a browser counts.
     */
    static constexpr std::size_t max_connections_per_window = 2 * max_unconnected_peers;

    /**
     * How many of the offers and answers it made or acted on a room remembers, to refuse one that
     * comes to it as a peer's (detail::replay_cache): each stays at least an offer's lifetime,
     * since the room makes or acts on at most two per connection it makes (an offer and the answer
     * to it), and in a quiet room far longer.
     */
    static constexpr std::size_t remembered_descriptions = 1024;
    static_assert(remembered_descriptions >= 2 * max_connections_per_window,
                  "every offer and answer made or acted on is remembered for an offer's lifetime");

    /** What the signaling asks of its room. */
    struct callbacks {
        /** Publishes a signal on a topic, through the relay given. */
        handler<void(std::size_t relay, std::string_view topic, std::string_view content)> publish;
        /** A negotiation ended with an open data channel: the connection is the room's. */
        handler<void(std::string_view peer, std::shared_ptr<backend::peer_connection>,
                     std::shared_ptr<backend::data_channel>)>
            connected;
        /**
         * A negotiation with a peer failed, with the error and what happened, or an empty text
         * for the error's own message; several, with an empty peer, when `what` counts them.
         */
        handler<void(std::string_view peer, boost::system::error_code, std::string_view what)>
            failed;
    };

    /** Every allocation goes through `memory`, the parsed signals through `json`, a storage of that
     * resource. */
    signaling(backend::factory& factory, std::shared_ptr<backend::sealer> sealer,
              std::string_view app_id, std::string_view room_id, std::string_view self_id,
              std::span<const backend::ice_server> ice_servers, callbacks cb, logger log,
              std::pmr::memory_resource* memory, boost::json::storage_ptr json)
        : factory_(factory),
          memory_(memory),
          json_(std::move(json)),
          sealer_(std::move(sealer)),
          app_id_(app_id, memory),
          room_id_(room_id, memory),
          self_id_(self_id, memory),
          ice_servers_(copy_ice_servers(ice_servers, memory)),
          cb_(std::move(cb)),
          log_(std::move(log)),
          peers_(memory) {}

    /** A message on the root or the self topic; `content` is read during the call. */
    void handle(std::size_t relay, std::string_view content) {
        if (closed_) {
            return;
        }
        boost::system::error_code ec;
        const auto document = protocol::json::parse(content, json_, ec);
        const auto message = ec ? std::nullopt : protocol::read_signal(document);
        if (!message || message->peer_id == self_id_) {
            return;
        }
        auto it = peers_.find(message->peer_id);
        if (it == peers_.end()) {
            if (!make_room_for(message->peer_id)) {
                return;
            }
            it = peers_.try_emplace(std::pmr::string(message->peer_id, memory_), allocator()).first;
        }
        const std::string_view peer = it->first;
        auto& n = it->second;
        if (n.connected) {
            return;
        }
        n.last_ms = factory_.now_ms();
        if (message->is_announcement()) {
            on_announcement(relay, peer, n);
        } else if (!message->offer.empty()) {
            on_offer(relay, peer, message->offer_id, message->offer, n);
        } else if (!message->answer.empty()) {
            on_answer(peer, message->offer_id, message->answer, n);
        } else {
            on_candidate(peer, message->offer_id, message->candidate);
        }
    }

    /** The peer left after connecting: its next announcement starts a new negotiation. */
    void forget(std::string_view peer) {
        if (const auto it = peers_.find(peer); it != peers_.end()) {
            reset_offer(it->second);
            reset_answer(it->second);
            peers_.erase(it);
        }
    }

    /** Ends every negotiation, and ignores every signal from now on. */
    void close() {
        closed_ = true;
        for (auto& [id, n] : peers_) {
            reset_offer(n);
            reset_answer(n);
        }
        peers_.clear();
    }

private:
    using pmr_allocator = std::pmr::polymorphic_allocator<>;

    static constexpr auto offer_ttl = std::chrono::milliseconds(57'333);
    static constexpr auto answered_offer_ttl = std::chrono::milliseconds(23'333);
    static constexpr auto answering_ttl = std::chrono::milliseconds(23'333);
    static constexpr auto first_offer_retry = std::chrono::milliseconds(2'000);
    static constexpr auto later_offer_retry = std::chrono::milliseconds(4'800);
    static constexpr int max_offer_retries = 2;
    static constexpr std::size_t max_pending_candidates = 128;
    /** The least time between two counter-announcements of the room on a relay: Trystero's
     * `announceIntervalMs`, the pace at which it announces. */
    static constexpr auto counter_announce_interval = std::chrono::milliseconds(5'333);
    /** How many refused peer ids are remembered, so that each is logged once. */
    static constexpr std::size_t remembered_refusals = 64;
    /** The least time between two reports of signals that did not open. */
    static constexpr auto failed_open_report_interval = std::chrono::milliseconds(10'000);

    /** One side of a negotiation: a connection, the remote candidates waiting for its remote
     * description. */
    struct side {
        explicit side(pmr_allocator a) : pending_remote(a) {}

        std::shared_ptr<backend::peer_connection> pc;
        std::shared_ptr<backend::data_channel> channel;
        std::pmr::vector<std::pmr::string> pending_remote;
    };

    struct negotiation {
        explicit negotiation(pmr_allocator a)
            : offer(a),
              offer_id(a),
              sealed_offer(a),
              offer_relays(a),
              offer_candidates(a),
              offer_retry(a),
              offer_retries(a),
              answer(a),
              answered_offer_id(a),
              answered_offer(a),
              answer_messages(a),
              answer_relays(a),
              answer_candidates(a),
              answer_replayed_ms(a),
              counter_announced_ms(a) {}

        bool connected = false;
        // When the peer last sent a message, for the eviction of an idle negotiation.
        double last_ms = 0;
        // Whether a signal of this negotiation that did not open was reported.
        bool failed_open_reported = false;
        // Whether an offer or an answer of this negotiation that the room had used before was
        // logged.
        bool replay_logged = false;
        // Our offer.
        side offer;
        std::pmr::string offer_id;
        std::pmr::string sealed_offer;  // empty until the offer is ready to send
        bool answered = false;
        std::pmr::set<std::size_t> offer_relays;
        std::pmr::vector<std::pmr::string>
            offer_candidates;  // plaintext, gathered before the offer went out
        std::shared_ptr<backend::timer> offer_expiry;
        std::pmr::map<std::size_t, std::shared_ptr<backend::timer>> offer_retry;
        std::pmr::map<std::size_t, int> offer_retries;
        // Our answer to their offer.
        side answer;
        std::pmr::string answered_offer_id;
        std::pmr::string answered_offer;
        bool answer_sent = false;
        std::pmr::vector<std::pmr::string>
            answer_messages;  // replayed when the same offer arrives again
        std::pmr::set<std::size_t> answer_relays;
        std::pmr::vector<std::pmr::string>
            answer_candidates;  // plaintext, gathered before the answer went out
        std::pmr::map<std::size_t, double> answer_replayed_ms;
        std::shared_ptr<backend::timer> answer_expiry;
        // Counter-announcements, per relay.
        std::pmr::map<std::size_t, double> counter_announced_ms;
    };

    pmr_allocator allocator() const { return {memory_}; }

    std::pmr::string copy(std::string_view text) const { return std::pmr::string(text, memory_); }

    /** `f` as a handler of the room's resource. */
    template <class Signature, class F>
    handler<Signature> bind(F&& f) const {
        return handler<Signature>(std::allocator_arg, allocator(), std::forward<F>(f));
    }

    std::pmr::string peer_topic(std::string_view peer) const {
        return protocol::peer_topic(app_id_, room_id_, peer, memory_);
    }

    std::pmr::string make_id(std::size_t n) {
        std::array<std::uint8_t, 32> random{};
        factory_.random(std::span(random).first(n));
        std::pmr::string id(n, '0', memory_);
        for (std::size_t i = 0; i < n; ++i) {
            id[i] = protocol::id_alphabet[random[i] % protocol::id_alphabet.size()];
        }
        return id;
    }

    negotiation* find(std::string_view peer) {
        const auto it = peers_.find(peer);
        return it == peers_.end() ? nullptr : &it->second;
    }

    /**
     * Whether a negotiation may start with a new peer: the room holds fewer than
     * `max_unconnected_peers` unconnected negotiations, or one of them holds no connection and is
     * dropped, the one idle longest. Otherwise the peer is refused, and logged once.
     */
    bool make_room_for(std::string_view peer) {
        std::size_t unconnected = 0;
        auto idle = peers_.end();
        for (auto it = peers_.begin(); it != peers_.end(); ++it) {
            const auto& n = it->second;
            if (n.connected) {
                continue;
            }
            ++unconnected;
            if (!n.offer.pc && !n.answer.pc &&
                (idle == peers_.end() || n.last_ms < idle->second.last_ms)) {
                idle = it;
            }
        }
        if (unconnected < max_unconnected_peers) {
            return true;
        }
        if (idle != peers_.end()) {
            peers_.erase(idle);
            return true;
        }
        if (first_refusal(peer)) {
            log_("signal", "refusing ", peer, ": ", max_unconnected_peers,
                 " negotiations are under way, the next announcement may find a slot");
        }
        return false;
    }

    /** Whether `peer` is refused for the first time, among the last `remembered_refusals`. */
    bool first_refusal(std::string_view peer) {
        if (std::ranges::find(refused_, peer) != refused_.end()) {
            return false;
        }
        refused_.emplace_back(peer);
        if (refused_.size() > remembered_refusals) {
            refused_.pop_front();
        }
        return true;
    }

    /**
     * Makes a connection for the negotiation with `peer`, within `max_connections_per_window`:
     * null beyond it, logged once per peer, or when the backend makes none, reported.
     */
    std::shared_ptr<backend::peer_connection> make_connection(std::string_view peer) {
        if (!constructions_.take(factory_.now_ms())) {
            if (first_refusal(peer)) {
                log_("signal", "refusing ", peer, ": ", max_connections_per_window,
                     " connections were made in the last ", offer_ttl.count(),
                     " ms, a later announcement may find one");
            }
            return nullptr;
        }
        auto pc = factory_.make_peer_connection(ice_servers_, memory_);
        if (!pc) {
            refused(peer);
        }
        return pc;
    }

    /** Drops a negotiation that holds no connection any more and is not connected. */
    void drop_if_done(std::string_view peer) {
        if (const auto it = peers_.find(peer); it != peers_.end() && !it->second.connected &&
                                               !it->second.offer.pc && !it->second.answer.pc) {
            peers_.erase(it);
        }
    }

    void on_announcement(std::size_t relay, std::string_view peer, negotiation& n) {
        if (n.answer.pc || n.answered) {
            return;
        }
        if (self_id_ > peer) {
            if (n.offer.pc) {
                return;
            }
            const double now = factory_.now_ms();
            if (const auto it = n.counter_announced_ms.find(relay);
                it != n.counter_announced_ms.end() && now - it->second < 1000) {
                return;
            }
            // Room-wide too: a flood of fresh ids below ours must not have the relay rate-limit
            // the room. A peer not asked offers on our next announcement instead.
            if (const auto it = counter_announced_ms_.find(relay);
                it != counter_announced_ms_.end() &&
                now - it->second < static_cast<double>(counter_announce_interval.count())) {
                return;
            }
            counter_announced_ms_[relay] = now;
            n.counter_announced_ms[relay] = now;
            log_("signal", "announcement from ", peer, ": their id is lower, asking them to offer");
            cb_.publish(relay, peer_topic(peer),
                        protocol::write_signal({.peer_id = self_id_}, {}, memory_));
            return;
        }
        if (n.offer_relays.contains(relay)) {
            return;
        }
        n.offer_relays.insert(relay);
        log_("signal", "announcement from ", peer, ": our id is lower, so we offer");
        if (!n.offer.pc) {
            start_offer(peer, n);
        } else if (!n.sealed_offer.empty()) {
            send_offer(relay, peer, n);
        }
    }

    void start_offer(std::string_view peer, negotiation& n) {
        n.offer_id = make_id(protocol::offer_id_size);
        n.offer.pc = make_connection(peer);
        if (!n.offer.pc) {
            reset_offer(n);
            return;
        }
        n.offer.channel = n.offer.pc->create_data_channel("data");
        if (!n.offer.channel) {
            reset_offer(n);
            refused(peer);
            return;
        }
        const auto pc = n.offer.pc;
        const auto weak = weak_from_this();
        pc->set_handlers({
            .ice_candidate = bind<void(std::string_view)>(
                [weak, peer = copy(peer), pc](std::string_view candidate) {
                    if (const auto self = weak.lock()) {
                        self->on_local_candidate(peer, pc, candidate);
                    }
                }),
            .data_channel = {},
            .state = bind<void(backend::connection_state)>(
                [weak, peer = copy(peer), pc](backend::connection_state state) {
                    if (const auto self = weak.lock()) {
                        self->on_state(peer, pc, state);
                    }
                }),
        });
        watch_channel(peer, pc, n.offer.channel);
        n.offer_expiry = factory_.make_timer(memory_);
        n.offer_expiry->start(offer_ttl, bind<void()>([weak, peer = copy(peer), pc] {
                                  const auto self = weak.lock();
                                  auto* m = self ? self->find(peer) : nullptr;
                                  if (m == nullptr || m->connected || m->offer.pc != pc) {
                                      return;
                                  }
                                  self->log_("signal", "offer to ", peer, " expired");
                                  reset_offer(*m);
                                  self->drop_if_done(peer);
                              }));
        pc->set_local_description(
            bind<void(boost::system::error_code, std::string_view, std::string_view)>(
                [weak, peer = copy(peer), pc](boost::system::error_code ec,
                                              std::string_view /*type*/, std::string_view sdp) {
                    if (const auto self = weak.lock()) {
                        self->on_offer_created(peer, pc, ec, sdp);
                    }
                }));
    }

    /** Seals the offer this side created, once its connection has its local description. */
    void on_offer_created(std::string_view peer,
                          const std::shared_ptr<backend::peer_connection>& pc,
                          boost::system::error_code ec, std::string_view sdp) {
        auto* m = find(peer);
        if (m == nullptr || m->offer.pc != pc) {
            return;
        }
        if (ec) {
            log_("webrtc", "could not create an offer for ", peer, ": ", ec);
            reset_offer(*m);
            return;
        }
        used_.insert(sdp);  // sent back to the room in a peer's name, it is refused
        const auto weak = weak_from_this();
        sealer_->seal(sdp, bind<void(boost::system::error_code, std::string_view)>(
                               [weak, peer = copy(peer), pc](boost::system::error_code ec,
                                                             std::string_view sealed) {
                                   if (const auto self = weak.lock()) {
                                       self->on_offer_sealed(peer, pc, ec, sealed);
                                   }
                               }));
    }

    /** Sends the sealed offer on every relay that asked for it, then the candidates it held. */
    void on_offer_sealed(std::string_view peer, const std::shared_ptr<backend::peer_connection>& pc,
                         boost::system::error_code ec, std::string_view sealed) {
        auto* m = find(peer);
        if (m == nullptr || m->offer.pc != pc || ec) {
            return;
        }
        m->sealed_offer = sealed;
        for (const auto relay : m->offer_relays) {
            send_offer(relay, peer, *m);
        }
        for (const auto& candidate : std::exchange(m->offer_candidates, {})) {
            publish_candidate(peer, pc, candidate);
        }
    }

    void send_offer(std::size_t relay, std::string_view peer, negotiation& n) {
        log_("signal", "sending offer ", n.offer_id, " to ", peer);
        cb_.publish(relay, peer_topic(peer),
                    protocol::write_signal(
                        {.peer_id = self_id_, .offer_id = n.offer_id, .offer = n.sealed_offer}, {},
                        memory_));
        // Discovery can be quiet for a while: repeat the offer on this relay twice before giving up
        // on it.
        auto& retry = n.offer_retry[relay];
        if (!retry) {
            retry = factory_.make_timer(memory_);
        }
        const auto pc = n.offer.pc;
        const auto weak = weak_from_this();
        retry->start(n.offer_retries[relay] == 0 ? first_offer_retry : later_offer_retry,
                     bind<void()>([weak, peer = copy(peer), pc, relay] {
                         const auto self = weak.lock();
                         auto* m = self ? self->find(peer) : nullptr;
                         if (m == nullptr || m->connected || m->answered || m->offer.pc != pc) {
                             return;
                         }
                         if (++m->offer_retries[relay] > max_offer_retries) {
                             return;
                         }
                         self->send_offer(relay, peer, *m);
                     }));
    }

    void on_offer(std::size_t relay, std::string_view peer, std::string_view offer_id,
                  std::string_view offer, negotiation& n) {
        if (n.answer.pc || n.answered) {
            replay_answer(relay, peer, offer_id, offer, n);
            return;
        }
        if (n.offer.pc && self_id_ < peer) {
            return;  // we lead; our offer stands
        }
        // Nothing changes until the offer opens with the room key: a forged one, which anyone who
        // reads the relays can send in any peer's name, makes no connection and resets no offer.
        const auto weak = weak_from_this();
        sealer_->open(
            offer, bind<void(boost::system::error_code, std::string_view)>(
                       [weak, relay, peer = copy(peer), offer_id = copy(offer_id),
                        offer = copy(offer)](boost::system::error_code ec, std::string_view sdp) {
                           if (const auto self = weak.lock()) {
                               self->on_offer_opened(relay, peer, offer_id, offer, ec, sdp);
                           }
                       }));
    }

    /**
     * Answers an offer the sealer opened: makes the answering connection and applies the offer as
     * its remote description. An offer that did not open changes nothing.
     */
    void on_offer_opened(std::size_t relay, std::string_view peer, std::string_view offer_id,
                         std::string_view offer, boost::system::error_code ec,
                         std::string_view sdp) {
        auto* found = find(peer);
        if (found == nullptr || found->connected) {
            return;
        }
        auto& n = *found;
        if (ec) {
            failed_open(peer, n, "offer");
            return;
        }
        if (n.answer.pc || n.answered || (n.offer.pc && self_id_ < peer)) {
            return;  // another offer, or our own, came first
        }
        if (used_.contains(sdp)) {
            replayed(peer, n, "offer");
            return;
        }
        auto made = make_connection(peer);
        if (!made) {
            return;
        }
        used_.insert(sdp);
        if (n.offer.pc) {
            reset_offer(n);
        }
        log_("signal", "offer ", offer_id, " from ", peer);
        n.answer.pc = std::move(made);
        n.answered_offer_id = offer_id;
        n.answered_offer = offer;
        n.answer_relays.insert(relay);
        n.answer_replayed_ms[relay] = factory_.now_ms();
        const auto pc = n.answer.pc;
        const auto weak = weak_from_this();
        pc->set_handlers({
            .ice_candidate = bind<void(std::string_view)>(
                [weak, peer = copy(peer), pc](std::string_view candidate) {
                    if (const auto self = weak.lock()) {
                        self->on_local_candidate(peer, pc, candidate);
                    }
                }),
            .data_channel = bind<void(std::shared_ptr<backend::data_channel>)>(
                [weak, peer = copy(peer),
                 pc](const std::shared_ptr<backend::data_channel>& channel) {
                    const auto self = weak.lock();
                    auto* m = self ? self->find(peer) : nullptr;
                    if (m == nullptr || m->answer.pc != pc) {
                        return;
                    }
                    m->answer.channel = channel;
                    self->watch_channel(peer, pc, channel);
                }),
            .state = bind<void(backend::connection_state)>(
                [weak, peer = copy(peer), pc](backend::connection_state state) {
                    if (const auto self = weak.lock()) {
                        self->on_state(peer, pc, state);
                    }
                }),
        });
        n.answer_expiry = factory_.make_timer(memory_);
        n.answer_expiry->start(answering_ttl, bind<void()>([weak, peer = copy(peer), pc] {
                                   const auto self = weak.lock();
                                   auto* m = self ? self->find(peer) : nullptr;
                                   if (m == nullptr || m->connected || m->answer.pc != pc) {
                                       return;
                                   }
                                   self->log_("signal", "answering ", peer, " timed out");
                                   reset_answer(*m);
                                   self->drop_if_done(peer);
                               }));
        pc->set_remote_description("offer", sdp,
                                   bind<void(boost::system::error_code)>(
                                       [weak, peer = copy(peer), pc](boost::system::error_code ec) {
                                           if (const auto self = weak.lock()) {
                                               self->on_offer_applied(peer, pc, ec);
                                           }
                                       }));
    }

    /** The same offer again: Trystero replays its answer on that relay, at most once a second. */
    void replay_answer(std::size_t relay, std::string_view peer, std::string_view offer_id,
                       std::string_view offer, negotiation& n) {
        const double now = factory_.now_ms();
        const auto last = n.answer_replayed_ms.find(relay);
        if (!n.answer.pc || n.answered_offer != offer || n.answered_offer_id != offer_id ||
            (last != n.answer_replayed_ms.end() && now - last->second < 1000)) {
            return;
        }
        n.answer_replayed_ms[relay] = now;
        n.answer_relays.insert(relay);
        const auto topic = peer_topic(peer);
        for (const auto& message : n.answer_messages) {
            cb_.publish(relay, topic, message);
        }
    }

    /** Creates the answer once the offer is the connection's remote description. */
    void on_offer_applied(std::string_view peer,
                          const std::shared_ptr<backend::peer_connection>& pc,
                          boost::system::error_code ec) {
        auto* m = find(peer);
        if (m == nullptr || m->answer.pc != pc) {
            return;
        }
        if (ec) {
            log_("webrtc", "the offer from ", peer, " was refused: ", ec);
            reset_answer(*m);
            return;
        }
        flush_remote_candidates(peer, m->answer);
        const auto weak = weak_from_this();
        pc->set_local_description(
            bind<void(boost::system::error_code, std::string_view, std::string_view)>(
                [weak, peer = copy(peer), pc](boost::system::error_code ec,
                                              std::string_view /*type*/, std::string_view sdp) {
                    if (const auto self = weak.lock()) {
                        self->on_answer_created(peer, pc, ec, sdp);
                    }
                }));
    }

    /** Seals the answer this side created. */
    void on_answer_created(std::string_view peer,
                           const std::shared_ptr<backend::peer_connection>& pc,
                           boost::system::error_code ec, std::string_view sdp) {
        const auto* m = find(peer);
        if (m == nullptr || m->answer.pc != pc || ec) {
            return;
        }
        used_.insert(sdp);  // sent back to the room in a peer's name, it is refused
        const auto weak = weak_from_this();
        sealer_->seal(sdp, bind<void(boost::system::error_code, std::string_view)>(
                               [weak, peer = copy(peer), pc](boost::system::error_code ec,
                                                             std::string_view sealed) {
                                   if (const auto self = weak.lock()) {
                                       self->on_answer_sealed(peer, pc, ec, sealed);
                                   }
                               }));
    }

    /** Sends the sealed answer on every relay the offer came through, then the candidates it
     * held. */
    void on_answer_sealed(std::string_view peer,
                          const std::shared_ptr<backend::peer_connection>& pc,
                          boost::system::error_code ec, std::string_view sealed) {
        auto* m = find(peer);
        if (m == nullptr || m->answer.pc != pc || ec) {
            return;
        }
        log_("signal", "sending answer to ", peer);
        send_answer_message(peer, *m,
                            protocol::write_signal(
                                {
                                    .peer_id = self_id_,
                                    .offer_id = m->answered_offer_id,
                                    .answer = sealed,
                                },
                                {}, memory_));
        m->answer_sent = true;
        for (const auto& candidate : std::exchange(m->answer_candidates, {})) {
            publish_candidate(peer, pc, candidate);
        }
    }

    void send_answer_message(std::string_view peer, negotiation& n, std::pmr::string message) {
        const auto topic = peer_topic(peer);
        for (const auto relay : n.answer_relays) {
            cb_.publish(relay, topic, message);
        }
        n.answer_messages.push_back(std::move(message));
    }

    void on_answer(std::string_view peer, std::string_view offer_id, std::string_view answer,
                   const negotiation& n) {
        if (!n.offer.pc || n.answered || (!offer_id.empty() && offer_id != n.offer_id)) {
            return;
        }
        // Nothing changes until the answer opens with the room key: a forged one, sent in this
        // peer's name, must not reset the offer the peer's own answer is on its way to.
        const auto pc = n.offer.pc;
        const auto weak = weak_from_this();
        sealer_->open(answer, bind<void(boost::system::error_code, std::string_view)>(
                                  [weak, peer = copy(peer), pc](boost::system::error_code ec,
                                                                std::string_view sdp) {
                                      if (const auto self = weak.lock()) {
                                          self->on_answer_opened(peer, pc, ec, sdp);
                                      }
                                  }));
    }

    /** Applies an answer the sealer opened to our offer. An answer that did not open changes
     * nothing. */
    void on_answer_opened(std::string_view peer,
                          const std::shared_ptr<backend::peer_connection>& pc,
                          boost::system::error_code ec, std::string_view sdp) {
        auto* m = find(peer);
        if (m == nullptr || m->offer.pc != pc || m->answered || m->connected) {
            return;
        }
        if (ec) {
            failed_open(peer, *m, "answer");
            return;
        }
        if (used_.contains(sdp)) {
            replayed(peer, *m, "answer");
            return;
        }
        used_.insert(sdp);
        m->answered = true;
        const auto weak = weak_from_this();
        m->offer_expiry->start(answered_offer_ttl, bind<void()>([weak, peer = copy(peer), pc] {
                                   const auto self = weak.lock();
                                   auto* n = self ? self->find(peer) : nullptr;
                                   if (n == nullptr || n->connected || n->offer.pc != pc) {
                                       return;
                                   }
                                   self->log_("signal", peer,
                                              " answered but the connection never opened");
                                   reset_offer(*n);
                                   self->drop_if_done(peer);
                               }));
        log_("signal", "answer from ", peer);
        pc->set_remote_description("answer", sdp,
                                   bind<void(boost::system::error_code)>(
                                       [weak, peer = copy(peer), pc](boost::system::error_code ec) {
                                           const auto self = weak.lock();
                                           auto* n = self ? self->find(peer) : nullptr;
                                           if (n == nullptr || n->offer.pc != pc) {
                                               return;
                                           }
                                           if (ec) {
                                               self->log_("webrtc", "the answer from ", peer,
                                                          " was refused: ", ec);
                                               reset_offer(*n);
                                               return;
                                           }
                                           self->flush_remote_candidates(peer, n->offer);
                                       }));
    }

    /**
     * Counts a sealed signal from `peer` that did not open with the room key, and changes nothing
     * else: the peer used another password, or someone forged it in the peer's name. The first of
     * a negotiation is logged and reported as `wrong_password`; the rest only counted. Reports are
     * paced: the first of a quiet interval goes at once, with its peer, and the others of the
     * interval in one report when it ends, with no peer and their number.
     *
     * Tip: anyone who reads the relays can send garbage from any number of fresh ids, each a
     * negotiation of its own; the application then gets one report per interval, not one per id.
     */
    void failed_open(std::string_view peer, negotiation& n, std::string_view what) {
        ++failed_opens_;
        if (std::exchange(n.failed_open_reported, true)) {
            return;
        }
        log_("signal", "could not open the ", what, " from ", peer,
             ": another password, or forged in its name; ignored");
        const double now = factory_.now_ms();
        const auto interval = static_cast<double>(failed_open_report_interval.count());
        if (unreported_failed_opens_ == 0 && now - failed_open_reported_ms_ >= interval) {
            failed_open_reported_ms_ = now;
            cb_.failed(peer, make_error_code(error::wrong_password), {});
            return;
        }
        if (unreported_failed_opens_++ != 0) {
            return;  // the report of the interval is already due
        }
        failed_open_report_ = factory_.make_timer(memory_);
        const auto delay = std::chrono::milliseconds(
            static_cast<long long>(std::max(0.0, failed_open_reported_ms_ + interval - now)));
        failed_open_report_->start(delay, bind<void()>([weak = weak_from_this()] {
                                       if (const auto self = weak.lock()) {
                                           self->report_failed_opens();
                                       }
                                   }));
    }

    /** Reports, in one, the negotiations whose signals did not open since the last report. */
    void report_failed_opens() {
        const auto count = std::exchange(unreported_failed_opens_, 0);
        if (closed_ || count == 0) {
            return;
        }
        failed_open_reported_ms_ = factory_.now_ms();
        std::pmr::string what(memory_);
        protocol::json::append_integer(what, static_cast<std::int64_t>(count));
        what += " more peers sent signals that did not open with the room key, in the last ";
        protocol::json::append_integer(what, failed_open_report_interval.count() / 1000);
        what += " s: another password, or forged in their names";
        cb_.failed({}, make_error_code(error::wrong_password), what);
    }

    /**
     * Counts an offer or an answer from `peer` that opened but that the room made, or acted on
     * before, and changes nothing else: someone who read the relays sent it in the peer's name, or
     * the peer resent one the room already answered. The first of a negotiation is logged.
     *
     * Tip: it is not reported to `room::on_error`, whose errors are Trystero's: a resent signal
     * proves nothing about the peer whose name it carries.
     */
    void replayed(std::string_view peer, negotiation& n, std::string_view what) {
        ++replayed_;
        if (std::exchange(n.replay_logged, true)) {
            return;
        }
        log_("signal", "refused the ", what, " from ", peer,
             ": the room made or used it before, so someone sent it again, maybe in its name");
    }

    void on_candidate(std::string_view peer, std::string_view offer_id_view,
                      std::string_view candidate) {
        const auto weak = weak_from_this();
        sealer_->open(
            candidate,
            bind<void(boost::system::error_code, std::string_view)>(
                [weak, peer = copy(peer), offer_id = copy(offer_id_view)](
                    boost::system::error_code ec, std::string_view plain) {
                    const auto self = weak.lock();
                    auto* m = self ? self->find(peer) : nullptr;
                    if (m == nullptr || m->connected) {
                        return;
                    }
                    if (ec) {
                        self->failed_open(peer, *m, "candidate");
                        return;
                    }
                    // A candidate names the offer it belongs to; one that names none
                    // goes to the connection this side answers, else to its offer.
                    const bool answers =
                        m->answer.pc && (offer_id.empty() || offer_id == m->answered_offer_id);
                    const bool offers =
                        m->offer.pc && (offer_id.empty() ? !answers : offer_id == m->offer_id);
                    side* target = nullptr;
                    if (offers) {
                        target = &m->offer;
                    } else if (answers) {
                        target = &m->answer;
                    }
                    if (target == nullptr) {
                        return;
                    }
                    if (!target->pc->has_remote_description()) {
                        if (target->pending_remote.size() < max_pending_candidates) {
                            target->pending_remote.emplace_back(plain);
                        }
                        return;
                    }
                    target->pc->add_ice_candidate(plain, self->candidate_added(peer));
                }));
    }

    void flush_remote_candidates(std::string_view peer, side& s) {
        for (const auto& candidate : std::exchange(s.pending_remote, {})) {
            s.pc->add_ice_candidate(candidate, candidate_added(peer));
        }
    }

    /**
     * What a remote candidate completes with: one the backend refused, as a peer's text that is
     * not a candidate, is dropped and logged, and the negotiation goes on with the others.
     */
    handler<void(boost::system::error_code)> candidate_added(std::string_view peer) {
        return bind<void(boost::system::error_code)>(
            [weak = weak_from_this(), peer = copy(peer)](boost::system::error_code ec) {
                if (const auto self = weak.lock(); self && ec) {
                    self->log_("signal", "dropped a candidate from ", peer, ": ", ec);
                }
            });
    }

    void on_local_candidate(std::string_view peer_view,
                            const std::shared_ptr<backend::peer_connection>& pc,
                            std::string_view candidate) {
        const auto peer = copy(peer_view);
        auto* n = find(peer);
        if (n == nullptr || n->connected) {
            return;
        }
        if (pc == n->offer.pc) {
            if (n->sealed_offer.empty()) {
                n->offer_candidates.emplace_back(candidate);
            } else {
                publish_candidate(peer, pc, candidate);
            }
        } else if (pc == n->answer.pc) {
            if (!n->answer_sent) {
                n->answer_candidates.emplace_back(candidate);
            } else {
                publish_candidate(peer, pc, candidate);
            }
        }
    }

    void publish_candidate(std::string_view peer,
                           const std::shared_ptr<backend::peer_connection>& pc,
                           std::string_view candidate) {
        const auto weak = weak_from_this();
        sealer_->seal(candidate, bind<void(boost::system::error_code, std::string_view)>(
                                     [weak, peer = copy(peer), pc](boost::system::error_code ec,
                                                                   std::string_view sealed) {
                                         const auto self = weak.lock();
                                         auto* m = self ? self->find(peer) : nullptr;
                                         if (m == nullptr || ec || m->connected) {
                                             return;
                                         }
                                         if (pc == m->offer.pc) {
                                             const auto message = protocol::write_signal(
                                                 {
                                                     .peer_id = self->self_id_,
                                                     .offer_id = m->offer_id,
                                                     .candidate = sealed,
                                                 },
                                                 {}, self->memory_);
                                             const auto topic = self->peer_topic(peer);
                                             for (const auto relay : m->offer_relays) {
                                                 self->cb_.publish(relay, topic, message);
                                             }
                                         } else if (pc == m->answer.pc) {
                                             self->send_answer_message(
                                                 peer, *m,
                                                 protocol::write_signal(
                                                     {
                                                         .peer_id = self->self_id_,
                                                         .offer_id = m->answered_offer_id,
                                                         .candidate = sealed,
                                                     },
                                                     {}, self->memory_));
                                         }
                                     }));
    }

    void watch_channel(std::string_view peer, const std::shared_ptr<backend::peer_connection>& pc,
                       const std::shared_ptr<backend::data_channel>& channel) {
        const auto weak = weak_from_this();
        channel->set_handlers({
            .open = bind<void()>([weak, peer = copy(peer), pc] {
                if (const auto self = weak.lock()) {
                    self->on_channel_open(peer, pc);
                }
            }),
            .message = {},
            .close = bind<void()>([weak, peer = copy(peer), pc] {
                if (const auto self = weak.lock()) {
                    self->on_state(peer, pc, backend::connection_state::closed);
                }
            }),
            .buffered_amount_low = {},
        });
        if (channel->is_open()) {
            on_channel_open(peer, pc);
        }
    }

    void on_channel_open(std::string_view peer_view,
                         const std::shared_ptr<backend::peer_connection>& pc) {
        const auto peer = copy(peer_view);
        auto* n = find(peer);
        if (n == nullptr || n->connected) {
            return;
        }
        side* winner = nullptr;
        if (pc == n->offer.pc) {
            winner = &n->offer;
        } else if (pc == n->answer.pc) {
            winner = &n->answer;
        }
        if (winner == nullptr || !winner->channel) {
            return;
        }
        n->connected = true;
        auto connected_pc = std::move(winner->pc);
        auto channel = std::move(winner->channel);
        reset_offer(*n);
        reset_answer(*n);
        log_("webrtc", "data channel open with ", peer);
        cb_.connected(peer, std::move(connected_pc), std::move(channel));
    }

    void on_state(std::string_view peer_view, const std::shared_ptr<backend::peer_connection>& pc,
                  backend::connection_state state) {
        const auto peer = copy(peer_view);
        auto* n = find(peer);
        if (n == nullptr || n->connected) {
            return;
        }
        if (state != backend::connection_state::failed &&
            state != backend::connection_state::closed) {
            return;
        }
        if (pc == n->offer.pc) {
            log_("webrtc", "the connection we offered to ", peer, " failed");
            reset_offer(*n);
        } else if (pc == n->answer.pc) {
            log_("webrtc", "the connection we answered for ", peer, " failed");
            reset_answer(*n);
        }
    }

    static void close_side(side& s) {
        if (s.channel) {
            s.channel->set_handlers({});
        }
        if (s.pc) {
            s.pc->set_handlers({});
            s.pc->close();
        }
        s.pc.reset();
        s.channel.reset();
        s.pending_remote.clear();
    }

    /**
     * Reports that the backend made no connection, or no channel, for a negotiation with `peer`,
     * which ends: the next announcement starts another.
     */
    void refused(std::string_view peer) {
        log_("webrtc", "the backend made no connection for ", peer);
        cb_.failed(peer, make_error_code(error::backend_failure), {});
    }

    static void reset_offer(negotiation& n) {
        close_side(n.offer);
        n.offer_id.clear();
        n.sealed_offer.clear();
        n.answered = false;
        n.offer_relays.clear();
        n.offer_candidates.clear();
        n.offer_expiry.reset();
        n.offer_retry.clear();
        n.offer_retries.clear();
    }

    static void reset_answer(negotiation& n) {
        close_side(n.answer);
        n.answered_offer_id.clear();
        n.answered_offer.clear();
        n.answer_sent = false;
        n.answer_messages.clear();
        n.answer_relays.clear();
        n.answer_candidates.clear();
        n.answer_replayed_ms.clear();
        n.answer_expiry.reset();
    }

    backend::factory& factory_;
    std::pmr::memory_resource* memory_;
    boost::json::storage_ptr json_;
    std::shared_ptr<backend::sealer> sealer_;
    std::pmr::string app_id_;
    std::pmr::string room_id_;
    std::pmr::string self_id_;
    std::pmr::vector<backend::ice_server> ice_servers_;
    callbacks cb_;
    logger log_;
    std::pmr::map<std::pmr::string, negotiation, std::less<>> peers_;
    std::pmr::deque<std::pmr::string> refused_{memory_};
    construction_window constructions_{max_connections_per_window, offer_ttl, memory_};
    std::pmr::map<std::size_t, double> counter_announced_ms_{memory_};  // per relay
    replay_cache used_{remembered_descriptions, memory_};
    std::size_t failed_opens_ = 0;
    // Negotiations whose failed open waits for the next report, when it was last made, and the
    // timer of the next.
    std::size_t unreported_failed_opens_ = 0;
    double failed_open_reported_ms_ = -std::numeric_limits<double>::infinity();
    std::shared_ptr<backend::timer> failed_open_report_;
    std::size_t replayed_ = 0;
    bool closed_ = false;
};

}  // namespace webcpp::trystero::detail

#endif
