// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_ACTION_WIRE_HPP
#define WEBCPP_TRYSTERO_DETAIL_ACTION_WIRE_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/protocol/action_wire.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/**
 * A received action message. It owns one buffer, the data channel frame it arrived in or the payload a transfer
 * assembled, and names its parts as ranges of it: moving a message moves the buffer and copies nothing.
 */
struct incoming {
    struct range {
        std::size_t offset = 0;
        std::size_t size = 0;
    };

    std::string peer = {};
    protocol::payload_format format = protocol::payload_format::text;
    backend::bytes buffer = {};
    range name_at = {};
    range payload_at = {};
    range metadata_at = {};  // JSON text, or empty

    std::string_view name() const { return text(name_at); }
    std::span<const std::uint8_t> payload() const { return std::span(buffer).subspan(payload_at.offset, payload_at.size); }
    std::string_view metadata() const { return text(metadata_at); }

    /** The payload as a buffer of its own: the buffer itself when the payload starts it, a copy otherwise. */
    backend::bytes take_payload() && {
        if (payload_at.offset == 0) {
            buffer.resize(payload_at.size);
            return std::move(buffer);
        }
        const auto p = payload();
        return backend::bytes(p.begin(), p.end());
    }

private:
    std::string_view text(range r) const { return std::string_view(reinterpret_cast<const char*>(buffer.data()) + r.offset, r.size); }
};

/** A payload to send: the bytes, and what keeps them alive while a transfer is in flight. */
struct payload_ref {
    std::span<const std::uint8_t> data;
    std::shared_ptr<const void> owner;  // may be empty for a payload that travels inline, which is sent at once
};

/**
 * Sends and receives a room's actions over Trystero's action wire (protocol/action_wire.hpp), after its action wire
 * manager and action receiver. Every frame it sends is a room frame: it writes the room envelope's header and the
 * action frame into one buffer, once per send, shared by every target.
 *
 * | Rule                     | Value                                                                              |
 * | inline                   | when header, metadata and payload fit 16 KiB                                        |
 * | larger payloads          | offer, wait for "accepted" (120 s), then chunks in order, each sent only while the |
 * |                          | channel buffers at most 64 KiB (10 s at most per wait)                              |
 * | receiving                | one transfer per peer at a time, accepted only for an action with a receiver,      |
 * |                          | at most `max_receive_bytes` reserved across peers                                  |
 * | frames before a receiver | kept (64 per peer, 120 s) until the action gets one                                |
 * | protocol violations      | the peer is dropped                                                                 |
 */
class action_wire : public std::enable_shared_from_this<action_wire> {
public:
    /** How the wire reaches one peer's data channel. */
    struct link {
        std::function<bool(std::span<const std::uint8_t>)> send;
        std::function<std::size_t()> buffered_amount;
        std::function<void(std::function<void(bool)>)> wait_drained;
    };

    struct callbacks {
        std::function<std::optional<link>(std::string_view peer, bool include_pending)> find_link;
        /** Views of the peers' ids, valid for the duration of the call. */
        std::function<std::vector<std::string_view>(bool include_pending)> peers;
        std::function<bool(std::string_view peer, std::string_view name)> may_receive;  // the handshake gate
        std::function<bool(std::string_view name)> has_receiver;
        std::function<void(incoming&&)> deliver;
        std::function<void(std::string_view peer, std::string_view reason)> fail_peer;
    };

    action_wire(backend::factory& factory, std::string_view room_token, std::size_t max_receive_bytes, callbacks cb)
        : factory_(factory), token_(room_token), max_receive_bytes_(max_receive_bytes), cb_(std::move(cb)) {}

    /**
     * Sends one payload to `targets`, or to every peer when empty. `done` runs once every target has its copy or has
     * failed, with the first failure. The name, the metadata and the targets are read during the call.
     */
    void send(std::string_view name, protocol::payload_format format, std::string_view metadata, payload_ref payload,
              std::span<const std::string_view> targets, bool include_pending, std::function<void(boost::system::error_code)> done) {
        if (!protocol::valid_action_name(name)) return done(make_error_code(error::invalid_action_name));
        if (metadata.size() > protocol::max_action_frame_bytes - protocol::offer_header_size) return done(make_error_code(error::metadata_too_large));
        std::vector<std::string_view> chosen(targets.begin(), targets.end());
        if (chosen.empty()) chosen = cb_.peers(include_pending);
        if (chosen.empty()) return done({});
        auto state = std::make_shared<fan_out>(chosen.size(), std::move(done));
        if (protocol::fits_inline(metadata.size(), payload.data.size())) {
            auto frame = protocol::begin_room_frame(token_, protocol::inline_header_size + metadata.size() + payload.data.size());
            protocol::append_inline(frame, name, format, metadata, payload.data);
            const auto shared = std::make_shared<const backend::bytes>(std::move(frame));
            for (const auto peer : chosen) {
                auto l = cb_.find_link(peer, include_pending);
                if (!l) {
                    state->complete(make_error_code(error::no_such_peer));
                    continue;
                }
                send_when_drained(*l, shared, [state](boost::system::error_code ec) { state->complete(ec); });
            }
            return;
        }
        if (!payload.owner) {
            // A transfer outlives this call: a payload nobody keeps alive is copied once, here.
            auto copy = std::make_shared<const backend::bytes>(payload.data.begin(), payload.data.end());
            payload = {std::span(*copy), copy};
        }
        for (const auto peer : chosen) {
            auto l = cb_.find_link(peer, include_pending);
            if (!l) {
                state->complete(make_error_code(error::no_such_peer));
                continue;
            }
            send_bulk(peer, std::move(*l), name, format, metadata, payload, state);
        }
    }

    /** One action frame from a peer: `action` is a view into `frame`, which the message keeps if it is delivered. */
    void handle(std::string_view peer, backend::bytes&& frame, std::span<const std::uint8_t> action) {
        const auto decoded = protocol::decode_action(action);
        if (!decoded.frame) {
            cb_.fail_peer(peer, "invalid action frame");
            return;
        }
        if (const auto* p = std::get_if<protocol::proposal>(&*decoded.frame)) {
            if (!cb_.may_receive(peer, p->name)) return;
            if (p->kind == protocol::frame_kind::inline_payload) {
                on_inline(peer, std::move(frame), *p);
            } else {
                on_offer(peer, *p);
            }
        } else if (const auto* c = std::get_if<protocol::chunk>(&*decoded.frame)) {
            on_chunk(peer, *c);
        } else {
            on_control(peer, std::get<protocol::control_message>(*decoded.frame));
        }
    }

    /** An action got a receiver: deliver what waited for it and accept its transfers. */
    void receiver_added() { drain(); }

    void clear_peer(std::string_view peer, boost::system::error_code reason) {
        if (auto it = outgoing_.find(peer); it != outgoing_.end()) {
            auto transfers = std::move(it->second);
            outgoing_.erase(it);
            for (auto& [id, t] : transfers) t->fail(reason);
        }
        if (auto it = incoming_.find(peer); it != incoming_.end()) {
            for (auto& [id, b] : it->second.bulk) {
                if (b.receiving) reserved_bytes_ -= b.size;
            }
            incoming_.erase(it);
        }
        drain();
    }

private:
    static constexpr auto transfer_timeout = std::chrono::milliseconds(120'000);
    static constexpr auto drain_timeout = std::chrono::milliseconds(10'000);
    static constexpr std::size_t max_pending_per_peer = 64;

    /** Completes a send once every target finished, with the first error. */
    struct fan_out {
        fan_out(std::size_t n, std::function<void(boost::system::error_code)> d) : remaining(n), done(std::move(d)) {}
        void complete(boost::system::error_code ec) {
            if (ec && !first_error) first_error = ec;
            if (--remaining == 0) done(first_error);
        }
        std::size_t remaining;
        std::function<void(boost::system::error_code)> done;
        boost::system::error_code first_error;
    };

    struct outgoing_transfer {
        std::function<void()> accepted;
        std::function<void(boost::system::error_code)> fail;
        std::unique_ptr<backend::timer> timer;
    };

    struct bulk_in {
        std::string name;
        protocol::payload_format format = protocol::payload_format::binary;
        std::string metadata;
        std::size_t size = 0;
        backend::bytes data;
        bool receiving = false;
        std::unique_ptr<backend::timer> timer;
    };

    struct waiting_inline {
        incoming message;
        double arrived_ms = 0;
    };

    struct peer_in {
        std::map<std::uint32_t, bulk_in> bulk;
        std::deque<waiting_inline> waiting;
        std::optional<std::uint32_t> active;
        std::size_t count() const { return bulk.size() + waiting.size(); }
    };

    /** Sends a frame once the channel buffers no more than Trystero's threshold. */
    void send_when_drained(const link& l, std::shared_ptr<const backend::bytes> frame, std::function<void(boost::system::error_code)> done) {
        if (l.buffered_amount() <= backend::data_channel::low_threshold) {
            return done(l.send(*frame) ? boost::system::error_code{} : make_error_code(error::peer_disconnected));
        }
        auto timer = std::shared_ptr<backend::timer>(factory_.make_timer());
        auto settled = std::make_shared<bool>(false);
        timer->start(drain_timeout, [settled, done] {
            if (std::exchange(*settled, true)) return;
            done(make_error_code(error::channel_stalled));
        });
        l.wait_drained([settled, timer, send = l.send, frame, done](bool open) {
            if (std::exchange(*settled, true)) return;
            timer->cancel();
            if (!open) return done(make_error_code(error::peer_disconnected));
            done(send(*frame) ? boost::system::error_code{} : make_error_code(error::peer_disconnected));
        });
    }

    std::shared_ptr<const backend::bytes> control_frame(void (*append)(backend::bytes&, std::uint32_t), std::uint32_t id) {
        auto frame = protocol::begin_room_frame(token_, protocol::control_header_size);
        append(frame, id);
        return std::make_shared<const backend::bytes>(std::move(frame));
    }

    void send_bulk(std::string_view peer_view, link l, std::string_view name, protocol::payload_format format, std::string_view metadata, payload_ref payload,
                   std::shared_ptr<fan_out> state) {
        const std::uint32_t id = next_id_++;
        auto offer = protocol::begin_room_frame(token_, protocol::offer_header_size + metadata.size());
        protocol::append_offer(offer, name, format, metadata, id, payload.data.size());
        std::string peer(peer_view);  // the transfer's handlers outlive this call
        auto& transfers = outgoing_[peer];
        auto transfer = std::make_shared<outgoing_transfer>();
        transfers[id] = transfer;
        auto weak = weak_from_this();
        auto finished = std::make_shared<bool>(false);
        auto finish = [weak, peer, id, state, finished](boost::system::error_code ec) {
            if (std::exchange(*finished, true)) return;
            if (auto self = weak.lock()) {
                if (auto it = self->outgoing_.find(peer); it != self->outgoing_.end()) {
                    it->second.erase(id);
                    if (it->second.empty()) self->outgoing_.erase(it);
                }
                // The receiver may hold a reservation for this transfer: tell it, as Trystero does.
                if (ec && ec != make_error_code(error::transfer_rejected) && ec != make_error_code(error::peer_disconnected)) {
                    if (auto l = self->cb_.find_link(peer, true)) l->send(*self->control_frame(protocol::append_cancelled, id));
                }
            }
            state->complete(ec);
        };
        transfer->fail = finish;
        transfer->timer = factory_.make_timer();
        transfer->timer->start(transfer_timeout, [finish] { finish(make_error_code(error::transfer_timed_out)); });
        transfer->accepted = [weak, l, id, payload, finish, transfer_weak = std::weak_ptr(transfer)] {
            auto self = weak.lock();
            auto t = transfer_weak.lock();
            if (!self || !t) return;
            t->timer->cancel();
            self->send_chunks(l, id, payload, 0, finish);
        };
        send_when_drained(l, std::make_shared<const backend::bytes>(std::move(offer)), [finish](boost::system::error_code ec) {
            if (ec) finish(ec);
        });
    }

    void send_chunks(const link& l, std::uint32_t id, const payload_ref& payload, std::size_t offset, std::function<void(boost::system::error_code)> finish) {
        if (offset >= payload.data.size()) return finish({});
        const auto length = std::min(protocol::chunk_size, payload.data.size() - offset);
        auto frame = protocol::begin_room_frame(token_, protocol::chunk_header_size + length);
        protocol::append_chunk(frame, id, offset, payload.data.subspan(offset, length));
        auto weak = weak_from_this();
        send_when_drained(l, std::make_shared<const backend::bytes>(std::move(frame)), [weak, l, id, payload, offset, length, finish](boost::system::error_code ec) {
            if (ec) return finish(ec);
            auto self = weak.lock();
            if (!self) return finish(make_error_code(error::room_left));
            self->send_chunks(l, id, payload, offset + length, finish);
        });
    }

    void control(std::string_view peer, const backend::bytes& frame) {
        if (auto l = cb_.find_link(peer, true)) {
            if (!l->send(frame)) cb_.fail_peer(peer, "unable to send action control");
        }
    }

    void on_control(std::string_view peer, const protocol::control_message& m) {
        if (m.kind == protocol::frame_kind::cancelled) {
            if (auto it = incoming_.find(peer); it != incoming_.end()) {
                if (auto b = it->second.bulk.find(m.id); b != it->second.bulk.end()) release_bulk(it->second, b);
            }
            drain();
            return;
        }
        auto it = outgoing_.find(peer);
        if (it == outgoing_.end()) return;
        auto t = it->second.find(m.id);
        if (t == it->second.end()) return;
        auto transfer = t->second;
        if (m.kind == protocol::frame_kind::accepted) {
            if (transfer->accepted) std::exchange(transfer->accepted, {})();
        } else {
            transfer->fail(make_error_code(error::transfer_rejected));
        }
    }

    void on_inline(std::string_view peer, backend::bytes&& frame, const protocol::proposal& p) {
        const auto base = frame.data();
        auto offset_of = [base](const void* at) { return static_cast<std::size_t>(static_cast<const std::uint8_t*>(at) - base); };
        incoming message{
            .peer = std::string(peer),
            .format = p.format,
            .buffer = std::move(frame),  // the views in `p` stay valid: moving a vector keeps its storage
            .name_at = {offset_of(p.name.data()), p.name.size()},
            .payload_at = {offset_of(p.payload.data()), p.payload.size()},
            .metadata_at = {offset_of(p.metadata.data()), p.metadata.size()},
        };
        if (cb_.has_receiver(p.name)) {
            cb_.deliver(std::move(message));
            return;
        }
        auto& in = incoming_for(peer);
        if (in.count() >= max_pending_per_peer) return;
        in.waiting.push_back({std::move(message), factory_.now_ms()});
    }

    peer_in& incoming_for(std::string_view peer) {
        auto it = incoming_.find(peer);
        if (it == incoming_.end()) it = incoming_.try_emplace(std::string(peer)).first;
        return it->second;
    }

    void on_offer(std::string_view peer, const protocol::proposal& p) {
        auto& in = incoming_for(peer);
        if (in.bulk.contains(p.id)) {
            cb_.fail_peer(peer, "duplicate action offer");
            return;
        }
        auto reject = [&](std::string_view reason) {
            auto frame = protocol::begin_room_frame(token_, protocol::control_header_size + reason.size());
            protocol::append_rejected(frame, p.id, reason);
            control(peer, frame);
        };
        if (p.size > max_receive_bytes_) return reject("payload exceeds receiver size limit");
        if (in.count() >= max_pending_per_peer) return reject("too many pending transfers");
        auto& b = in.bulk[p.id];
        b.name = p.name;
        b.format = p.format;
        b.metadata = p.metadata;
        b.size = p.size;
        b.timer = factory_.make_timer();
        arm(peer, p.id, b);
        drain();
    }

    /** (Re)starts a transfer's expiry: 120 s without an accepted chunk drops it. */
    void arm(std::string_view peer_view, std::uint32_t id, bulk_in& b) {
        auto weak = weak_from_this();
        b.timer->start(transfer_timeout, [weak, peer = std::string(peer_view), id] {
            auto self = weak.lock();
            if (!self) return;
            auto it = self->incoming_.find(peer);
            if (it == self->incoming_.end()) return;
            auto b = it->second.bulk.find(id);
            if (b == it->second.bulk.end()) return;
            self->release_bulk(it->second, b);
            auto frame = protocol::begin_room_frame(self->token_, protocol::control_header_size + 32);
            protocol::append_rejected(frame, id, "action transfer timed out");
            self->control(peer, frame);
            self->drain();
        });
    }

    void on_chunk(std::string_view peer, const protocol::chunk& c) {
        auto it = incoming_.find(peer);
        if (it == incoming_.end()) return;
        auto b = it->second.bulk.find(c.id);
        if (b == it->second.bulk.end()) return;  // cancelled or expired transfers cannot resurrect
        auto& t = b->second;
        if (!t.receiving || c.offset != static_cast<double>(t.data.size()) || c.data.empty() ||
            c.data.size() != std::min(protocol::chunk_size, t.size - t.data.size())) {
            cb_.fail_peer(peer, "invalid action chunk offset or length");
            return;
        }
        // Room for the payload, then its metadata and name, so the delivered message is this one buffer.
        if (t.data.empty()) t.data.reserve(t.size + t.metadata.size() + t.name.size());
        t.data.insert(t.data.end(), c.data.begin(), c.data.end());
        arm(peer, c.id, t);
        if (t.data.size() < t.size) return;
        incoming message{.peer = std::string(peer), .format = t.format};
        message.payload_at = {0, t.size};
        message.metadata_at = {t.size, t.metadata.size()};
        message.name_at = {t.size + t.metadata.size(), t.name.size()};
        t.data.insert(t.data.end(), t.metadata.begin(), t.metadata.end());
        t.data.insert(t.data.end(), t.name.begin(), t.name.end());
        message.buffer = std::move(t.data);
        release_bulk(it->second, b);
        cb_.deliver(std::move(message));
        drain();
    }

    void release_bulk(peer_in& in, std::map<std::uint32_t, bulk_in>::iterator b) {
        if (b->second.receiving) reserved_bytes_ -= b->second.size;
        if (in.active == b->first) in.active.reset();
        in.bulk.erase(b);
    }

    /** Delivers inline frames whose action now has a receiver, and accepts the next transfer of each idle peer. */
    void drain() {
        const double now = factory_.now_ms();
        std::vector<incoming> ready;
        for (auto& [peer, in] : incoming_) {
            std::deque<waiting_inline> still_waiting;
            for (auto& w : in.waiting) {
                if (cb_.has_receiver(w.message.name())) {
                    ready.push_back(std::move(w.message));
                } else if (now - w.arrived_ms < static_cast<double>(transfer_timeout.count())) {
                    still_waiting.push_back(std::move(w));
                }
            }
            in.waiting = std::move(still_waiting);
            if (in.active) continue;
            for (auto& [id, b] : in.bulk) {
                if (b.receiving || !cb_.has_receiver(b.name) || reserved_bytes_ + b.size > max_receive_bytes_) continue;
                b.receiving = true;
                reserved_bytes_ += b.size;
                in.active = id;
                control(peer, *control_frame(protocol::append_accepted, id));
                break;
            }
        }
        for (auto& m : ready) cb_.deliver(std::move(m));
    }

    backend::factory& factory_;
    std::string token_;
    std::size_t max_receive_bytes_;
    callbacks cb_;
    std::uint32_t next_id_ = 0;
    std::size_t reserved_bytes_ = 0;
    std::map<std::string, std::map<std::uint32_t, std::shared_ptr<outgoing_transfer>>, std::less<>> outgoing_;
    std::map<std::string, peer_in, std::less<>> incoming_;
};

}  // namespace webcpp::trystero::detail

#endif
