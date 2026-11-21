// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_SIGNALING_HPP
#define WEBCPP_TRYSTERO_DETAIL_SIGNALING_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/clock.hpp>
#include <webcpp/trystero/detail/construction_window.hpp>
#include <webcpp/trystero/detail/ice_servers.hpp>
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/detail/random_id.hpp>
#include <webcpp/trystero/detail/replay_cache.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/seal.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>
#include <webcpp/trystero/room_error.hpp>

#include <webcpp/crypto/secret.hpp>

#include <webcpp/document/parse.hpp>

#include <boost/assert.hpp>
#include <boost/system/result.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/**
 * When each key was last reported, for at most `capacity` keys, the least recently reported or
 * looked up forgotten first, as 0.27.0's `createBoundedCache` keeps its last decrypt errors: a key
 * is reported again only once `interval` has passed since its last report.
 */
class report_pacer {
public:
    /** Remembers nothing yet, in `memory`. */
    report_pacer(std::size_t capacity, std::chrono::milliseconds interval,
                 std::pmr::memory_resource* memory)
        : capacity_(capacity), interval_(interval), order_(memory), at_(memory) {}

    /**
     * Whether `key` is reported at `now`: it was never reported, or it was forgotten, or its last
     * report is `interval` old or older; when it is, `now` becomes its last report. Either way it
     * becomes the most recent key, as 0.27.0's cache refreshes a key it reads.
     */
    bool due(std::string_view key, time_point now) {
        if (const auto it = at_.find(key); it != at_.end()) {
            order_.splice(order_.end(), order_, it->second.place);
            if (now - it->second.reported < interval_) {
                return false;
            }
            it->second.reported = now;
            return true;
        }
        if (capacity_ == 0) {
            return true;
        }
        if (at_.size() >= capacity_) {
            at_.erase(std::string_view(order_.front()));
            order_.pop_front();
        }
        // A list never moves what it holds, so the view stays valid until its key is forgotten.
        const std::string_view stored = order_.emplace_back(key);
        at_.emplace(stored, entry{.reported = now, .place = std::prev(order_.end())});
        return true;
    }

private:
    struct entry {
        time_point reported;
        std::pmr::list<std::pmr::string>::iterator place;
    };

    std::size_t capacity_;
    std::chrono::milliseconds interval_;
    // The keys, the least recent first.
    std::pmr::list<std::pmr::string> order_;
    std::pmr::map<std::string_view, entry, std::less<>> at_;
};

/**
 * The negotiation with each peer seen on the signaling topics, after Trystero's signal handler.
 *
 * | Message      | What this side does                                                            |
 * | announcement | the lower peer id offers on the relay that carried it; the higher one asks the |
 * |              | other to offer by announcing itself on the other's topic, at most once a       |
 * |              | second per peer and once in 5.3 s per relay for the whole room                 |
 * | offer        | answers it, unless it already answers one, or offers itself and its id is      |
 * |              | lower; the same offer through another relay has the answer sent there too      |
 * | answer       | applies it to the matching offer                                               |
 * | candidate    | adds it to the connection its offer id names, or keeps it until that           |
 * |              | connection has a remote description                                            |
 *
 * An offer, an answer and every candidate travel sealed under the room's AES key, with the AAD of
 * their type, their sender and their offer id (protocol/seal.hpp), sealed and opened at once, in
 * the call, as 0.27.0's `encrypt` and `decrypt` do them in WebCrypto. A negotiation ends when the
 * data channel opens: the connection is handed to the room and signals from that peer are ignored
 * until it leaves.
 *
 * Announcements are not sealed, so anyone who knows the app and room ids can announce any number
 * of peer ids, and anyone who reads the relays can send any signal in any peer's name. Nothing a
 * peer sends changes a negotiation before it opens with the room key: a signal that does not open
 * is counted and changes nothing else, and an offer or an answer that does not open is reported
 * as `decrypt_failed` at most once a minute per peer and stage, as 0.27.0 reports it
 * (`decrypt_report_interval`). The AAD binds a sealed signal to its type, its sender and its
 * offer id, so that one recorded from the relays opens only as what
 * it was: the same signal of the same sender for the same offer, never another's, nor an answer
 * sent back as an offer. Resent so, it still opens: the room remembers the offers and answers it
 * made or acted on (`remembered_descriptions`) and refuses one that comes to it again, which it
 * logs. The AAD does not name the receiver, and every peer seals a fresh offer for any id
 * announced above its own: so anyone who reads and publishes on the relays, without the key, can
 * still announce ids and answer nothing, though it can neither read nor join anything, nor touch
 * joined peers. The negotiations under way are bounded (`max_unconnected_peers`): a negotiation is
 * dropped when its offer and its answer expire without a connection, an idle one (no connection)
 * gives its place to a new peer, and beyond the bound a new peer is refused, logged once. The
 * connections made are bounded too (`max_connections_per_window`), however soon they end, since a
 * browser counts a closed one until its garbage collector runs. Joined peers, memory and
 * connections stay safe under a flood; new peers do not: a flooder who keeps sending fresh ids
 * takes each place, and each connection of the window, as it frees, and keeps new peers out for as
 * long as the flood lasts. A joined peer whose connection drops is a new peer when it comes back,
 * with the same id, and is kept out the same way: there is no renegotiation of a joined peer's
 * connection. Trystero bounds none of this.
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
     * most two pending connections: a peer beyond it waits for its next announcement, or its offer
     * sent again.
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
        /** Whether the room claims a signal over an existing app connection. */
        handler<bool(std::string_view peer)> claim;
        /** Publishes a signal on a topic, through the relay given. */
        handler<void(std::size_t relay, std::string_view topic, std::string_view content)> publish;
        /** A negotiation ended with an open data channel: the connection is the room's. */
        handler<void(std::string_view peer, std::shared_ptr<backend::peer_connection>,
                     std::shared_ptr<backend::data_channel>)>
            connected;
        /**
         * A negotiation with a peer failed: an offer or an answer did not open
         * (`error::decrypt_failed`), a connection did not open after the exchange of descriptions
         * (`error::peer_connect_failed`), or the backend made no connection
         * (`error::backend_failure`). The report's views are valid for the call; its room's id is
         * set, and its app's.
         */
        handler<void(const room_error&)> failed;
    };

    /** Every allocation goes through `memory`, the parsed signals' documents included, but the
     * peer connections, which are made in `connection_memory`, the app's: the
     * one a negotiation opens becomes the app's connection. */
    signaling(backend::factory& factory, std::string_view app_id, std::string_view room_id,
              std::string_view self_id, std::span<const backend::ice_server> ice_servers,
              callbacks cb, logger log, std::pmr::memory_resource* memory,
              std::pmr::memory_resource* connection_memory)
        : factory_(factory),
          memory_(memory),
          connection_memory_(connection_memory),
          app_id_(app_id, memory),
          room_id_(room_id, memory),
          self_id_(self_id, memory),
          ice_servers_(copy_ice_servers(ice_servers, memory)),
          turn_configured_(has_turn_server(ice_servers)),
          cb_(std::move(cb)),
          log_(std::move(log)),
          peers_(memory) {}

    /**
     * Gives the signaling the room's AES key, which it seals and opens every signal under: the room
     * gives it before its strategy starts, so before the first message.
     *
     * @param key The room's AES key, copied.
     */
    void set_key(const webcpp::crypto::secret<32>& key) {
        std::ranges::copy(key.bytes(), key_.writable().begin());
        keyed_ = true;
    }

    /** A message on the root or the self topic; `content` is read during the call. */
    void handle(std::size_t relay, std::string_view content) {
        if (closed_) {
            return;
        }
        BOOST_ASSERT(keyed_);
        // A signal past its bound is dropped as a malformed one is, before it is read.
        const auto document = webcpp::document::parse(
            content, protocol::json::protocol_limits(protocol::max_parsed_signal_bytes),
            std::pmr::polymorphic_allocator<std::byte>(memory_));
        const auto message =
            document.has_value() ? protocol::read_signal(document->root()) : std::nullopt;
        if (!message || message->peer_id == self_id_) {
            return;
        }
        if (cb_.claim && cb_.claim(message->peer_id)) {
            return;
        }
        auto it = peers_.find(message->peer_id);
        if (it == peers_.end()) {
            if (!make_room_for(message->peer_id)) {
                return;
            }
            it = admit(message->peer_id);
        }
        const std::string_view peer = it->first;
        auto& n = it->second;
        if (n.connected) {
            return;
        }
        n.last_message = steady_now(factory_);
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

    /** A room is bound to `peer`: its negotiation ends, its entry stays marked connected, and every
     * later signal of that peer is ignored until `forget`. */
    void bound(std::string_view peer) {
        auto it = peers_.find(peer);
        if (it == peers_.end()) {
            it = admit(peer);
        }
        reset_offer(it->second);
        reset_answer(it->second);
        it->second.connected = true;
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
    static constexpr std::uint32_t max_offer_retries = 2;
    static constexpr std::size_t max_pending_candidates = 128;
    /** The least time between two counter-announcements to one peer on a relay, and between two
     * replays of an answer on a relay: Trystero's second, for each. */
    static constexpr auto per_peer_interval = std::chrono::milliseconds(1'000);
    /** The least time between two counter-announcements of the room on a relay: Trystero's
     * `announceIntervalMs`, the pace at which it announces. */
    static constexpr auto counter_announce_interval = std::chrono::milliseconds(5'333);
    /** How many refused peer ids are remembered, so that each is logged once. */
    static constexpr std::size_t remembered_refusals = 64;
    /** The least time between two reports of an offer, or of an answer, of one peer that did not
     * open: 0.27.0's `decryptErrDedupeTtlMs`. */
    static constexpr auto decrypt_report_interval = std::chrono::milliseconds(60'000);
    /** How many peers and stages the last report of a signal that did not open is remembered for,
     * the least recently reported or seen forgotten first: 0.27.0's bound of `recentDecryptErrors`.
     */
    static constexpr std::size_t remembered_decrypt_failures = 512;

    /** One side of a negotiation: a connection, the remote candidates waiting for its remote
     * description. */
    struct side {
        explicit side(pmr_allocator a) : pending_remote(a) {}

        std::shared_ptr<backend::peer_connection> pc;
        std::shared_ptr<backend::data_channel> channel;
        std::pmr::vector<std::pmr::string> pending_remote;
    };

    struct negotiation {
        negotiation(pmr_allocator a, std::uint64_t admitted)
            : admitted(admitted),
              offer(a),
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
              answer_replayed(a),
              counter_announced(a) {}

        // Its place in the order the room took its negotiations in: of two idle as long, the one
        // taken first gives its place first.
        std::uint64_t admitted;
        bool connected = false;
        // When the peer last sent a message, for the eviction of an idle negotiation.
        time_point last_message{};
        // Whether a signal of this negotiation that did not open was logged.
        bool failed_open_logged = false;
        // Whether the failure of a connection of this negotiation to open after the exchange of
        // descriptions was reported, 0.27.0's `didReportFailure`: once per offer or answer.
        bool connect_failure_reported = false;
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
        std::pmr::map<std::size_t, std::uint32_t> offer_retries;
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
        std::pmr::map<std::size_t, time_point> answer_replayed;
        std::shared_ptr<backend::timer> answer_expiry;
        // Counter-announcements, per relay.
        std::pmr::map<std::size_t, time_point> counter_announced;
    };

    using negotiations = std::pmr::map<std::pmr::string, negotiation, std::less<>>;

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

    negotiation* find(std::string_view peer) {
        const auto it = peers_.find(peer);
        return it == peers_.end() ? nullptr : &it->second;
    }

    /** Starts a negotiation with `peer`, which has none. */
    negotiations::iterator admit(std::string_view peer) {
        return peers_.try_emplace(std::pmr::string(peer, memory_), allocator(), admissions_++)
            .first;
    }

    /** Whether a negotiation waits on nothing: it is not connected and holds no connection. */
    static bool idle(const negotiation& n) { return !n.connected && !n.offer.pc && !n.answer.pc; }

    /**
     * `plaintext` sealed as a signal of this side's, under the room key, with a fresh IV from the
     * backend's randomness, as 0.27.0's `toCipher` seals it.
     */
    boost::system::result<std::pmr::string> seal_signal(std::string_view type,
                                                        std::string_view offer_id,
                                                        std::string_view plaintext) {
        BOOST_ASSERT(keyed_);
        std::array<std::uint8_t, protocol::gcm_iv_size> iv{};
        factory_.random(iv);
        const auto aad = protocol::signal_aad(type, self_id_, offer_id, memory_);
        return protocol::seal(key_, iv, plaintext, std::string_view(aad), memory_);
    }

    /** `sealed` opened as a signal of `peer`'s, under the room key, as 0.27.0's `toPlain` opens
     * it. */
    boost::system::result<std::pmr::string> open_signal(std::string_view type,
                                                        std::string_view peer,
                                                        std::string_view offer_id,
                                                        std::string_view sealed) const {
        const auto aad = protocol::signal_aad(type, peer, offer_id, memory_);
        return protocol::open(key_, sealed, std::string_view(aad), memory_);
    }

    /**
     * Whether a negotiation may start with a new peer: the room holds fewer than
     * `max_unconnected_peers` unconnected negotiations, or one of them is idle and is dropped, the
     * one idle longest, and of those idle as long the one taken first. Otherwise the peer is
     * refused, and logged once.
     */
    bool make_room_for(std::string_view peer) {
        std::size_t unconnected = 0;
        auto evicted = peers_.end();
        for (auto it = peers_.begin(); it != peers_.end(); ++it) {
            const auto& n = it->second;
            if (n.connected) {
                continue;
            }
            ++unconnected;
            if (idle(n) && (evicted == peers_.end() ||
                            std::tie(n.last_message, n.admitted) <
                                std::tie(evicted->second.last_message, evicted->second.admitted))) {
                evicted = it;
            }
        }
        if (unconnected < max_unconnected_peers) {
            return true;
        }
        if (evicted != peers_.end()) {
            peers_.erase(evicted);
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
        if (!constructions_.take(steady_now(factory_))) {
            if (first_refusal(peer)) {
                log_("signal", "refusing ", peer, ": ", max_connections_per_window,
                     " connections were made in the last ", offer_ttl.count(),
                     " ms, a later announcement may find one");
            }
            return nullptr;
        }
        auto pc = factory_.make_peer_connection(ice_servers_, connection_memory_);
        if (!pc) {
            refused(peer);
        }
        return pc;
    }

    /** Drops a negotiation that is idle: no connection. */
    void drop_if_done(std::string_view peer) {
        if (const auto it = peers_.find(peer); it != peers_.end() && idle(it->second)) {
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
            const auto now = steady_now(factory_);
            if (const auto it = n.counter_announced.find(relay);
                it != n.counter_announced.end() && now - it->second < per_peer_interval) {
                return;
            }
            // Room-wide too: a flood of fresh ids below ours must not have the relay rate-limit
            // the room. A peer not asked offers on our next announcement instead.
            if (const auto it = counter_announced_.find(relay);
                it != counter_announced_.end() && now - it->second < counter_announce_interval) {
                return;
            }
            counter_announced_[relay] = now;
            n.counter_announced[relay] = now;
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
        n.offer_id = random_id(factory_, protocol::offer_id_size, memory_);
        n.connect_failure_reported = false;
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
        auto sealed = seal_signal("offer", m->offer_id, sdp);
        if (!sealed) {
            log_("signal", "could not seal the offer for ", peer, ": ", sealed.error());
            reset_offer(*m);
            return;
        }
        // Sent on every relay that asked for it, then the candidates it held.
        m->sealed_offer = std::move(*sealed);
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
        // Nothing changes unless the offer opens with the room key: a forged one, which anyone who
        // reads the relays can send in any peer's name, makes no connection and resets no offer.
        // One that opens is answered at once, so that the same offer again, through this relay or
        // another, has the answer replayed (replay_answer), as Trystero's replay records its relay.
        const auto sdp = open_signal("offer", peer, offer_id, offer);
        if (!sdp) {
            failed_open(peer, n, signal_stage::offer);
            return;
        }
        if (used_.contains(*sdp)) {
            replayed(peer, n, "offer");
            return;
        }
        const auto made = make_connection(peer);
        if (!made) {
            return;
        }
        std::pmr::set<std::size_t> relays(memory_);
        relays.insert(relay);
        start_answer(peer, n, made, copy(offer_id), copy(offer), relays, *sdp);
    }

    /**
     * Answers the offer `offer_id`, sealed as `offer` and opened as `sdp`, through the connection
     * `pc`: it becomes the negotiation's answering one, the offer its remote description, and the
     * answer goes through every relay the offer came through.
     */
    void start_answer(std::string_view peer, negotiation& n,
                      const std::shared_ptr<backend::peer_connection>& pc,
                      std::pmr::string offer_id, std::pmr::string offer,
                      const std::pmr::set<std::size_t>& relays, std::string_view sdp) {
        used_.insert(sdp);
        if (n.offer.pc) {
            reset_offer(n);
        }
        log_("signal", "offer ", offer_id, " from ", peer);
        n.connect_failure_reported = false;
        n.answer.pc = pc;
        n.answered_offer_id = std::move(offer_id);
        n.answered_offer = std::move(offer);
        const auto now = steady_now(factory_);
        for (const auto relay : relays) {
            n.answer_relays.insert(relay);
            n.answer_replayed[relay] = now;
        }
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
                                   if (m->answer_sent) {
                                       self->report_connect_failed(peer, *m);
                                   }
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
        const auto now = steady_now(factory_);
        const auto last = n.answer_replayed.find(relay);
        if (!n.answer.pc || n.answered_offer != offer || n.answered_offer_id != offer_id ||
            (last != n.answer_replayed.end() && now - last->second < per_peer_interval)) {
            return;
        }
        n.answer_replayed[relay] = now;
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
        auto* m = find(peer);
        if (m == nullptr || m->answer.pc != pc || ec) {
            return;
        }
        used_.insert(sdp);  // sent back to the room in a peer's name, it is refused
        const auto sealed = seal_signal("answer", m->answered_offer_id, sdp);
        if (!sealed) {
            log_("signal", "could not seal the answer for ", peer, ": ", sealed.error());
            reset_answer(*m);
            return;
        }
        // Sent on every relay the offer came through, then the candidates it held.
        log_("signal", "sending answer to ", peer);
        send_answer_message(peer, *m,
                            protocol::write_signal(
                                {
                                    .peer_id = self_id_,
                                    .offer_id = m->answered_offer_id,
                                    .answer = *sealed,
                                    .answering = true,
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
                   negotiation& n) {
        if (!n.offer.pc || n.answered || (!offer_id.empty() && offer_id != n.offer_id)) {
            return;
        }
        // Nothing changes unless the answer opens with the room key: a forged one, sent in this
        // peer's name, must not reset the offer the peer's own answer is on its way to.
        const auto sdp = open_signal("answer", peer, offer_id, answer);
        if (!sdp) {
            failed_open(peer, n, signal_stage::answer);
            return;
        }
        if (used_.contains(*sdp)) {
            replayed(peer, n, "answer");
            return;
        }
        used_.insert(*sdp);
        const auto pc = n.offer.pc;
        n.answered = true;
        const auto weak = weak_from_this();
        n.offer_expiry->start(answered_offer_ttl, bind<void()>([weak, peer = copy(peer), pc] {
                                  const auto self = weak.lock();
                                  auto* m = self ? self->find(peer) : nullptr;
                                  if (m == nullptr || m->connected || m->offer.pc != pc) {
                                      return;
                                  }
                                  self->log_("signal", peer,
                                             " answered but the connection never opened");
                                  self->report_connect_failed(peer, *m);
                                  reset_offer(*m);
                                  self->drop_if_done(peer);
                              }));
        log_("signal", "answer from ", peer);
        pc->set_remote_description("answer", *sdp,
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
     * a negotiation is logged; an offer or an answer is reported as `decrypt_failed`, at most once
     * per `decrypt_report_interval` for each peer and stage, as 0.27.0's `reportDecryptErr`
     * reports it; a candidate is not reported, as 0.27.0 drops it unreported.
     *
     * Tip: anyone who reads the relays can send garbage from any number of fresh ids, and each id
     * is reported, as 0.27.0 reports each; the reports remembered are bounded, as 0.27.0's are.
     */
    void failed_open(std::string_view peer, negotiation& n, std::optional<signal_stage> stage) {
        ++failed_opens_;
        if (!std::exchange(n.failed_open_logged, true)) {
            log_("signal", "could not open the ", stage ? stage_name(*stage) : "candidate",
                 " from ", peer, ": another password, or forged in its name; ignored");
        }
        if (stage) {
            report_decrypt_failed(peer, *stage);
        }
    }

    /** The name 0.27.0 gives a signal of `stage`: "offer" or "answer". */
    static std::string_view stage_name(signal_stage stage) {
        return stage == signal_stage::offer ? "offer" : "answer";
    }

    /** Reports that `peer`'s signal of `stage` did not open, unless it was reported less than
     * `decrypt_report_interval` ago. */
    void report_decrypt_failed(std::string_view peer, signal_stage stage) {
        const std::string_view name = stage_name(stage);
        std::pmr::string key(name, memory_);
        key += ':';
        key += peer;
        if (!decrypt_reports_.due(key, steady_now(factory_))) {
            return;
        }
        std::pmr::string message("failed to decrypt ", memory_);
        message += name;
        message += " (incorrect password or corrupted signal)";
        room_error e = report_of(error::decrypt_failed, message, peer);
        e.stage = stage;
        cb_.failed(e);
    }

    /**
     * Reports that a connection with `peer` did not open after the exchange of descriptions, once
     * per offer or answer, as 0.27.0's `reportSdpFailure` does: an offer answered, or an answer
     * sent, whose connection expired, failed or closed before it opened.
     */
    void report_connect_failed(std::string_view peer, negotiation& n) {
        if (closed_ || n.connected || std::exchange(n.connect_failure_reported, true)) {
            return;
        }
        std::pmr::string message("could not connect to peer ", memory_);
        message += peer;
        message += " after exchanging SDP; ";
        message += turn_configured_ ? "verify TURN server URLs and credentials"
                                    : "configure TURN via app_config::ice_servers";
        room_error e = report_of(error::peer_connect_failed, message, peer);
        e.turn_configured = turn_configured_;
        cb_.failed(e);
    }

    /** A report of `code` about `peer`, with its message, the app's and the room's ids, and no
     * other context. */
    room_error report_of(error code, std::string_view message, std::string_view peer) const {
        room_error e;
        e.code = make_error_code(code);
        e.message = message;
        e.app_id = app_id_;
        e.room_id = room_id_;
        e.peer_id = peer;
        return e;
    }

    /** Whether any of `servers` has a `turn:` or `turns:` URL, in any case, as 0.27.0's
     * `hasTurnServer` reads them. */
    static bool has_turn_server(std::span<const backend::ice_server> servers) {
        const auto starts_with = [](std::string_view url, std::string_view scheme) {
            return url.size() >= scheme.size() &&
                   std::ranges::equal(url.substr(0, scheme.size()), scheme, [](char a, char b) {
                       return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
                   });
        };
        return std::ranges::any_of(servers, [&](const backend::ice_server& server) {
            return std::ranges::any_of(server.urls, [&](const std::pmr::string& url) {
                return starts_with(url, "turn:") || starts_with(url, "turns:");
            });
        });
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

    void on_candidate(std::string_view peer, std::string_view offer_id,
                      std::string_view candidate) {
        auto* m = find(peer);
        if (m == nullptr || m->connected) {
            return;
        }
        const auto plain = open_signal("candidate", peer, offer_id, candidate);
        if (!plain) {
            failed_open(peer, *m, std::nullopt);
            return;
        }
        // A candidate names the offer it belongs to; one that names none goes to the connection
        // this side answers, else to its offer.
        const bool answers = m->answer.pc && (offer_id.empty() || offer_id == m->answered_offer_id);
        const bool offers = m->offer.pc && (offer_id.empty() ? !answers : offer_id == m->offer_id);
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
                target->pending_remote.emplace_back(*plain);
            }
            return;
        }
        target->pc->add_ice_candidate(*plain, candidate_added(peer));
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
        auto* m = find(peer);
        if (m == nullptr || m->connected) {
            return;
        }
        if (pc == m->offer.pc) {
            const auto sealed = seal_signal("candidate", m->offer_id, candidate);
            if (!sealed) {
                return;
            }
            const auto message = protocol::write_signal(
                {.peer_id = self_id_, .offer_id = m->offer_id, .candidate = *sealed}, {}, memory_);
            const auto topic = peer_topic(peer);
            for (const auto relay : m->offer_relays) {
                cb_.publish(relay, topic, message);
            }
        } else if (pc == m->answer.pc) {
            const auto sealed = seal_signal("candidate", m->answered_offer_id, candidate);
            if (!sealed) {
                return;
            }
            send_answer_message(peer, *m,
                                protocol::write_signal(
                                    {
                                        .peer_id = self_id_,
                                        .offer_id = m->answered_offer_id,
                                        .candidate = *sealed,
                                        .answering = true,
                                    },
                                    {}, memory_));
        }
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
            if (n->answered) {
                report_connect_failed(peer, *n);
            }
            reset_offer(*n);
        } else if (pc == n->answer.pc) {
            log_("webrtc", "the connection we answered for ", peer, " failed");
            if (n->answer_sent) {
                report_connect_failed(peer, *n);
            }
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
        std::array<char, 128> buffer{};  // the message without a std::string
        cb_.failed(report_of(
            error::backend_failure,
            make_error_code(error::backend_failure).message(buffer.data(), buffer.size()), peer));
    }

    /** Ends the offer side; a failure to connect may be reported again afterwards, as 0.27.0's
     * `resetOfferState` clears `didReportFailure`. */
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
        n.connect_failure_reported = false;
    }

    static void reset_answer(negotiation& n) {
        close_side(n.answer);
        n.answered_offer_id.clear();
        n.answered_offer.clear();
        n.answer_sent = false;
        n.answer_messages.clear();
        n.answer_relays.clear();
        n.answer_candidates.clear();
        n.answer_replayed.clear();
        n.answer_expiry.reset();
    }

    backend::factory& factory_;
    std::pmr::memory_resource* memory_;
    std::pmr::memory_resource* connection_memory_;
    // The room's AES key, once the room gave it (set_key).
    webcpp::crypto::secret<32> key_;
    bool keyed_ = false;
    std::pmr::string app_id_;
    std::pmr::string room_id_;
    std::pmr::string self_id_;
    std::pmr::vector<backend::ice_server> ice_servers_;
    // Whether a TURN server is among them, which a report of a connection that did not open says.
    bool turn_configured_;
    callbacks cb_;
    logger log_;
    negotiations peers_;
    std::uint64_t admissions_ = 0;  // the negotiations the room took in, the next one's place
    std::pmr::deque<std::pmr::string> refused_{memory_};
    construction_window constructions_{max_connections_per_window, offer_ttl, memory_};
    std::pmr::map<std::size_t, time_point> counter_announced_{memory_};  // per relay
    replay_cache used_{remembered_descriptions, memory_};
    std::size_t failed_opens_ = 0;
    // The last report of each peer's offers and answers that did not open.
    report_pacer decrypt_reports_{remembered_decrypt_failures, decrypt_report_interval, memory_};
    std::size_t replayed_ = 0;
    bool closed_ = false;
};

}  // namespace webcpp::trystero::detail

#endif
