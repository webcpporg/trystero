// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_HANDSHAKE_MANAGER_HPP
#define WEBCPP_TRYSTERO_DETAIL_HANDSHAKE_MANAGER_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/handshake_reason.hpp>
#include <webcpp/trystero/protocol/text.hpp>

#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace webcpp::trystero::detail {

/**
 * What a @ref handshake_manager asks of its room, 0.27.0's `HandshakeManagerDeps` without the state
 * plugin's `beforeSendHandshakeReady` (Phase 3). Every one is set.
 *
 * Each is called on the room's executor, and may call the manager back: the manager looks its peer
 * up again after each call.
 */
struct handshake_callbacks {
    /** Runs the handshake's steps with a peer, 0.27.0's `onPeerHandshake`: the room's identity
     * handshake, through @ref handshake_manager::next_message; `done` takes "" when they passed,
     * else the reason they failed. */
    handler<void(std::string_view peer, bool initiator, handler<void(std::string_view)> done)>
        run_steps;
    /** Sends the ready frame, `@_hsready`, whose payload is `ready_json`: "" for 0.27.0's empty
     * text, else a JSON text. */
    handler<void(std::string_view peer, std::string_view ready_json)> send_ready;
    /** The steps passed on this side, 0.27.0's `onLocalHandshakePassed`: false when the room
     * refused the peer, a full room a new user (a ban needs roles, Phase 3). */
    handler<bool(std::string_view peer)> local_passed;
    /** Admits the peer, 0.27.0's `acquireAdmissionSlot`: false when the room no longer wants it. */
    handler<bool(std::string_view peer)> admit;
    /** Both sides are ready: the peer joins, 0.27.0's `onActivate`. */
    handler<void(std::string_view peer)> activate;
    /** The handshake failed, with the reason, 0.27.0's `onHandshakeError` and `onFailure`; the room
     * lets the peer go, which clears it. */
    handler<void(std::string_view peer, std::string_view reason)> failed;
};

/**
 * The handshake of each peer of a room, 0.27.0's `createHandshakeManager` (`handshake.ts`): its
 * phases, its 10 s, its 16 buffered messages and its ready frames.
 *
 * | Phase                   | Left by                                                        |
 * | `added`                 | `start`, which runs the steps and starts the timer              |
 * | `running_steps`         | the steps' end: to `awaiting_remote_ready` for a deferred       |
 * |                         | responder whose peer is not ready, else through                 |
 * |                         | `preparing_ready`, which sends the ready frame, to              |
 * |                         | `awaiting_peer_ready`, or to `active` when the peer is ready    |
 * | `awaiting_remote_ready` | the peer's ready frame, then as above                           |
 * | `awaiting_peer_ready`   | the peer's ready frame: `active`                                |
 * | `active`                | `clear` alone                                                   |
 *
 * The initiator is the side whose id is lower in UTF-16 code unit order, as 0.27.0's
 * `effectiveSelfId < id` compares its strings. The ready frame is sent before `activate` is called,
 * so that a send inside the room's `on_peer_join` follows it on the wire. A failure is reported
 * through `callbacks::failed` while the handshake is pending, once per timer or message that causes
 * it: the timer's 10 s ("handshake timed out after 10000ms"), a 17th message buffered before the
 * steps read it ("too many pending handshake messages"), or the steps' own reason. The room then
 * clears the peer.
 *
 * Every allocation goes through the room's resource. It is owned by a `std::shared_ptr`, made with
 * `std::allocate_shared`: its timers and the steps' `done` name it weakly.
 *
 * Tip: each `add` gives the peer a new generation, which every timer and `done` carries, so that
 * one of a cleared or replaced handshake reaches nothing, as 0.27.0 compares the peer's handle.
 */
class handshake_manager final : public std::enable_shared_from_this<handshake_manager> {
public:
    /** How long a handshake has once started, 0.27.0's `defaultHandshakeTimeoutMs`. */
    static constexpr std::chrono::milliseconds timeout{10'000};
    /** The most messages buffered before the steps read them, 0.27.0's
     * `maxBufferedHandshakeMessages`. */
    static constexpr std::size_t max_buffered_messages = 16;
    /** The reason the steps get with a message they wait for when the peer is cleared, 0.27.0's
     * `handshakeDisconnectErr`, whose code is `error::peer_disconnected`. */
    static constexpr std::string_view disconnected_reason =
        protocol::handshake_reason::disconnected;
    /** The reason of a handshake whose timer expired, 0.27.0's message without its prefix. */
    static constexpr std::string_view timed_out_reason = protocol::handshake_reason::timed_out;
    /** The reason of a 17th buffered message, 0.27.0's message without its prefix. */
    static constexpr std::string_view too_many_messages_reason =
        protocol::handshake_reason::too_many_messages;

    /** Where a peer's handshake is, 0.27.0's `HandshakePeerEntry` phases. */
    enum class phase {
        /** Added; its messages and its ready frame are kept until `start`. */
        added,
        /** The steps run. */
        running_steps,
        /** A deferred responder's steps passed; it waits for the peer's ready frame. */
        awaiting_remote_ready,
        /** The ready frame is being prepared: reserved for Phase 3's state plugin, whose
         * preparation is asynchronous. Until then the preparation ends in the call that starts it,
         * and no peer is ever in this phase, so the cases that read it (`receive_ready`'s
         * among them) are untested until Phase 3 enters it. */
        preparing_ready,
        /** The ready frame was sent; it waits for the peer's. */
        awaiting_peer_ready,
        /** Both sides are ready: the peer is in the room. */
        active,
    };

    /**
     * A manager of the handshakes of the room of `self_id`.
     *
     * @param self_id This peer's id, copied: the lower id initiates.
     * @param defer_responder_ready Whether the responder waits for the initiator's ready frame
     * before it admits and sends its own, 0.27.0's `deferResponderReady`: a room with
     * `max_users`.
     * @param cb What the manager asks of its room.
     * @param factory The backend, for the timers; it outlives the manager.
     * @param memory The room's resource, which outlives the manager.
     */
    // NOLINTNEXTLINE(performance-unnecessary-value-param): a sink, moved on
    handshake_manager(std::string_view self_id, bool defer_responder_ready, handshake_callbacks cb,
                      backend::factory& factory, std::pmr::memory_resource* memory)
        : factory_(factory),
          memory_(memory),
          self_id_(self_id, memory),
          defer_responder_ready_(defer_responder_ready),
          cb_(std::move(cb)),
          peers_(memory) {}

    /** A manager has one identity and cannot be copied. */
    handshake_manager(const handshake_manager&) = delete;
    /** A manager cannot be replaced by copying. */
    handshake_manager& operator=(const handshake_manager&) = delete;

    /** Calls nothing: pending steps and timers reach nothing once it is gone. */
    ~handshake_manager() = default;

    /**
     * Adds a peer in the phase `added`, 0.27.0's `addPeer`; a handshake the peer had is cleared
     * first, as `clear` clears it.
     *
     * @param peer The peer's id, copied.
     */
    void add(std::string_view peer) {
        clear(peer);
        auto& e = peers_.try_emplace(std::pmr::string(peer, memory_), allocator()).first->second;
        e.generation = ++generations_;
    }

    /**
     * Starts an added peer's handshake, 0.27.0's `start`: the timer, then the steps, which may pass
     * during the call. A peer not in the phase `added` is left as it is.
     *
     * @param peer_view The peer's id.
     */
    void start(std::string_view peer_view) {
        auto* e = find(peer_view);
        if (e == nullptr || e->at != phase::added) {
            return;
        }
        const std::pmr::string peer(peer_view, memory_);
        const auto generation = e->generation;
        const auto weak = weak_from_this();
        e->timer = factory_.make_timer(memory_);
        e->timer->start(timeout,
                        bind<void()>([weak, peer = std::pmr::string(peer, memory_), generation] {
                            if (const auto self = weak.lock()) {
                                self->fail(peer, generation, timed_out_reason);
                            }
                        }));
        e->at = phase::running_steps;
        e->initiator = protocol::utf16_less(self_id_, peer);
        cb_.run_steps(peer, e->initiator,
                      bind<void(std::string_view)>([weak, peer = std::pmr::string(peer, memory_),
                                                    generation](std::string_view reason) {
                          if (const auto self = weak.lock()) {
                              self->steps_done(peer, generation, reason);
                          }
                      }));
    }

    /**
     * Forgets a peer's handshake, 0.27.0's `clearPeer`: its timer stops, its buffered messages go,
     * and a step waiting for a message completes with `error::peer_disconnected`
     * (@ref disconnected_reason). Nothing is reported.
     *
     * @param peer The peer's id.
     */
    void clear(std::string_view peer) {
        const auto it = peers_.find(peer);
        if (it == peers_.end()) {
            return;
        }
        // A node handle's mapped value is mutable through a const handle.
        const auto node = peers_.extract(it);
        auto& e = node.mapped();
        if (e.timer) {
            e.timer->cancel();
        }
        auto waiters = std::move(e.waiters);
        for (auto& next : waiters) {
            next(make_error_code(error::peer_disconnected), {});
        }
    }

    /**
     * A message of the peer's steps, the payload of an `@_hsdata` frame, 0.27.0's `receiveData`: to
     * the step waiting for one, else buffered, a 17th buffered one failing the handshake. Dropped
     * once the steps passed.
     *
     * @param peer The peer's id.
     * @param json The payload, copied when buffered.
     */
    void receive_data(std::string_view peer, std::string_view json) {
        auto* e = find(peer);
        if (e == nullptr || (e->at != phase::added && e->at != phase::running_steps)) {
            return;
        }
        if (!e->waiters.empty()) {
            auto next = std::move(e->waiters.front());
            e->waiters.pop_front();
            next(boost::system::error_code{}, json);
            return;
        }
        if (e->messages.size() >= max_buffered_messages) {
            fail(peer, e->generation, too_many_messages_reason);
            return;
        }
        e->messages.emplace_back(json);
    }

    /**
     * Hands the steps the peer's next message, 0.27.0's `receive`: a buffered one during the call,
     * else the next to arrive. `next` completes with `error::peer_disconnected` when the peer's
     * steps are not running, or once it is cleared.
     *
     * @param peer The peer's id.
     * @param next Called once, with the message, valid for the duration of the call.
     */
    void next_message(std::string_view peer,
                      handler<void(boost::system::error_code, std::string_view json)> next) {
        auto* e = find(peer);
        if (e == nullptr || e->at != phase::running_steps) {
            next(make_error_code(error::peer_disconnected), {});
            return;
        }
        if (!e->messages.empty()) {
            const std::pmr::string json = std::move(e->messages.front());
            e->messages.pop_front();
            next(boost::system::error_code{}, json);
            return;
        }
        e->waiters.push_back(std::move(next));
    }

    /**
     * The peer's ready frame, 0.27.0's `receiveReady`: kept until this side's steps passed, or the
     * peer joins now.
     *
     * @param peer The peer's id.
     */
    void receive_ready(std::string_view peer) {
        auto* e = find(peer);
        if (e == nullptr) {
            return;
        }
        switch (e->at) {
            case phase::added:
            case phase::running_steps:
            case phase::preparing_ready: e->remote_ready = true; return;
            case phase::awaiting_remote_ready:
                begin_preparing_ready(std::pmr::string(peer, memory_), e->generation, true);
                return;
            case phase::awaiting_peer_ready: {
                const std::pmr::string id(peer, memory_);
                e->timer->cancel();
                e->at = phase::active;
                cb_.activate(id);
                return;
            }
            case phase::active: return;
        }
    }

    /**
     * Whether a message of the peer may be received now, 0.27.0's `canReceiveFromPeer`: the peer's
     * handshake is known, and it is active or the action is received while pending.
     *
     * @param peer The peer's id.
     * @param receive_while_pending Whether the action is received during the handshake.
     * @return Whether it may.
     */
    [[nodiscard]] bool can_receive(std::string_view peer, bool receive_while_pending) const {
        const auto it = peers_.find(peer);
        return it != peers_.end() && (it->second.at == phase::active || receive_while_pending);
    }

    /**
     * Where a peer's handshake is.
     *
     * @param peer The peer's id.
     * @return Its phase; empty for a peer not added, or cleared.
     */
    [[nodiscard]] std::optional<phase> phase_of(std::string_view peer) const {
        const auto it = peers_.find(peer);
        return it == peers_.end() ? std::nullopt : std::optional<phase>(it->second.at);
    }

private:
    struct entry {
        explicit entry(std::pmr::polymorphic_allocator<> a) : messages(a), waiters(a) {}

        phase at = phase::added;
        std::uint64_t generation = 0;
        bool remote_ready = false;
        bool initiator = false;
        std::shared_ptr<backend::timer> timer;
        std::pmr::deque<std::pmr::string> messages;
        std::pmr::deque<handler<void(boost::system::error_code, std::string_view)>> waiters;
    };

    std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    template <class Signature, class F>
    handler<Signature> bind(F&& f) const {
        return handler<Signature>(std::allocator_arg, allocator(), std::forward<F>(f));
    }

    entry* find(std::string_view peer) {
        const auto it = peers_.find(peer);
        return it == peers_.end() ? nullptr : &it->second;
    }

    /** The peer's entry, if it is still the handshake of `generation`. */
    entry* find(std::string_view peer, std::uint64_t generation) {
        auto* e = find(peer);
        return e != nullptr && e->generation == generation ? e : nullptr;
    }

    /** 0.27.0's `failPeerHandshake`: the timer stops and the room hears of it; the entry stays
     * until the room clears it. */
    void fail(std::string_view peer_view, std::uint64_t generation, std::string_view reason_view) {
        const auto* e = find(peer_view, generation);
        if (e == nullptr) {
            return;
        }
        if (e->timer) {
            e->timer->cancel();
        }
        // Copies: the room's call may free the views' storage.
        const std::pmr::string peer(peer_view, memory_);
        const std::pmr::string reason(reason_view, memory_);
        cb_.failed(peer, reason);
    }

    /** The steps ended: a reason fails the handshake; else, the steps having run in the phase
     * `running_steps`, the room hears they passed, and the ready frame follows. */
    void steps_done(std::string_view peer, std::uint64_t generation, std::string_view reason) {
        if (!reason.empty()) {
            fail(peer, generation, reason);
            return;
        }
        auto* e = find(peer, generation);
        if (e == nullptr || e->at != phase::running_steps) {
            return;
        }
        e->messages.clear();
        auto waiters = std::move(e->waiters);
        e->waiters.clear();
        for (auto& next : waiters) {
            next(make_error_code(error::peer_disconnected), {});
        }
        if (!cb_.local_passed(peer)) {
            return;
        }
        e = find(peer, generation);
        if (e == nullptr || e->at != phase::running_steps) {
            return;
        }
        if (defer_responder_ready_ && !e->initiator && !e->remote_ready) {
            e->at = phase::awaiting_remote_ready;
            return;
        }
        begin_preparing_ready(peer, generation, e->remote_ready);
    }

    /** 0.27.0's `beginPreparingReady` and its `finishReady`, which nothing prepares before Phase
     * 3's state plugin: the ready frame is "", sent before the peer joins. */
    void begin_preparing_ready(std::string_view peer, std::uint64_t generation, bool remote_ready) {
        if (!cb_.admit(peer)) {
            return;
        }
        auto* e = find(peer, generation);
        if (e == nullptr || e->at == phase::active) {
            return;
        }
        const bool ready = remote_ready || e->at == phase::awaiting_remote_ready || e->remote_ready;
        if (ready) {
            e->timer->cancel();
            e->at = phase::active;
        } else {
            e->at = phase::awaiting_peer_ready;
        }
        cb_.send_ready(peer, "");
        // The send can have ended the peer; 0.27.0 reports a failed send later, from its promise.
        if (ready && find(peer, generation) != nullptr) {
            cb_.activate(peer);
        }
    }

    backend::factory& factory_;
    std::pmr::memory_resource* memory_;
    std::pmr::string self_id_;
    bool defer_responder_ready_;
    handshake_callbacks cb_;
    std::pmr::map<std::pmr::string, entry, std::less<>> peers_;
    std::uint64_t generations_ = 0;
};

}  // namespace webcpp::trystero::detail

#endif
