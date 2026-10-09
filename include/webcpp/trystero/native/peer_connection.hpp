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
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/native/data_channel.hpp>
#include <webcpp/trystero/protocol/json.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <rtc/peerconnection.hpp>

#include <deque>
#include <memory>
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
 */
class peer_connection : public backend::peer_connection, public std::enable_shared_from_this<peer_connection> {
public:
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
     * @return The new connection.
     */
    static std::shared_ptr<peer_connection> create(boost::asio::any_io_executor ex, std::span<const backend::ice_server> servers,
                                                   std::string_view bind_address = {}) {
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
        auto self = std::shared_ptr<peer_connection>(new peer_connection(std::move(ex), std::make_shared<rtc::PeerConnection>(config)));
        self->start();
        return self;
    }

    ~peer_connection() override {
        pc_->resetCallbacks();
        pc_->close();
    }

    void set_handlers(handlers h) override { h_ = std::move(h); }

    std::shared_ptr<backend::data_channel> create_data_channel(std::string_view label) override {
        return data_channel::create(ex_, pc_->createDataChannel(std::string(label)));
    }

    void set_local_description(std::function<void(backend::error_code, std::string_view, std::string_view)> done) override {
        pending_local_.push_back(std::move(done));
        try {
            pc_->setLocalDescription();
        } catch (...) {
            auto failed = std::move(pending_local_.back());
            pending_local_.pop_back();
            post([failed = std::move(failed)] { failed(make_error_code(error::backend_failure), {}, {}); });
        }
    }

    void set_remote_description(std::string_view type, std::string_view sdp, std::function<void(backend::error_code)> done) override {
        backend::error_code ec;
        try {
            pc_->setRemoteDescription(rtc::Description(std::string(sdp), std::string(type)));
        } catch (...) {
            ec = make_error_code(error::backend_failure);
        }
        post([done = std::move(done), ec] { done(ec); });
    }

    void add_ice_candidate(std::string_view candidate_json, std::function<void(backend::error_code)> done) override {
        backend::error_code ec;
        boost::system::error_code parse_error;
        const auto document = boost::json::parse(boost::json::string_view(candidate_json.data(), candidate_json.size()), parse_error);
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
        post([done = std::move(done), ec] { done(ec); });
    }

    bool has_remote_description() const override { return pc_->remoteDescription().has_value(); }

    void close() override { pc_->close(); }

    /** libdatachannel's own `rtc::PeerConnection`: what media will be added to (the documentation's roadmap). */
    const std::shared_ptr<rtc::PeerConnection>& native_handle() const { return pc_; }

private:
    peer_connection(boost::asio::any_io_executor ex, std::shared_ptr<rtc::PeerConnection> pc) : ex_(std::move(ex)), pc_(std::move(pc)) {}

    /** "turn:host" with a username and credential becomes "turn:user:credential@host", the form libdatachannel parses. */
    static std::string with_credentials(std::string_view url, std::string_view username, std::string_view credential) {
        if (username.empty()) return std::string(url);
        const auto colon = url.find(':');
        if (colon == std::string_view::npos) return std::string(url);
        std::string out(url.substr(0, colon + 1));
        out.append(username).append(":").append(credential).append("@").append(url.substr(colon + 1));
        return out;
    }

    template <class F>
    void post(F&& f) {
        boost::asio::post(ex_, std::forward<F>(f));
    }

    void start() {
        std::weak_ptr<peer_connection> weak = shared_from_this();
        auto ex = ex_;
        pc_->onLocalDescription([weak, ex](rtc::Description description) {
            boost::asio::post(ex, [weak, type = description.typeString(), sdp = std::string(description)] {
                auto self = weak.lock();
                if (!self || self->pending_local_.empty()) return;
                auto done = std::move(self->pending_local_.front());
                self->pending_local_.pop_front();
                done({}, type, sdp);
            });
        });
        pc_->onLocalCandidate([weak, ex](rtc::Candidate candidate) {
            std::string json = R"({"candidate":)";
            protocol::json::append_string(json, candidate.candidate());
            json += R"(,"sdpMid":)";
            protocol::json::append_string(json, candidate.mid());
            json += R"(,"sdpMLineIndex":0})";
            boost::asio::post(ex, [weak, json = std::move(json)] {
                if (auto self = weak.lock(); self && self->h_.ice_candidate) {
                    auto f = self->h_.ice_candidate;
                    f(json);
                }
            });
        });
        pc_->onDataChannel([weak, ex](std::shared_ptr<rtc::DataChannel> channel) {
            boost::asio::post(ex, [weak, ex, channel = std::move(channel)]() mutable {
                auto self = weak.lock();
                auto wrapped = data_channel::create(ex, std::move(channel));
                if (self && self->h_.data_channel) {
                    auto f = self->h_.data_channel;
                    f(std::move(wrapped));
                }
            });
        });
        pc_->onStateChange([weak, ex](rtc::PeerConnection::State state) {
            boost::asio::post(ex, [weak, state] {
                if (auto self = weak.lock(); self && self->h_.state) {
                    auto f = self->h_.state;
                    f(map(state));
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
    handlers h_;
    std::deque<std::function<void(backend::error_code, std::string_view, std::string_view)>> pending_local_;
};

}  // namespace webcpp::trystero::native

#endif  // WEBCPP_TRYSTERO_NO_EXCEPTIONS

#endif
