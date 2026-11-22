// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_IDENTITY_HANDSHAKE_HPP
#define WEBCPP_TRYSTERO_DETAIL_IDENTITY_HANDSHAKE_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/detail/identity_cache.hpp>
#include <webcpp/trystero/identity.hpp>
#include <webcpp/trystero/protocol/ecdsa.hpp>
#include <webcpp/trystero/protocol/handshake.hpp>
#include <webcpp/trystero/protocol/ids.hpp>
#include <webcpp/trystero/protocol/text.hpp>

#include <webcpp/crypto/curves/p256.hpp>

#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::detail {

/**
 * This side of a handshake: its endpoint key and the user it presents.
 *
 * It refers to what the app holds, which outlives every handshake of its rooms.
 */
struct local_identity {
    /** The endpoint's key pair, which signs the transcript. */
    const webcpp::crypto::curves::p256::keypair& endpoint;
    /** The endpoint's peer id. */
    std::string_view peer_id;
    /** The endpoint's compact public key, sent as `pk`. */
    std::string_view endpoint_key;
    /** The user presented, whose id and key are sent as `uid` and `uk` and who signs the
     * transcript. */
    const user_identity& user;
};

/**
 * One side's run of 0.27.0's identity handshake with one remote peer, `createIdentityHandshake`'s
 * `run`, as a synchronous step machine: the room calls @ref start once, then @ref receive with each
 * `@_hsdata` payload of the peer, and sends what each returns.
 *
 * The initiator sends step 1 (its nonce, fingerprints, user id, endpoint and user keys), the
 * responder answers step 2 (its own, and its two signatures of the transcript), and the initiator,
 * once they verify, signs the transcript too and sends step 3. Each step's `pk` must derive the
 * remote peer's id and its `uk` the `uid` claimed, through the app's @ref identity_cache, whose
 * cache also verifies the signatures.
 *
 * Tip: 0.27.0's `onVerified`, which its roles plugin uses to check bans, runs between the
 * initiator's verification and its step 3; this port has no roles plugin (Phase 3), so the
 * initiator reports the remote's identity and its step 3 at once.
 */
class identity_handshake {
public:
    /** The remote side's identity, verified: 0.27.0's `VerifiedPeerIdentity`. */
    struct verified {
        /** Its user id. */
        std::pmr::string user_id;
        /** Its endpoint's compact public key. */
        std::pmr::string endpoint_key;
        /** Its user's compact public key. */
        std::pmr::string user_key;
    };

    /** What a call of the machine gives the room. */
    struct outcome {
        /** An `@_hsdata` payload to send, or empty. */
        std::pmr::string send;
        /** The remote's identity once verified; then the run is over. */
        std::optional<verified> done;
        /** A `protocol::handshake_reason` when the run failed, then over; empty otherwise. */
        std::string_view failure;
    };

    /**
     * A run that has not started.
     *
     * @param self This side's endpoint and user, which outlive the run.
     * @param cache The app's identity cache, which outlives the run.
     * @param app_id The app's id.
     * @param room_id The room's id.
     * @param remote_peer The remote peer's id.
     * @param initiator Whether this side starts, 0.27.0's `selfId < remotePeerId`.
     * @param local_dtls The fingerprints of this side's description.
     * @param remote_dtls The fingerprints of the remote description this side applied.
     * @param nonce_random 16 random bytes, this side's nonce once a version 4 UUID.
     * @param memory The resource of the run's texts and of what it returns.
     */
    identity_handshake(local_identity self, identity_cache& cache, std::string_view app_id,
                       std::string_view room_id, std::string_view remote_peer, bool initiator,
                       std::span<const std::pmr::string> local_dtls,
                       std::span<const std::pmr::string> remote_dtls,
                       std::array<std::uint8_t, 16> nonce_random, std::pmr::memory_resource* memory)
        : self_(self),
          cache_(&cache),
          memory_(memory),
          app_id_(app_id, memory),
          room_id_(room_id, memory),
          remote_peer_(remote_peer, memory),
          local_dtls_(local_dtls.begin(), local_dtls.end(), memory),
          remote_dtls_(remote_dtls.begin(), remote_dtls.end(), memory),
          nonce_(memory),
          transcript_(memory),
          remote_{
              .user_id = std::pmr::string(memory),
              .endpoint_key = std::pmr::string(memory),
              .user_key = std::pmr::string(memory),
          },
          initiator_(initiator) {
        const auto uuid = protocol::uuid_v4(nonce_random);
        nonce_.assign(uuid.data(), uuid.size());
    }

    /**
     * Starts the run: it fails with `dtls_missing` when either description has no fingerprint;
     * else the initiator sends step 1, and the responder waits for it.
     *
     * @return Step 1 to send (initiator), nothing (responder), or the failure.
     */
    outcome start() {
        if (stage_ != stage::idle) {
            return nothing();
        }
        if (local_dtls_.empty() || remote_dtls_.empty()) {
            return fail(protocol::handshake_reason::dtls_missing);
        }
        if (!initiator_) {
            stage_ = stage::awaiting_step_1;
            return nothing();
        }
        stage_ = stage::awaiting_step_2;
        auto out = nothing();
        out.send = write_claims(1, nullptr);
        return out;
    }

    /**
     * Takes the remote's next `@_hsdata` payload: step 1 (responder), step 2 (initiator) or step 3
     * (responder). Before @ref start and once the run is over, it takes nothing.
     *
     * @param json The payload.
     * @return What to send, the remote's identity once verified, or the failure.
     */
    outcome receive(std::string_view json) {
        switch (stage_) {
            case stage::awaiting_step_1: return receive_step_1(json);
            case stage::awaiting_step_2: return receive_step_2(json);
            case stage::awaiting_step_3: return receive_step_3(json);
            case stage::idle:
            case stage::finished: break;
        }
        return nothing();
    }

private:
    enum class stage : std::uint8_t {
        idle,
        awaiting_step_1,
        awaiting_step_2,
        awaiting_step_3,
        finished
    };

    /** The two signatures of the transcript, base64url. */
    struct signatures {
        std::pmr::string endpoint;
        std::pmr::string user;
    };

    /** An outcome with nothing in it, in the run's resource. */
    [[nodiscard]] outcome nothing() const {
        return outcome{.send = std::pmr::string(memory_), .done = std::nullopt, .failure = {}};
    }

    /** Ends the run with `reason`. */
    outcome fail(std::string_view reason) {
        stage_ = stage::finished;
        auto out = nothing();
        out.failure = reason;
        return out;
    }

    /** Ends the run with the remote's identity, and `send` to send. */
    outcome succeed(std::pmr::string send) {
        stage_ = stage::finished;
        auto out = nothing();
        out.send = std::move(send);
        out.done = std::move(remote_);
        return out;
    }

    /** This side's step 1 or 2, 0.27.0's `sendClaims`, with `signed_by` at step 2. */
    [[nodiscard]] std::pmr::string write_claims(int step, const signatures* signed_by) const {
        protocol::handshake_claims claims{
            .step = step,
            .nonce = std::pmr::string(nonce_, memory_),
            .user_id = std::pmr::string(self_.user.user_id(), memory_),
            .endpoint_key = std::pmr::string(self_.endpoint_key, memory_),
            .user_key = std::pmr::string(self_.user.public_key(), memory_),
            .endpoint_signature = std::pmr::string(memory_),
            .user_signature = std::pmr::string(memory_),
            .dtls = std::pmr::vector<std::pmr::string>(local_dtls_, memory_),
        };
        if (signed_by != nullptr) {
            claims.endpoint_signature = signed_by->endpoint;
            claims.user_signature = signed_by->user;
        }
        return protocol::write_handshake_step(claims, memory_);
    }

    /**
     * Reads step 1 or 2 and checks its keys, 0.27.0's `parseAndVerifyPeerIdentityClaims`: the
     * remote's ids and keys are kept; on failure, the reason.
     */
    std::optional<protocol::handshake_claims> read_claims(std::string_view json, int step,
                                                          std::string_view& failure) {
        auto read = protocol::read_handshake_step(json, step, remote_dtls_, local_dtls_, memory_);
        if (!read.claims) {
            failure = read.failure;
            return std::nullopt;
        }
        if (!cache_->verify_peer_id(remote_peer_, read.claims->endpoint_key) ||
            !cache_->verify_user_id(read.claims->user_id, read.claims->user_key)) {
            failure = protocol::handshake_reason::identity;
            return std::nullopt;
        }
        remote_.user_id = read.claims->user_id;
        remote_.endpoint_key = read.claims->endpoint_key;
        remote_.user_key = read.claims->user_key;
        return std::move(read.claims);
    }

    /** The remote fingerprints the transcript holds, 0.27.0's `resolveRemoteDtls`: the
     * description's when known, else those claimed. */
    [[nodiscard]] std::span<const std::pmr::string> remote_dtls_for(
        const protocol::handshake_claims& claims) const noexcept {
        if (!remote_dtls_.empty()) {
            return remote_dtls_;
        }
        return claims.dtls;
    }

    /** The transcript's bytes. */
    [[nodiscard]] std::span<const std::uint8_t> transcript_bytes() const noexcept {
        return {reinterpret_cast<const std::uint8_t*>(transcript_.data()), transcript_.size()};
    }

    /** Signs the transcript by the endpoint and by the user, 0.27.0's `signTranscript`. */
    [[nodiscard]] std::optional<signatures> sign() const {
        const auto endpoint = protocol::sign_p256(self_.endpoint, transcript_bytes());
        const auto user = self_.user.sign(transcript_bytes());
        if (!endpoint || !user) {
            return std::nullopt;
        }
        return signatures{
            .endpoint = protocol::to_base64url(*endpoint, memory_),
            .user = protocol::to_base64url(*user, memory_),
        };
    }

    /** Whether the remote's two signatures verify over the transcript, 0.27.0's
     * `verifyTranscriptDualSignatures`, by the keys of its step 1 or 2. */
    [[nodiscard]] bool verify(std::string_view endpoint_signature,
                              std::string_view user_signature) const {
        // 0.27.0 reads a signature with its loose fromBase64Url; this reading is strict (design
        // D18), so a text it would not write is refused as a signature that does not verify.
        const auto endpoint = protocol::from_base64url(endpoint_signature, memory_);
        const auto user = protocol::from_base64url(user_signature, memory_);
        return endpoint && user &&
               cache_->verify(remote_.endpoint_key, *endpoint, transcript_bytes()) &&
               cache_->verify(remote_.user_key, *user, transcript_bytes());
    }

    /** The responder takes step 1, signs the transcript and answers step 2. */
    outcome receive_step_1(std::string_view json) {
        std::string_view failure;
        const auto claims = read_claims(json, 1, failure);
        if (!claims) {
            return fail(failure);
        }
        transcript_ = protocol::handshake_transcript(
            app_id_, room_id_, remote_peer_, self_.peer_id, claims->nonce, nonce_,
            remote_dtls_for(*claims), local_dtls_, claims->user_id, self_.user.user_id(), memory_);
        const auto signed_by = sign();
        if (!signed_by) {
            return fail(protocol::handshake_reason::identity);
        }
        stage_ = stage::awaiting_step_3;
        auto out = nothing();
        out.send = write_claims(2, &*signed_by);
        return out;
    }

    /** The initiator takes step 2, verifies its signatures, and ends with step 3. */
    outcome receive_step_2(std::string_view json) {
        std::string_view failure;
        const auto claims = read_claims(json, 2, failure);
        if (!claims) {
            return fail(failure);
        }
        transcript_ = protocol::handshake_transcript(
            app_id_, room_id_, self_.peer_id, remote_peer_, nonce_, claims->nonce, local_dtls_,
            remote_dtls_for(*claims), self_.user.user_id(), claims->user_id, memory_);
        if (!verify(claims->endpoint_signature, claims->user_signature)) {
            return fail(protocol::handshake_reason::identity);
        }
        const auto signed_by = sign();
        if (!signed_by) {
            return fail(protocol::handshake_reason::identity);
        }
        protocol::handshake_claims step_3{
            .step = 3,
            .nonce = std::pmr::string(memory_),
            .user_id = std::pmr::string(memory_),
            .endpoint_key = std::pmr::string(memory_),
            .user_key = std::pmr::string(memory_),
            .endpoint_signature = signed_by->endpoint,
            .user_signature = signed_by->user,
            .dtls = std::pmr::vector<std::pmr::string>(memory_),
        };
        return succeed(protocol::write_handshake_step(step_3, memory_));
    }

    /** The responder takes step 3 and verifies its signatures. */
    outcome receive_step_3(std::string_view json) {
        const auto read = protocol::read_handshake_step(json, 3, {}, {}, memory_);
        if (!read.claims) {
            return fail(read.failure);
        }
        if (!verify(read.claims->endpoint_signature, read.claims->user_signature)) {
            return fail(protocol::handshake_reason::identity);
        }
        return succeed(std::pmr::string(memory_));
    }

    local_identity self_;
    identity_cache* cache_;
    std::pmr::memory_resource* memory_;
    std::pmr::string app_id_;
    std::pmr::string room_id_;
    std::pmr::string remote_peer_;
    std::pmr::vector<std::pmr::string> local_dtls_;
    std::pmr::vector<std::pmr::string> remote_dtls_;
    // This side's nonce, a version 4 UUID.
    std::pmr::string nonce_;
    // The transcript both sides sign, once the remote's nonce is known.
    std::pmr::string transcript_;
    // The remote's ids and keys, once its step 1 or 2 is read.
    verified remote_;
    bool initiator_;
    stage stage_ = stage::idle;
};

}  // namespace webcpp::trystero::detail

#endif
