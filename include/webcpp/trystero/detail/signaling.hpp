// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_SIGNALING_HPP
#define WEBCPP_TRYSTERO_DETAIL_SIGNALING_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include <boost/json/parse.hpp>

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/**
 * The negotiation with each peer seen on the signaling topics, after Trystero's signal handler.
 *
 * | Message                 | What this side does                                                               |
 * | announcement            | the lower peer id offers on the relay that carried it; the higher one asks the    |
 * |                         | other to offer by announcing itself on the other's topic, at most once a second   |
 * | offer                   | answers it, unless it already answers one, or offers itself and its id is lower   |
 * | answer                  | applies it to the matching offer                                                  |
 * | candidate               | adds it to the connection its offer id names, or keeps it until that connection  |
 * |                         | has a remote description                                                          |
 *
 * An offer, an answer and every candidate travel sealed (protocol/signal.hpp). A negotiation ends when the data
 * channel opens: the connection is handed to the room and signals from that peer are ignored until it leaves.
 *
 * Tip: a peer id is copied once, into the map's key, and every asynchronous handler captures its own copy because it
 * may outlive the entry. The entry points a handler calls take their arguments by value, since they may reset the
 * very handler whose captures the arguments would otherwise refer to.
 */
class signaling : public std::enable_shared_from_this<signaling> {
public:
    struct callbacks {
        std::function<void(std::size_t relay, std::string_view topic, std::string_view content)> publish;
        std::function<void(std::string_view peer, std::shared_ptr<backend::peer_connection>, std::shared_ptr<backend::data_channel>)> connected;
        std::function<void(std::string_view peer, boost::system::error_code)> failed;
    };

    signaling(backend::factory& factory, std::shared_ptr<backend::sealer> sealer, std::string_view app_id, std::string_view room_id,
              std::string_view self_id, std::span<const backend::ice_server> ice_servers, callbacks cb, logger log)
        : factory_(factory),
          sealer_(std::move(sealer)),
          app_id_(app_id),
          room_id_(room_id),
          self_id_(self_id),
          ice_servers_(ice_servers.begin(), ice_servers.end()),
          cb_(std::move(cb)),
          log_(std::move(log)) {}

    /** A message on the root or the self topic; `content` is read during the call. */
    void handle(std::size_t relay, std::string_view content) {
        if (closed_) return;
        boost::system::error_code ec;
        const auto document = boost::json::parse(boost::json::string_view(content.data(), content.size()), ec);
        const auto message = ec ? std::nullopt : protocol::read_signal(document);
        if (!message || message->peer_id == self_id_) return;
        auto it = peers_.find(message->peer_id);
        if (it == peers_.end()) it = peers_.try_emplace(std::string(message->peer_id)).first;
        std::string_view peer = it->first;
        auto& n = it->second;
        if (n.connected) return;
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
        if (auto it = peers_.find(peer); it != peers_.end()) {
            reset_offer(it->second);
            reset_answer(it->second);
            peers_.erase(it);
        }
    }

    void close() {
        closed_ = true;
        for (auto& [id, n] : peers_) {
            reset_offer(n);
            reset_answer(n);
        }
        peers_.clear();
    }

private:
    static constexpr auto offer_ttl = std::chrono::milliseconds(57'333);
    static constexpr auto answered_offer_ttl = std::chrono::milliseconds(23'333);
    static constexpr auto answering_ttl = std::chrono::milliseconds(23'333);
    static constexpr auto first_offer_retry = std::chrono::milliseconds(2'000);
    static constexpr auto later_offer_retry = std::chrono::milliseconds(4'800);
    static constexpr int max_offer_retries = 2;
    static constexpr std::size_t max_pending_candidates = 128;

    /** One side of a negotiation: a connection, the remote candidates waiting for its remote description. */
    struct side {
        std::shared_ptr<backend::peer_connection> pc;
        std::shared_ptr<backend::data_channel> channel;
        std::vector<std::string> pending_remote;
    };

    struct negotiation {
        bool connected = false;
        // Our offer.
        side offer;
        std::string offer_id;
        std::string sealed_offer;  // empty until the offer is ready to send
        bool answered = false;
        std::set<std::size_t> offer_relays;
        std::vector<std::string> offer_candidates;  // plaintext, gathered before the offer went out
        std::unique_ptr<backend::timer> offer_expiry;
        std::map<std::size_t, std::unique_ptr<backend::timer>> offer_retry;
        std::map<std::size_t, int> offer_retries;
        // Our answer to their offer.
        side answer;
        std::string answered_offer_id;
        std::string answered_offer;
        bool answer_sent = false;
        std::vector<std::string> answer_messages;  // replayed when the same offer arrives again
        std::set<std::size_t> answer_relays;
        std::vector<std::string> answer_candidates;  // plaintext, gathered before the answer went out
        std::map<std::size_t, double> answer_replayed_ms;
        std::unique_ptr<backend::timer> answer_expiry;
        // Counter-announcements, per relay.
        std::map<std::size_t, double> counter_announced_ms;
    };

    std::string peer_topic(std::string_view peer) const { return protocol::peer_topic(app_id_, room_id_, peer); }

    std::string make_id(std::size_t n) {
        std::array<std::uint8_t, 32> random{};
        factory_.random(std::span(random).first(n));
        std::string id(n, '0');
        for (std::size_t i = 0; i < n; ++i) id[i] = protocol::id_alphabet[random[i] % protocol::id_alphabet.size()];
        return id;
    }

    negotiation* find(std::string_view peer) {
        auto it = peers_.find(peer);
        return it == peers_.end() ? nullptr : &it->second;
    }

    void on_announcement(std::size_t relay, std::string_view peer, negotiation& n) {
        if (n.answer.pc || n.answered) return;
        if (self_id_ > peer) {
            if (n.offer.pc) return;
            const double now = factory_.now_ms();
            if (auto it = n.counter_announced_ms.find(relay); it != n.counter_announced_ms.end() && now - it->second < 1000) return;
            n.counter_announced_ms[relay] = now;
            log_("signal", [&] { return "announcement from " + std::string(peer) + ": their id is lower, asking them to offer"; });
            cb_.publish(relay, peer_topic(peer), protocol::write_signal({.peer_id = self_id_}));
            return;
        }
        if (n.offer_relays.contains(relay)) return;
        n.offer_relays.insert(relay);
        log_("signal", [&] { return "announcement from " + std::string(peer) + ": our id is lower, so we offer"; });
        if (!n.offer.pc) {
            start_offer(peer, n);
        } else if (!n.sealed_offer.empty()) {
            send_offer(relay, peer, n);
        }
    }

    void start_offer(std::string_view peer, negotiation& n) {
        n.offer_id = make_id(protocol::offer_id_size);
        n.offer.pc = factory_.make_peer_connection(ice_servers_);
        n.offer.channel = n.offer.pc->create_data_channel("data");
        const auto pc = n.offer.pc;
        auto weak = weak_from_this();
        pc->set_handlers({
            .ice_candidate = [weak, peer = std::string(peer), pc](std::string_view candidate) {
                if (auto self = weak.lock()) self->on_local_candidate(peer, pc, std::string(candidate));
            },
            .data_channel = {},
            .state = [weak, peer = std::string(peer), pc](backend::connection_state state) {
                if (auto self = weak.lock()) self->on_state(peer, pc, state);
            },
        });
        watch_channel(peer, pc, n.offer.channel);
        n.offer_expiry = factory_.make_timer();
        n.offer_expiry->start(offer_ttl, [weak, peer = std::string(peer), pc] {
            auto self = weak.lock();
            auto* m = self ? self->find(peer) : nullptr;
            if (m == nullptr || m->connected || m->offer.pc != pc) return;
            self->log_("signal", [&] { return "offer to " + std::string(peer) + " expired"; });
            self->reset_offer(*m);
        });
        pc->set_local_description([weak, peer = std::string(peer), pc](boost::system::error_code ec, std::string_view, std::string_view sdp) {
            auto self = weak.lock();
            auto* m = self ? self->find(peer) : nullptr;
            if (m == nullptr || m->offer.pc != pc) return;
            if (ec) {
                self->log_("webrtc", [&] { return "could not create an offer for " + std::string(peer) + ": " + ec.message(); });
                self->reset_offer(*m);
                return;
            }
            self->sealer_->seal(sdp, [weak, peer = std::string(peer), pc](boost::system::error_code ec, std::string_view sealed) {
                auto self = weak.lock();
                auto* m = self ? self->find(peer) : nullptr;
                if (m == nullptr || m->offer.pc != pc || ec) return;
                m->sealed_offer = sealed;
                for (const auto relay : m->offer_relays) self->send_offer(relay, peer, *m);
                for (const auto& candidate : std::exchange(m->offer_candidates, {})) self->publish_candidate(peer, pc, candidate);
            });
        });
    }

    void send_offer(std::size_t relay, std::string_view peer, negotiation& n) {
        log_("signal", [&] { return "sending offer " + n.offer_id + " to " + std::string(peer); });
        cb_.publish(relay, peer_topic(peer), protocol::write_signal({.peer_id = self_id_, .offer_id = n.offer_id, .offer = n.sealed_offer}));
        // Discovery can be quiet for a while: repeat the offer on this relay twice before giving up on it.
        auto& retry = n.offer_retry[relay];
        if (!retry) retry = factory_.make_timer();
        const auto pc = n.offer.pc;
        auto weak = weak_from_this();
        retry->start(n.offer_retries[relay] == 0 ? first_offer_retry : later_offer_retry, [weak, peer = std::string(peer), pc, relay] {
            auto self = weak.lock();
            auto* m = self ? self->find(peer) : nullptr;
            if (m == nullptr || m->connected || m->answered || m->offer.pc != pc) return;
            if (++m->offer_retries[relay] > max_offer_retries) return;
            self->send_offer(relay, peer, *m);
        });
    }

    void on_offer(std::size_t relay, std::string_view peer, std::string_view offer_id, std::string_view offer, negotiation& n) {
        if (n.answer.pc || n.answered) {
            // The same offer again: Trystero replays its answer on that relay, at most once a second.
            const double now = factory_.now_ms();
            const auto last = n.answer_replayed_ms.find(relay);
            if (n.answer.pc && n.answered_offer == offer && n.answered_offer_id == offer_id &&
                (last == n.answer_replayed_ms.end() || now - last->second >= 1000)) {
                n.answer_replayed_ms[relay] = now;
                n.answer_relays.insert(relay);
                const auto topic = peer_topic(peer);
                for (const auto& message : n.answer_messages) cb_.publish(relay, topic, message);
            }
            return;
        }
        if (n.offer.pc && self_id_ < peer) return;  // we lead; our offer stands
        if (n.offer.pc) reset_offer(n);
        log_("signal", [&] { return "offer " + std::string(offer_id) + " from " + std::string(peer); });
        n.answer.pc = factory_.make_peer_connection(ice_servers_);
        n.answered_offer_id = offer_id;
        n.answered_offer = offer;
        n.answer_relays.insert(relay);
        n.answer_replayed_ms[relay] = factory_.now_ms();
        const auto pc = n.answer.pc;
        auto weak = weak_from_this();
        pc->set_handlers({
            .ice_candidate = [weak, peer = std::string(peer), pc](std::string_view candidate) {
                if (auto self = weak.lock()) self->on_local_candidate(peer, pc, std::string(candidate));
            },
            .data_channel = [weak, peer = std::string(peer), pc](std::shared_ptr<backend::data_channel> channel) {
                auto self = weak.lock();
                auto* m = self ? self->find(peer) : nullptr;
                if (m == nullptr || m->answer.pc != pc) return;
                m->answer.channel = channel;
                self->watch_channel(peer, pc, channel);
            },
            .state = [weak, peer = std::string(peer), pc](backend::connection_state state) {
                if (auto self = weak.lock()) self->on_state(peer, pc, state);
            },
        });
        n.answer_expiry = factory_.make_timer();
        n.answer_expiry->start(answering_ttl, [weak, peer = std::string(peer), pc] {
            auto self = weak.lock();
            auto* m = self ? self->find(peer) : nullptr;
            if (m == nullptr || m->connected || m->answer.pc != pc) return;
            self->log_("signal", [&] { return "answering " + std::string(peer) + " timed out"; });
            self->reset_answer(*m);
        });
        sealer_->open(offer, [weak, peer = std::string(peer), pc](boost::system::error_code ec, std::string_view sdp) {
            auto self = weak.lock();
            auto* m = self ? self->find(peer) : nullptr;
            if (m == nullptr || m->answer.pc != pc) return;
            if (ec) {
                self->log_("signal", [&] { return "could not open the offer from " + std::string(peer) + ": another password?"; });
                self->reset_answer(*m);
                self->cb_.failed(peer, make_error_code(error::wrong_password));
                return;
            }
            pc->set_remote_description("offer", sdp, [weak, peer = std::string(peer), pc](boost::system::error_code ec) {
                auto self = weak.lock();
                auto* m = self ? self->find(peer) : nullptr;
                if (m == nullptr || m->answer.pc != pc) return;
                if (ec) {
                    self->log_("webrtc", [&] { return "the offer from " + std::string(peer) + " was refused: " + ec.message(); });
                    self->reset_answer(*m);
                    return;
                }
                self->flush_remote_candidates(m->answer);
                pc->set_local_description([weak, peer = std::string(peer), pc](boost::system::error_code ec, std::string_view, std::string_view sdp) {
                    auto self = weak.lock();
                    auto* m = self ? self->find(peer) : nullptr;
                    if (m == nullptr || m->answer.pc != pc || ec) return;
                    self->sealer_->seal(sdp, [weak, peer = std::string(peer), pc](boost::system::error_code ec, std::string_view sealed) {
                        auto self = weak.lock();
                        auto* m = self ? self->find(peer) : nullptr;
                        if (m == nullptr || m->answer.pc != pc || ec) return;
                        self->log_("signal", [&] { return "sending answer to " + std::string(peer); });
                        self->send_answer_message(peer, *m, protocol::write_signal({.peer_id = self->self_id_, .offer_id = m->answered_offer_id, .answer = sealed}));
                        m->answer_sent = true;
                        for (const auto& candidate : std::exchange(m->answer_candidates, {})) self->publish_candidate(peer, pc, candidate);
                    });
                });
            });
        });
    }

    void send_answer_message(std::string_view peer, negotiation& n, std::string message) {
        const auto topic = peer_topic(peer);
        for (const auto relay : n.answer_relays) cb_.publish(relay, topic, message);
        n.answer_messages.push_back(std::move(message));
    }

    void on_answer(std::string_view peer, std::string_view offer_id, std::string_view answer, negotiation& n) {
        if (!n.offer.pc || n.answered || (!offer_id.empty() && offer_id != n.offer_id)) return;
        n.answered = true;
        const auto pc = n.offer.pc;
        auto weak = weak_from_this();
        n.offer_expiry->start(answered_offer_ttl, [weak, peer = std::string(peer), pc] {
            auto self = weak.lock();
            auto* m = self ? self->find(peer) : nullptr;
            if (m == nullptr || m->connected || m->offer.pc != pc) return;
            self->log_("signal", [&] { return std::string(peer) + " answered but the connection never opened"; });
            self->reset_offer(*m);
        });
        sealer_->open(answer, [weak, peer = std::string(peer), pc](boost::system::error_code ec, std::string_view sdp) {
            auto self = weak.lock();
            auto* m = self ? self->find(peer) : nullptr;
            if (m == nullptr || m->offer.pc != pc) return;
            if (ec) {
                self->log_("signal", [&] { return "could not open the answer from " + std::string(peer) + ": another password?"; });
                self->reset_offer(*m);
                self->cb_.failed(peer, make_error_code(error::wrong_password));
                return;
            }
            self->log_("signal", [&] { return "answer from " + std::string(peer); });
            pc->set_remote_description("answer", sdp, [weak, peer = std::string(peer), pc](boost::system::error_code ec) {
                auto self = weak.lock();
                auto* m = self ? self->find(peer) : nullptr;
                if (m == nullptr || m->offer.pc != pc) return;
                if (ec) {
                    self->log_("webrtc", [&] { return "the answer from " + std::string(peer) + " was refused: " + ec.message(); });
                    self->reset_offer(*m);
                    return;
                }
                self->flush_remote_candidates(m->offer);
            });
        });
    }

    void on_candidate(std::string_view peer, std::string_view offer_id_view, std::string_view candidate) {
        auto weak = weak_from_this();
        sealer_->open(candidate, [weak, peer = std::string(peer), offer_id = std::string(offer_id_view)](boost::system::error_code ec, std::string_view plain) {
            auto self = weak.lock();
            auto* m = self ? self->find(peer) : nullptr;
            if (m == nullptr || ec || m->connected) return;
            side* target = nullptr;
            if (!offer_id.empty() && m->offer.pc && offer_id == m->offer_id) {
                target = &m->offer;
            } else if (m->answer.pc && (offer_id.empty() || offer_id == m->answered_offer_id)) {
                target = &m->answer;
            } else if (offer_id.empty() && m->offer.pc) {
                target = &m->offer;
            }
            if (target == nullptr) return;
            if (!target->pc->has_remote_description()) {
                if (target->pending_remote.size() < max_pending_candidates) target->pending_remote.emplace_back(plain);
                return;
            }
            target->pc->add_ice_candidate(plain, [](boost::system::error_code) {});
        });
    }

    static void flush_remote_candidates(side& s) {
        for (const auto& candidate : std::exchange(s.pending_remote, {})) s.pc->add_ice_candidate(candidate, [](boost::system::error_code) {});
    }

    void on_local_candidate(std::string peer, std::shared_ptr<backend::peer_connection> pc, std::string candidate) {
        auto* n = find(peer);
        if (n == nullptr || n->connected) return;
        if (pc == n->offer.pc) {
            if (n->sealed_offer.empty()) {
                n->offer_candidates.push_back(std::move(candidate));
            } else {
                publish_candidate(peer, pc, candidate);
            }
        } else if (pc == n->answer.pc) {
            if (!n->answer_sent) {
                n->answer_candidates.push_back(std::move(candidate));
            } else {
                publish_candidate(peer, pc, candidate);
            }
        }
    }

    void publish_candidate(std::string_view peer, const std::shared_ptr<backend::peer_connection>& pc, std::string_view candidate) {
        auto weak = weak_from_this();
        sealer_->seal(candidate, [weak, peer = std::string(peer), pc](boost::system::error_code ec, std::string_view sealed) {
            auto self = weak.lock();
            auto* m = self ? self->find(peer) : nullptr;
            if (m == nullptr || ec || m->connected) return;
            if (pc == m->offer.pc) {
                const auto message = protocol::write_signal({.peer_id = self->self_id_, .offer_id = m->offer_id, .candidate = sealed});
                const auto topic = self->peer_topic(peer);
                for (const auto relay : m->offer_relays) self->cb_.publish(relay, topic, message);
            } else if (pc == m->answer.pc) {
                self->send_answer_message(peer, *m, protocol::write_signal({.peer_id = self->self_id_, .offer_id = m->answered_offer_id, .candidate = sealed}));
            }
        });
    }

    void watch_channel(std::string_view peer, const std::shared_ptr<backend::peer_connection>& pc, const std::shared_ptr<backend::data_channel>& channel) {
        auto weak = weak_from_this();
        channel->set_handlers({
            .open = [weak, peer = std::string(peer), pc] { if (auto self = weak.lock()) self->on_channel_open(peer, pc); },
            .message = {},
            .close = [weak, peer = std::string(peer), pc] { if (auto self = weak.lock()) self->on_state(peer, pc, backend::connection_state::closed); },
            .buffered_amount_low = {},
        });
        if (channel->is_open()) on_channel_open(std::string(peer), pc);
    }

    void on_channel_open(std::string peer, std::shared_ptr<backend::peer_connection> pc) {
        auto* n = find(peer);
        if (n == nullptr || n->connected) return;
        side* winner = pc == n->offer.pc ? &n->offer : pc == n->answer.pc ? &n->answer : nullptr;
        if (winner == nullptr || !winner->channel) return;
        n->connected = true;
        auto connected_pc = std::move(winner->pc);
        auto channel = std::move(winner->channel);
        reset_offer(*n);
        reset_answer(*n);
        log_("webrtc", [&] { return "data channel open with " + std::string(peer); });
        cb_.connected(peer, std::move(connected_pc), std::move(channel));
    }

    void on_state(std::string peer, std::shared_ptr<backend::peer_connection> pc, backend::connection_state state) {
        auto* n = find(peer);
        if (n == nullptr || n->connected) return;
        if (state != backend::connection_state::failed && state != backend::connection_state::closed) return;
        if (pc == n->offer.pc) {
            log_("webrtc", [&] { return "the connection we offered to " + std::string(peer) + " failed"; });
            reset_offer(*n);
        } else if (pc == n->answer.pc) {
            log_("webrtc", [&] { return "the connection we answered for " + std::string(peer) + " failed"; });
            reset_answer(*n);
        }
    }

    static void close_side(side& s) {
        if (s.channel) s.channel->set_handlers({});
        if (s.pc) {
            s.pc->set_handlers({});
            s.pc->close();
        }
        s = side{};
    }

    void reset_offer(negotiation& n) {
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

    void reset_answer(negotiation& n) {
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
    std::shared_ptr<backend::sealer> sealer_;
    std::string app_id_;
    std::string room_id_;
    std::string self_id_;
    std::vector<backend::ice_server> ice_servers_;
    callbacks cb_;
    logger log_;
    std::map<std::string, negotiation, std::less<>> peers_;
    bool closed_ = false;
};

}  // namespace webcpp::trystero::detail

#endif
