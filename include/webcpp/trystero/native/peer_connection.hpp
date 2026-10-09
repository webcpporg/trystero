// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_NATIVE_PEER_CONNECTION_HPP
#define WEBCPP_TRYSTERO_NATIVE_PEER_CONNECTION_HPP

#include <webcpp/trystero/config.hpp>

// libdatachannel reports errors by throwing, and the peer connection over it catches them: it exists only
// with exceptions, and without them (WEBCPP_TRYSTERO_NO_EXCEPTIONS) this header declares nothing.
#ifndef WEBCPP_TRYSTERO_NO_EXCEPTIONS

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/post.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/json_resource.hpp>
#include <webcpp/trystero/native/data_channel.hpp>
#include <webcpp/trystero/protocol/json.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/json/storage_ptr.hpp>
#include <boost/json/value.hpp>
#include <rtc/peerconnection.hpp>

#include <deque>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

namespace webcpp::trystero::native {

/**
 * A peer connection of libdatachannel, with its negotiation driven by the signaling: automatic negotiation is off, so
 * an offer or an answer exists only when `set_local_description` asks for one, as in the browser.
 *
 * | libdatachannel                  | Here                                                            |
 * | events, on its own threads      | posted to the executor, through a weak reference               |
 * | exceptions on refused input     | `error::backend_failure`                                         |
 * | `Candidate`                     | the JSON of an `RTCIceCandidateInit`, as the browser writes it   |
 *
 * Tip: the connection, its handlers and the texts it hands to the room are in the room's resource; libdatachannel's
 * own objects, and the `std::string` and `rtc::Description` its API takes and returns, are libdatachannel's.
 */
class peer_connection : public backend::peer_connection, public std::enable_shared_from_this<peer_connection> {
    struct private_tag {
        explicit private_tag() = default;
    };

public:
    using description_handler = handler<void(backend::error_code, std::string_view, std::string_view)>;

    /**
     * Makes a peer connection that gathers through `servers`, bound to `bind_address` when it is not empty.
     *
     * A connection that is not bound gathers a host candidate on each interface but the loopback, which ICE never
     * offers (RFC 8445, 5.1.1.1); one bound to an address, as `"127.0.0.1"`, gathers on that address alone, which
     * lets two peers of one machine connect with no other network.
     *
     * @param ex The executor every handler of the connection runs on.
     * @param servers The STUN and TURN servers it gathers through.
     * @param bind_address The local address it binds to, as `"127.0.0.1"`; empty means every interface but the
     * loopback.
     * @param memory The resource the connection, its handlers and its texts are allocated in.
     * @return The new connection.
     */
    static std::shared_ptr<peer_connection> create(boost::asio::any_io_executor ex, std::span<const backend::ice_server> servers,
                                                   std::string_view bind_address, std::pmr::memory_resource* memory) {
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        if (!bind_address.empty()) config.bindAddress = std::string(bind_address);
        for (const auto& server : servers) {
            for (const auto& url : server.urls) {
                try {
                    config.iceServers.emplace_back(with_credentials(url, server.username, server.credential));
                } catch (...) {  // a URL libdatachannel cannot parse is skipped, as a browser skips an unusable server
                }
            }
        }
        const std::pmr::polymorphic_allocator<> allocator(memory);
        auto self = std::allocate_shared<peer_connection>(allocator, private_tag{}, std::move(ex),
                                                          std::allocate_shared<rtc::PeerConnection>(allocator, config), memory);
        self->start();
        return self;
    }

    peer_connection(private_tag /*tag*/, boost::asio::any_io_executor ex, std::shared_ptr<rtc::PeerConnection> pc, std::pmr::memory_resource* memory)
        : ex_(std::move(ex)), pc_(std::move(pc)), memory_(memory), json_(memory), pending_local_(memory) {}

    ~peer_connection() override {
        pc_->resetCallbacks();
        pc_->close();
    }

    /** The handlers, kept in a shared object so that one replacing them during its own call outlives the call. */
    void set_handlers(handlers h) override { h_ = std::allocate_shared<handlers>(allocator(), std::move(h)); }

    std::shared_ptr<backend::data_channel> create_data_channel(std::string_view label) override {
        return data_channel::create(ex_, pc_->createDataChannel(std::string(label)), memory_);
    }

    void set_local_description(description_handler done) override {
        pending_local_.push_back(std::move(done));
        try {
            pc_->setLocalDescription();
        } catch (...) {
            auto failed = std::move(pending_local_.back());
            pending_local_.pop_back();
            post([failed = std::move(failed)]() mutable { failed(make_error_code(error::backend_failure), {}, {}); });
        }
    }

    void set_remote_description(std::string_view type, std::string_view sdp, handler<void(backend::error_code)> done) override {
        backend::error_code ec;
        try {
            pc_->setRemoteDescription(rtc::Description(std::string(sdp), std::string(type)));
        } catch (...) {
            ec = make_error_code(error::backend_failure);
        }
        post([done = std::move(done), ec]() mutable { done(ec); });
    }

    void add_ice_candidate(std::string_view candidate_json, handler<void(backend::error_code)> done) override {
        backend::error_code ec;
        boost::system::error_code parse_error;
        const auto document = protocol::json::parse(candidate_json, boost::json::storage_ptr(&json_), parse_error);
        const auto* object = parse_error ? nullptr : document.if_object();
        const auto* candidate = object != nullptr ? object->if_contains("candidate") : nullptr;
        const auto* mid = object != nullptr ? object->if_contains("sdpMid") : nullptr;
        if (candidate == nullptr || !candidate->is_string()) {
            ec = make_error_code(error::backend_failure);
        } else if (!candidate->get_string().empty()) {  // an empty candidate marks the end of gathering
            try {
                const std::string text(candidate->get_string());
                const std::string mid_text = mid != nullptr && mid->is_string() ? std::string(mid->get_string()) : std::string("0");
                pc_->addRemoteCandidate(rtc::Candidate(text, mid_text));
            } catch (...) {
                ec = make_error_code(error::backend_failure);
            }
        }
        post([done = std::move(done), ec]() mutable { done(ec); });
    }

    bool has_remote_description() const override { return pc_->remoteDescription().has_value(); }

    void close() override { pc_->close(); }

    /** libdatachannel's own `rtc::PeerConnection`: what media will be added to (the documentation's roadmap). */
    const std::shared_ptr<rtc::PeerConnection>& native_handle() const { return pc_; }

private:
    std::pmr::polymorphic_allocator<> allocator() const { return std::pmr::polymorphic_allocator<>(memory_); }

    /** "turn:host" with a username and credential becomes "turn:user:credential@host", the form libdatachannel parses. */
    static std::string with_credentials(std::string_view url, std::string_view username, std::string_view credential) {
        if (username.empty()) return std::string(url);
        const auto colon = url.find(':');
        if (colon == std::string_view::npos) return std::string(url);
        std::string out(url.substr(0, colon + 1));
        out.append(username).append(":").append(credential).append("@").append(url.substr(colon + 1));
        return out;
    }

    /** Posts a completion from the executor, in the room's resource. */
    template <class F>
    void post(F&& f) {
        trystero::detail::post(ex_, memory_, std::forward<F>(f));
    }

    void start() {
        std::weak_ptr<peer_connection> weak = shared_from_this();
        auto ex = ex_;
        pc_->onLocalDescription([weak, ex](rtc::Description description) {
            boost::asio::post(ex, [weak, description = std::move(description)] {
                auto self = weak.lock();
                if (!self || self->pending_local_.empty()) return;
                auto done = std::move(self->pending_local_.front());
                self->pending_local_.pop_front();
                done({}, description.typeString(), std::string(description));
            });
        });
        pc_->onLocalCandidate([weak, ex](rtc::Candidate candidate) {
            boost::asio::post(ex, [weak, candidate = std::move(candidate)] {
                auto self = weak.lock();
                if (!self) return;
                auto h = self->h_;
                if (!h || !h->ice_candidate) return;
                std::pmr::string json(R"({"candidate":)", self->memory_);
                protocol::json::append_string(json, candidate.candidate());
                json += R"(,"sdpMid":)";
                protocol::json::append_string(json, candidate.mid());
                json += R"(,"sdpMLineIndex":0})";
                h->ice_candidate(json);
            });
        });
        pc_->onDataChannel([weak, ex](std::shared_ptr<rtc::DataChannel> channel) {
            boost::asio::post(ex, [weak, ex, channel = std::move(channel)]() mutable {
                auto self = weak.lock();
                if (!self) return;
                auto wrapped = data_channel::create(ex, std::move(channel), self->memory_);
                if (auto h = self->h_; h && h->data_channel) h->data_channel(std::move(wrapped));
            });
        });
        pc_->onStateChange([weak, ex](rtc::PeerConnection::State state) {
            boost::asio::post(ex, [weak, state] {
                if (auto self = weak.lock()) {
                    if (auto h = self->h_; h && h->state) h->state(map(state));
                }
            });
        });
    }

    static backend::connection_state map(rtc::PeerConnection::State state) {
        using backend::connection_state;
        switch (state) {
            case rtc::PeerConnection::State::New: return connection_state::fresh;
            case rtc::PeerConnection::State::Connecting: return connection_state::connecting;
            case rtc::PeerConnection::State::Connected: return connection_state::connected;
            case rtc::PeerConnection::State::Disconnected: return connection_state::disconnected;
            case rtc::PeerConnection::State::Failed: return connection_state::failed;
            case rtc::PeerConnection::State::Closed: return connection_state::closed;
        }
        return connection_state::fresh;
    }

    boost::asio::any_io_executor ex_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::pmr::memory_resource* memory_;
    json_resource json_;
    std::shared_ptr<handlers> h_;
    std::pmr::deque<description_handler> pending_local_;
};

}  // namespace webcpp::trystero::native

#endif  // WEBCPP_TRYSTERO_NO_EXCEPTIONS

#endif
