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
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/action_wire.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>

#include <boost/json/storage_ptr.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

// Its implicit move assignment may allocate: a std::pmr member copies when the two resources
// differ, which bugprone-exception-escape, expecting every move assignment not to throw, reports.
/**
 * A received action message. It owns one buffer, the data channel frame it arrived in or the
 * payload a transfer assembled, and names its parts as ranges of it: moving a message moves the
 * buffer and copies nothing. It carries the JSON storage of the room that received it, in which a
 * JSON payload is decoded.
 */
struct incoming {  // NOLINT(bugprone-exception-escape)

    /** Where a part of the message is in its buffer. */
    struct range {
        /** The part's first byte, from the buffer's start. */
        std::size_t offset = 0;
        /** The part's size in bytes. */
        std::size_t size = 0;
    };

    /** The sender's id. */
    std::pmr::string peer = {};
    /** The payload's format, which its frame carried. */
    protocol::payload_format format = protocol::payload_format::text;
    /** The one buffer the message owns: its frame, or the payload its transfer assembled. */
    backend::bytes buffer = {};
    /** Where the action's name is in the buffer. */
    range name_at = {};
    /** Where the payload is in the buffer. */
    range payload_at = {};
    /** Where the metadata, JSON text or empty, is in the buffer. */
    range metadata_at = {};
    /** The receiving room's JSON storage, which a JSON payload is parsed into. */
    boost::json::storage_ptr json = {};

    /** The action's name, a view into the buffer. */
    [[nodiscard]] std::string_view name() const { return text(name_at); }

    /** The payload, a view into the buffer. */
    [[nodiscard]] std::span<const std::uint8_t> payload() const {
        return std::span(buffer).subspan(payload_at.offset, payload_at.size);
    }

    /** The metadata, a view into the buffer. */
    [[nodiscard]] std::string_view metadata() const { return text(metadata_at); }

    /** The payload as a buffer of its own, in the buffer's resource: the buffer itself when the
     * payload starts it. */
    backend::bytes take_payload() && {
        if (payload_at.offset == 0) {
            buffer.resize(payload_at.size);
            return std::move(buffer);
        }
        const auto p = payload();
        return {p.begin(), p.end(), buffer.get_allocator()};
    }

private:
    [[nodiscard]] std::string_view text(range r) const {
        return {reinterpret_cast<const char*>(buffer.data()) + r.offset, r.size};
    }
};

/** A payload to send: the bytes, and what keeps them alive while a transfer is in flight. */
struct payload_ref {
    /** The payload's bytes. */
    std::span<const std::uint8_t> data;
    /** What keeps the bytes alive; may be empty for a payload that travels inline, sent at once. */
    std::shared_ptr<const void> owner;
};

/** What the wire sends through: one peer's data channel, which `detail::peer_link` is. */
class channel_link {
public:
    /** Sends a room frame; false when the channel is no longer open. */
    virtual bool send(std::span<const std::uint8_t> room_frame) = 0;
    /** The bytes the channel buffers, not yet sent; 0 once it ended. */
    [[nodiscard]] virtual std::size_t buffered_amount() const = 0;
    /** Calls `done(true)` once the channel's buffer is below Trystero's threshold, `done(false)` if
     * it closes first. */
    virtual void wait_drained(handler<void(bool)> done) = 0;

protected:
    channel_link() = default;
    channel_link(const channel_link&) = default;
    channel_link& operator=(const channel_link&) = default;
    ~channel_link() = default;
};

/**
 * Sends and receives a room's actions over Trystero's action wire (protocol/action_wire.hpp), after
 * its action wire manager and action receiver. Every frame it sends is a room frame: it writes the
 * room envelope's header and the action frame into one buffer, once per send, shared by every
 * target.
 *
 * | Rule                     | Value                                                              |
 * | inline                   | when header, metadata and payload fit 16 KiB                       |
 * | larger payloads          | offer, wait for "accepted" (120 s), then chunks in order, each     |
 * |                          | sent only while the channel buffers at most 64 KiB (10 s at most   |
 * |                          | per wait)                                                          |
 * | receiving                | one transfer per peer at a time, accepted only for an action with  |
 * |                          | a receiver, at most `max_receive_bytes` reserved across peers      |
 * | frames before a receiver | kept (64 per peer, 120 s) until the action gets one                |
 * | protocol violations      | the peer is dropped                                                |
 */
class action_wire : public std::enable_shared_from_this<action_wire> {
public:
    /** How the wire reaches one peer's data channel, for as long as the channel is there. */
    struct link {
        /** The peer's channel, gone once the peer is. */
        std::weak_ptr<channel_link> target;

        /** Sends a room frame; false when the channel is gone or no longer open. */
        [[nodiscard]] bool send(std::span<const std::uint8_t> frame) const {
            const auto t = target.lock();
            return t && t->send(frame);
        }

        /** The bytes the channel buffers; 0 when it is gone. */
        [[nodiscard]] std::size_t buffered_amount() const {
            const auto t = target.lock();
            return t ? t->buffered_amount() : std::size_t{0};
        }

        /** Calls `done(true)` once the channel drained, `done(false)` if it is gone or closes. */
        void wait_drained(handler<void(bool)> done) const {
            if (const auto t = target.lock()) {
                t->wait_drained(std::move(done));
                return;
            }
            done(false);
        }
    };

    /** What the wire asks of its room. */
    struct callbacks {
        /** The link to a peer, active or, with `include_pending`, still in its handshake. */
        handler<std::optional<link>(std::string_view peer, bool include_pending)> find_link;
        /** Views of the peers' ids, valid for the duration of the call. */
        handler<std::pmr::vector<std::string_view>(bool include_pending)> peers;
        /** Whether a peer's message of an action may be received now: the handshake gate. */
        handler<bool(std::string_view peer, std::string_view name)> may_receive;
        /** Whether an action has a receiver in the room: it was made, or it is internal. */
        handler<bool(std::string_view name)> has_receiver;
        /** Hands a received message to the room. */
        handler<void(incoming&&)> deliver;
        /** Drops a peer that broke the protocol, with the reason. */
        handler<void(std::string_view peer, std::string_view reason)> fail_peer;
    };

    /** Every allocation goes through `memory`; a received message carries `json`, a storage of that
     * resource. */
    action_wire(backend::factory& factory, std::string_view room_token,
                std::size_t max_receive_bytes, callbacks cb, std::pmr::memory_resource* memory,
                boost::json::storage_ptr json)
        : factory_(factory),
          memory_(memory),
          json_(std::move(json)),
          token_(room_token, memory),
          max_receive_bytes_(max_receive_bytes),
          cb_(std::move(cb)),
          outgoing_(memory),
          incoming_(memory) {}

    /**
     * Sends one payload to `targets`, or to every peer when empty. `done` runs once each target's
     * data channel has taken the inline frame, or the last chunk of an accepted transfer, or the
     * send to it has failed, with the first failure. The name, the metadata and the targets are
     * read during the call.
     */
    void send(std::string_view name, protocol::payload_format format, std::string_view metadata,
              payload_ref payload, std::span<const std::string_view> targets, bool include_pending,
              handler<void(boost::system::error_code)> done) {
        if (!protocol::valid_action_name(name)) {
            done(make_error_code(error::invalid_action_name));
            return;
        }
        if (metadata.size() > protocol::max_action_frame_bytes - protocol::offer_header_size) {
            done(make_error_code(error::metadata_too_large));
            return;
        }
        std::pmr::vector<std::string_view> chosen(targets.begin(), targets.end(), memory_);
        if (chosen.empty()) {
            chosen = cb_.peers(include_pending);
        }
        if (chosen.empty()) {
            done({});
            return;
        }
        const auto state =
            std::allocate_shared<fan_out>(allocator(), chosen.size(), std::move(done));
        if (protocol::fits_inline(metadata.size(), payload.data.size())) {
            auto frame = protocol::begin_room_frame(
                token_, protocol::inline_header_size + metadata.size() + payload.data.size(),
                memory_);
            protocol::append_inline(frame, name, format, metadata, payload.data);
            const std::shared_ptr<const backend::bytes> shared =
                std::allocate_shared<backend::bytes>(allocator(), std::move(frame));
            for (const auto peer : chosen) {
                auto l = cb_.find_link(peer, include_pending);
                if (!l) {
                    state->complete(make_error_code(error::no_such_peer));
                    continue;
                }
                send_when_drained(
                    *l, shared,
                    bind<void(boost::system::error_code)>(
                        [state](boost::system::error_code ec) { state->complete(ec); }));
            }
            return;
        }
        if (!payload.owner) {
            // A transfer outlives this call: a payload nobody keeps alive is copied once, here.
            const std::shared_ptr<const backend::bytes> copy = std::allocate_shared<backend::bytes>(
                allocator(), payload.data.begin(), payload.data.end());
            payload = {.data = std::span(*copy), .owner = copy};
        }
        for (const auto peer : chosen) {
            auto l = cb_.find_link(peer, include_pending);
            if (!l) {
                state->complete(make_error_code(error::no_such_peer));
                continue;
            }
            send_bulk(peer, *l, name, format, metadata, payload, state);
        }
    }

    /** One action frame from a peer: `action` is a view into `frame`, which the message keeps if it
     * is delivered. */
    void handle(std::string_view peer, backend::bytes&& frame,
                std::span<const std::uint8_t> action) {
        const auto decoded = protocol::decode_action(action);
        if (!decoded.frame) {
            cb_.fail_peer(peer, "invalid action frame");
            return;
        }
        if (const auto* p = std::get_if<protocol::proposal>(&*decoded.frame)) {
            if (!cb_.may_receive(peer, p->name)) {
                return;
            }
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

    /** Fails a peer's transfers with `reason` and forgets what it sent, once it is gone. */
    void clear_peer(std::string_view peer, boost::system::error_code reason) {
        if (const auto it = outgoing_.find(peer); it != outgoing_.end()) {
            auto transfers = std::move(it->second);
            outgoing_.erase(it);
            for (auto& [id, t] : transfers) {
                t->fail(reason);
            }
        }
        if (const auto it = incoming_.find(peer); it != incoming_.end()) {
            for (auto& [id, b] : it->second.bulk) {
                if (b.receiving) {
                    reserved_bytes_ -= b.size;
                }
            }
            incoming_.erase(it);
        }
        drain();
    }

private:
    using pmr_allocator = std::pmr::polymorphic_allocator<>;
    using done_handler = handler<void(boost::system::error_code)>;

    static constexpr auto transfer_timeout = std::chrono::milliseconds(120'000);
    static constexpr auto drain_timeout = std::chrono::milliseconds(10'000);
    static constexpr std::size_t max_pending_per_peer = 64;

    /** Completes a send once every target finished, with the first error. */
    struct fan_out {
        fan_out(std::size_t n, done_handler d) : remaining(n), done(std::move(d)) {}

        void complete(boost::system::error_code ec) {
            if (ec && !first_error) {
                first_error = ec;
            }
            if (--remaining == 0) {
                done(first_error);
            }
        }

        std::size_t remaining;
        done_handler done;
        boost::system::error_code first_error;
    };

    /** A completion the drain timer and the drain wait share: the first to settle it calls it. */
    struct settling {
        explicit settling(done_handler d) : done(std::move(d)) {}

        bool settled = false;
        done_handler done;
    };

    struct outgoing_transfer {
        handler<void()> accepted;
        done_handler fail;
        std::shared_ptr<backend::timer> timer;
    };

    struct bulk_in {
        explicit bulk_in(pmr_allocator a) : name(a), metadata(a), data(a) {}

        std::pmr::string name;
        protocol::payload_format format = protocol::payload_format::binary;
        std::pmr::string metadata;
        std::size_t size = 0;
        backend::bytes data;
        bool receiving = false;
        std::shared_ptr<backend::timer> timer;
    };

    // Its implicit move assignment may allocate: a std::pmr member copies when the two resources
    // differ, which bugprone-exception-escape, expecting every move assignment not to throw,
    // reports.
    struct waiting_inline {  // NOLINT(bugprone-exception-escape)
        incoming message;
        double arrived_ms = 0;
    };

    struct peer_in {
        explicit peer_in(pmr_allocator a) : bulk(a), waiting(a) {}

        std::pmr::map<std::uint32_t, bulk_in> bulk;
        std::pmr::deque<waiting_inline> waiting;
        std::optional<std::uint32_t> active;

        [[nodiscard]] std::size_t count() const { return bulk.size() + waiting.size(); }
    };

    pmr_allocator allocator() const { return {memory_}; }

    /** `f` as a handler of the room's resource. */
    template <class Signature, class F>
    handler<Signature> bind(F&& f) const {
        return handler<Signature>(std::allocator_arg, allocator(), std::forward<F>(f));
    }

    /** Sends a frame once the channel buffers no more than Trystero's threshold. */
    void send_when_drained(const link& l, const std::shared_ptr<const backend::bytes>& frame,
                           done_handler done) {
        if (l.buffered_amount() <= backend::data_channel::low_threshold) {
            done(l.send(*frame) ? boost::system::error_code{}
                                : make_error_code(error::peer_disconnected));
            return;
        }
        const auto timer = factory_.make_timer(memory_);
        const auto pending = std::allocate_shared<settling>(allocator(), std::move(done));
        timer->start(drain_timeout, bind<void()>([pending] {
                         if (std::exchange(pending->settled, true)) {
                             return;
                         }
                         pending->done(make_error_code(error::channel_stalled));
                     }));
        l.wait_drained(bind<void(bool)>([pending, timer, l, frame](bool open) {
            if (std::exchange(pending->settled, true)) {
                return;
            }
            timer->cancel();
            if (!open) {
                pending->done(make_error_code(error::peer_disconnected));
                return;
            }
            pending->done(l.send(*frame) ? boost::system::error_code{}
                                         : make_error_code(error::peer_disconnected));
        }));
    }

    std::shared_ptr<const backend::bytes> control_frame(void (*append)(backend::bytes&,
                                                                       std::uint32_t),
                                                        std::uint32_t id) {
        auto frame = protocol::begin_room_frame(token_, protocol::control_header_size, memory_);
        append(frame, id);
        return std::allocate_shared<backend::bytes>(allocator(), std::move(frame));
    }

    void send_bulk(std::string_view peer_view, const link& l, std::string_view name,
                   protocol::payload_format format, std::string_view metadata,
                   const payload_ref& payload, const std::shared_ptr<fan_out>& state) {
        const std::uint32_t id = next_id_++;
        auto offer = protocol::begin_room_frame(
            token_, protocol::offer_header_size + metadata.size(), memory_);
        protocol::append_offer(offer, name, format, metadata, id, payload.data.size());
        std::pmr::string peer(peer_view, memory_);  // the transfer's handlers outlive this call
        auto& transfers = outgoing_[peer];
        const auto transfer = std::allocate_shared<outgoing_transfer>(allocator());
        transfers[id] = transfer;
        const auto weak = weak_from_this();
        const auto finished = std::allocate_shared<bool>(allocator(), false);
        // Shared by the timer, the acceptance, the chunks and the transfer's failure: the first
        // call settles it.
        const auto finish = std::allocate_shared<done_handler>(
            allocator(),
            [weak, peer = std::move(peer), id, state, finished](boost::system::error_code ec) {
                if (std::exchange(*finished, true)) {
                    return;
                }
                if (const auto self = weak.lock()) {
                    if (const auto it = self->outgoing_.find(peer); it != self->outgoing_.end()) {
                        it->second.erase(id);
                        if (it->second.empty()) {
                            self->outgoing_.erase(it);
                        }
                    }
                    // The receiver may hold a reservation for this transfer: tell it, as Trystero
                    // does. A channel that refuses the frame is closing, which frees it anyway.
                    if (ec && ec != make_error_code(error::transfer_rejected) &&
                        ec != make_error_code(error::peer_disconnected)) {
                        if (auto l = self->cb_.find_link(peer, true)) {
                            static_cast<void>(
                                l->send(*self->control_frame(protocol::append_cancelled, id)));
                        }
                    }
                }
                state->complete(ec);
            });
        transfer->fail = bind<void(boost::system::error_code)>(
            [finish](boost::system::error_code ec) { (*finish)(ec); });
        transfer->timer = factory_.make_timer(memory_);
        transfer->timer->start(transfer_timeout, bind<void()>([finish] {
                                   (*finish)(make_error_code(error::transfer_timed_out));
                               }));
        transfer->accepted =
            bind<void()>([weak, l, id, payload, finish, transfer_weak = std::weak_ptr(transfer)] {
                const auto self = weak.lock();
                const auto t = transfer_weak.lock();
                if (!self || !t) {
                    return;
                }
                t->timer->cancel();
                self->send_chunks(l, id, payload, 0, finish);
            });
        send_when_drained(
            l, std::allocate_shared<backend::bytes>(allocator(), std::move(offer)),
            bind<void(boost::system::error_code)>([finish](boost::system::error_code ec) {
                if (ec) {
                    (*finish)(ec);
                }
            }));
    }

    void send_chunks(const link& l, std::uint32_t id, const payload_ref& payload,
                     std::size_t offset, const std::shared_ptr<done_handler>& finish) {
        if (offset >= payload.data.size()) {
            (*finish)({});
            return;
        }
        const auto length = std::min(protocol::chunk_size, payload.data.size() - offset);
        auto frame =
            protocol::begin_room_frame(token_, protocol::chunk_header_size + length, memory_);
        protocol::append_chunk(frame, id, offset, payload.data.subspan(offset, length));
        const auto weak = weak_from_this();
        send_when_drained(
            l, std::allocate_shared<backend::bytes>(allocator(), std::move(frame)),
            bind<void(boost::system::error_code)>(
                [weak, l, id, payload, offset, length, finish](boost::system::error_code ec) {
                    if (ec) {
                        (*finish)(ec);
                        return;
                    }
                    const auto self = weak.lock();
                    if (!self) {
                        (*finish)(make_error_code(error::room_left));
                        return;
                    }
                    self->send_chunks(l, id, payload, offset + length, finish);
                }));
    }

    void control(std::string_view peer, const backend::bytes& frame) {
        if (auto l = cb_.find_link(peer, true)) {
            if (!l->send(frame)) {
                cb_.fail_peer(peer, "unable to send action control");
            }
        }
    }

    void on_control(std::string_view peer, const protocol::control_message& m) {
        if (m.kind == protocol::frame_kind::cancelled) {
            if (const auto it = incoming_.find(peer); it != incoming_.end()) {
                if (const auto b = it->second.bulk.find(m.id); b != it->second.bulk.end()) {
                    release_bulk(it->second, b);
                }
            }
            drain();
            return;
        }
        const auto it = outgoing_.find(peer);
        if (it == outgoing_.end()) {
            return;
        }
        const auto t = it->second.find(m.id);
        if (t == it->second.end()) {
            return;
        }
        const auto transfer = t->second;
        if (m.kind == protocol::frame_kind::accepted) {
            if (transfer->accepted) {
                std::exchange(transfer->accepted, {})();
            }
        } else {
            transfer->fail(make_error_code(error::transfer_rejected));
        }
    }

    void on_inline(std::string_view peer, backend::bytes&& frame, const protocol::proposal& p) {
        const auto* const base = frame.data();
        const auto offset_of = [base](const void* at) {
            return static_cast<std::size_t>(static_cast<const std::uint8_t*>(at) - base);
        };
        incoming message{
            .peer = std::pmr::string(peer, memory_),
            .format = p.format,
            .buffer =
                std::move(frame),  // the views in `p` stay valid: moving a vector keeps its storage
            .name_at = {.offset = offset_of(p.name.data()), .size = p.name.size()},
            .payload_at = {.offset = offset_of(p.payload.data()), .size = p.payload.size()},
            .metadata_at = {.offset = offset_of(p.metadata.data()), .size = p.metadata.size()},
            .json = json_,
        };
        if (cb_.has_receiver(p.name)) {
            cb_.deliver(std::move(message));
            return;
        }
        auto& in = incoming_for(peer);
        if (in.count() >= max_pending_per_peer) {
            return;
        }
        in.waiting.push_back({.message = std::move(message), .arrived_ms = factory_.now_ms()});
    }

    peer_in& incoming_for(std::string_view peer) {
        auto it = incoming_.find(peer);
        if (it == incoming_.end()) {
            it = incoming_.try_emplace(std::pmr::string(peer, memory_), allocator()).first;
        }
        return it->second;
    }

    void on_offer(std::string_view peer, const protocol::proposal& p) {
        auto& in = incoming_for(peer);
        if (in.bulk.contains(p.id)) {
            cb_.fail_peer(peer, "duplicate action offer");
            return;
        }
        const auto reject = [&](std::string_view reason) {
            auto frame = protocol::begin_room_frame(
                token_, protocol::control_header_size + reason.size(), memory_);
            protocol::append_rejected(frame, p.id, reason);
            control(peer, frame);
        };
        if (p.size > max_receive_bytes_) {
            reject("payload exceeds receiver size limit");
            return;
        }
        if (in.count() >= max_pending_per_peer) {
            reject("too many pending transfers");
            return;
        }
        auto& b = in.bulk.try_emplace(p.id, allocator()).first->second;
        b.name = p.name;
        b.format = p.format;
        b.metadata = p.metadata;
        b.size = p.size;
        b.timer = factory_.make_timer(memory_);
        arm(peer, p.id, b);
        drain();
    }

    /** (Re)starts a transfer's expiry: 120 s without an accepted chunk drops it. */
    void arm(std::string_view peer_view, std::uint32_t id, bulk_in& b) {
        const auto weak = weak_from_this();
        b.timer->start(transfer_timeout,
                       bind<void()>([weak, peer = std::pmr::string(peer_view, memory_), id] {
                           const auto self = weak.lock();
                           if (!self) {
                               return;
                           }
                           const auto it = self->incoming_.find(peer);
                           if (it == self->incoming_.end()) {
                               return;
                           }
                           const auto b = it->second.bulk.find(id);
                           if (b == it->second.bulk.end()) {
                               return;
                           }
                           self->release_bulk(it->second, b);
                           auto frame = protocol::begin_room_frame(
                               self->token_, protocol::control_header_size + 32, self->memory_);
                           protocol::append_rejected(frame, id, "action transfer timed out");
                           self->control(peer, frame);
                           self->drain();
                       }));
    }

    void on_chunk(std::string_view peer, const protocol::chunk& c) {
        const auto it = incoming_.find(peer);
        if (it == incoming_.end()) {
            return;
        }
        const auto b = it->second.bulk.find(c.id);
        if (b == it->second.bulk.end()) {
            return;  // cancelled or expired transfers cannot resurrect
        }
        auto& t = b->second;
        if (!t.receiving || c.offset != static_cast<double>(t.data.size()) || c.data.empty() ||
            c.data.size() != std::min(protocol::chunk_size, t.size - t.data.size())) {
            cb_.fail_peer(peer, "invalid action chunk offset or length");
            return;
        }
        // Room for the payload, then its metadata and name, so the delivered message is this one
        // buffer.
        if (t.data.empty()) {
            t.data.reserve(t.size + t.metadata.size() + t.name.size());
        }
        t.data.insert(t.data.end(), c.data.begin(), c.data.end());
        arm(peer, c.id, t);
        if (t.data.size() < t.size) {
            return;
        }
        const incoming::range payload_at{.offset = 0, .size = t.size};
        const incoming::range metadata_at{.offset = t.size, .size = t.metadata.size()};
        const incoming::range name_at{.offset = t.size + t.metadata.size(), .size = t.name.size()};
        t.data.insert(t.data.end(), t.metadata.begin(), t.metadata.end());
        t.data.insert(t.data.end(), t.name.begin(), t.name.end());
        incoming message{
            .peer = std::pmr::string(peer, memory_),
            .format = t.format,
            .buffer = std::move(t.data),  // moved, in its resource
            .name_at = name_at,
            .payload_at = payload_at,
            .metadata_at = metadata_at,
            .json = json_,
        };
        release_bulk(it->second, b);
        cb_.deliver(std::move(message));
        drain();
    }

    void release_bulk(peer_in& in, std::pmr::map<std::uint32_t, bulk_in>::iterator b) {
        if (b->second.receiving) {
            reserved_bytes_ -= b->second.size;
        }
        if (in.active == b->first) {
            in.active.reset();
        }
        in.bulk.erase(b);
    }

    /** Delivers inline frames whose action now has a receiver, and accepts the next transfer of
     * each idle peer. */
    void drain() {
        const double now = factory_.now_ms();
        std::pmr::vector<incoming> ready(memory_);
        for (auto& [peer, in] : incoming_) {
            std::pmr::deque<waiting_inline> still_waiting(memory_);
            for (auto& w : in.waiting) {
                if (cb_.has_receiver(w.message.name())) {
                    ready.push_back(std::move(w.message));
                } else if (now - w.arrived_ms < static_cast<double>(transfer_timeout.count())) {
                    still_waiting.push_back(std::move(w));
                }
            }
            in.waiting = std::move(still_waiting);
            if (in.active) {
                continue;
            }
            for (auto& [id, b] : in.bulk) {
                if (b.receiving || !cb_.has_receiver(b.name) ||
                    reserved_bytes_ + b.size > max_receive_bytes_) {
                    continue;
                }
                b.receiving = true;
                reserved_bytes_ += b.size;
                in.active = id;
                control(peer, *control_frame(protocol::append_accepted, id));
                break;
            }
        }
        for (auto& m : ready) {
            cb_.deliver(std::move(m));
        }
    }

    backend::factory& factory_;
    std::pmr::memory_resource* memory_;
    boost::json::storage_ptr json_;
    std::pmr::string token_;
    std::size_t max_receive_bytes_;
    callbacks cb_;
    std::uint32_t next_id_ = 0;
    std::size_t reserved_bytes_ = 0;
    std::pmr::map<std::pmr::string,
                  std::pmr::map<std::uint32_t, std::shared_ptr<outgoing_transfer>>, std::less<>>
        outgoing_;
    std::pmr::map<std::pmr::string, peer_in, std::less<>> incoming_;
};

}  // namespace webcpp::trystero::detail

#endif
