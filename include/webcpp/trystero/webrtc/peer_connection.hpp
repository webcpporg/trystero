// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_WEBRTC_PEER_CONNECTION_HPP
#define WEBCPP_TRYSTERO_WEBRTC_PEER_CONNECTION_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/webrtc/data_channel.hpp>

#include <webcpp/webrtc/error.hpp>
#include <webcpp/webrtc/options.hpp>
#include <webcpp/webrtc/peer_connection.hpp>
#include <webcpp/webrtc/runtime.hpp>
#include <webcpp/webrtc/types.hpp>

#include <webcpp/asio/bind_allocator.hpp>  // lint-world: binds an allocator only
#include <webcpp/asio/post.hpp>            // lint-world: posts handlers only

#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webcpp::trystero::webrtc {

namespace detail {

/**
 * The JSON of an `RTCIceCandidateInit`, as a browser's `RTCIceCandidate` gives it to
 * `JSON.stringify`: `candidate`, `sdpMid`, null when empty, `sdpMLineIndex`, null when unknown,
 * then `usernameFragment` when there is one; `errc::type` when a text is not UTF-8, which no
 * browser's is and JSON cannot carry.
 */
inline webcpp::document::result<std::pmr::string> candidate_json(
    std::string_view candidate, std::string_view sdp_mid,
    std::optional<std::uint16_t> sdp_m_line_index, std::string_view username_fragment,
    std::pmr::memory_resource* memory) {
    std::pmr::string json(R"({"candidate":)", memory);
    if (const auto quoted = protocol::json::append_string(json, candidate); !quoted) {
        return quoted.error();
    }
    json += R"(,"sdpMid":)";
    if (sdp_mid.empty()) {
        json += "null";
    } else if (const auto quoted = protocol::json::append_string(json, sdp_mid); !quoted) {
        return quoted.error();
    }
    json += R"(,"sdpMLineIndex":)";
    if (sdp_m_line_index) {
        protocol::json::append_integer(json, *sdp_m_line_index);
    } else {
        json += "null";
    }
    if (!username_fragment.empty()) {
        json += R"(,"usernameFragment":)";
        if (const auto quoted = protocol::json::append_string(json, username_fragment); !quoted) {
            return quoted.error();
        }
    }
    json += '}';
    return json;
}

}  // namespace detail

/**
 * A peer connection over webcpp::webrtc's, the backend's `RTCPeerConnection`, with its
 * negotiation driven by the signaling: an offer or an answer exists only when
 * `set_local_description` asks for one.
 *
 * It keeps a wait of each kind pending on its connection, re-armed on the executor, and reports
 * through the handlers what each completes with:
 *
 * | webcpp::webrtc                                      | Reported                              |
 * | `async_wait_ice_candidate`                          | the JSON of an `RTCIceCandidateInit`  |
 * | `async_wait_state_change`, and the ICE state's wait | one state, merging both               |
 * | `async_accept_data_channel`                         | the channel, as a `data_channel`      |
 *
 * The merged state is the browser's view of both: failed when either failed, then closed,
 * disconnected, connected (or ICE completed), connecting (or ICE checking), and new.
 *
 * Tip: the connection, its channels, its handlers, its operations and the texts it hands to the
 * app are in the app's resource; what the platform holds (libdatachannel's objects, or the page's)
 * is the platform's.
 *
 * @see "The backend's WebRTC", in the guide.
 */
class peer_connection : public backend::peer_connection,
                        public std::enable_shared_from_this<peer_connection> {
    struct private_tag {
        explicit private_tag() = default;
    };

    [[nodiscard]] std::pmr::polymorphic_allocator<> allocator() const { return {memory_}; }

    /** Binds `f` to the connection's resource, which webcpp::webrtc allocates its operation in. */
    template <class F>
    auto bound(F&& f) const {
        return webcpp::asio::bind_allocator(allocator(), std::forward<F>(f));
    }

    /** Posts a completion from the executor, in the connection's resource. */
    template <class F>
    void post(F&& f) {
        webcpp::asio::post(pc_.get_executor(), bound(std::forward<F>(f)));
    }

public:
    /** webcpp::webrtc's peer connection it holds. */
    using connection_type = ::webcpp::webrtc::basic_peer_connection<executor_type, allocator_type>;

    /** What `set_local_description` completes with: an error, or the type and the SDP. */
    using description_handler =
        handler<void(backend::error_code, std::string_view, std::string_view)>;

    /**
     * Makes a peer connection that gathers through `servers`, bound to `bind_address` when it is
     * not empty.
     *
     * A connection that is not bound gathers a host candidate on each interface but the loopback,
     * which ICE never offers (RFC 8445, 5.1.1.1); natively, one bound to an address, as
     * `"127.0.0.1"`, gathers on that address alone, which lets two peers of one machine connect
     * with no other network. The browser cannot honour it.
     *
     * @param runtime The runtime that counts the connection, which it keeps alive.
     * @param ex The executor every handler of the connection runs on.
     * @param servers The STUN and TURN servers it gathers through, read during the call.
     * @param bind_address The local address it binds to, natively; empty for every interface.
     * @param factory The backend, which makes its channels' timers, and which outlives every
     * operation of the connection.
     * @param memory The app's resource: the connection, its channels, its handlers and its texts
     * are allocated in it.
     * @return The connection, or webcpp::webrtc's error: `too_many_peer_connections` past the
     * runtime's cap, `syntax_error` for an ICE server URL the platform cannot parse,
     * `operation_error` for another refusal.
     */
    // NOLINTBEGIN(performance-unnecessary-value-param): sinks, moved on
    static boost::system::result<std::shared_ptr<peer_connection>> make(
        std::shared_ptr<::webcpp::webrtc::runtime> runtime, const executor_type& ex,
        std::span<const backend::ice_server> servers, std::string_view bind_address,
        backend::factory& factory, std::pmr::memory_resource* memory) {
        // NOLINTEND(performance-unnecessary-value-param)
        auto made = make_connection(*runtime, ex, servers, bind_address, memory);
        if (!made) {
            return made.error();
        }
        auto self = std::allocate_shared<peer_connection>(std::pmr::polymorphic_allocator<>(memory),
                                                          private_tag{}, std::move(runtime),
                                                          std::move(*made), factory, memory);
        self->start();
        return self;
    }

    /**
     * Makes webcpp::webrtc's connection alone, with the configuration `make` gives it.
     *
     * @param runtime The runtime that counts the connection.
     * @param ex The executor of the connection.
     * @param servers The STUN and TURN servers, read during the call.
     * @param bind_address The local address it binds to, natively.
     * @param memory The resource of the connection, and of the call's own lists.
     * @return The connection, or webcpp::webrtc's error, as `make` says.
     */
    static boost::system::result<connection_type> make_connection(
        ::webcpp::webrtc::runtime& runtime, const executor_type& ex,
        std::span<const backend::ice_server> servers, std::string_view bind_address,
        std::pmr::memory_resource* memory) {
        std::size_t count = 0;
        for (const auto& server : servers) {
            count += server.urls.size();
        }
        // Views of every URL, then the servers as webcpp::webrtc reads them, over those views.
        std::pmr::vector<std::string_view> urls(memory);
        urls.reserve(count);
        for (const auto& server : servers) {
            urls.insert(urls.end(), server.urls.begin(), server.urls.end());
        }
        std::pmr::vector<::webcpp::webrtc::ice_server> list(memory);
        list.reserve(servers.size());
        std::size_t at = 0;
        for (const auto& server : servers) {
            list.push_back({
                .urls = std::span<const std::string_view>(urls).subspan(at, server.urls.size()),
                .username = server.username,
                .credential = server.credential,
            });
            at += server.urls.size();
        }
        const ::webcpp::webrtc::configuration config{
            .ice_servers = list,
            .policy = ::webcpp::webrtc::ice_transport_policy::all,
            .bind_address = bind_address,
        };
        const ::webcpp::webrtc::options bounds;
        return connection_type::make(runtime, ex, config, bounds, allocator_type(memory));
    }

    /**
     * Constructs the wrapper; only `make` can, which then takes over the connection's events.
     *
     * @param runtime The runtime that counts the connection.
     * @param pc webcpp::webrtc's connection.
     * @param factory The backend, which makes its channels' timers.
     * @param memory The resource its handlers and its texts are allocated in.
     */
    peer_connection(private_tag /*tag*/, std::shared_ptr<::webcpp::webrtc::runtime> runtime,
                    connection_type pc, backend::factory& factory,
                    std::pmr::memory_resource* memory)
        : runtime_(std::move(runtime)), factory_(factory), memory_(memory), pc_(std::move(pc)) {}

    /** Not copyable: its operations reach it by its address. */
    peer_connection(const peer_connection&) = delete;
    /** Not copy-assignable: its operations reach it by its address. */
    peer_connection& operator=(const peer_connection&) = delete;

    /**
     * Destroys the wrapper, which closes the connection without reporting the close:
     * webcpp::webrtc aborts its pending operations, whose handlers find the wrapper gone.
     */
    ~peer_connection() override = default;

    /**
     * Replaces the handlers, kept in a shared object so that one replacing them during its own call
     * outlives the call.
     *
     * @param h The new handlers, kept in the connection's resource.
     */
    void set_handlers(handlers h) override {
        h_ = std::allocate_shared<handlers>(allocator(), std::move(h));
    }

    /**
     * Opens a data channel, with webcpp::webrtc's `create_data_channel`.
     *
     * @param label The channel's label, read during the call.
     * @return The channel, made in the connection's resource, or null when the platform refuses
     * to open one, as on a closed connection.
     */
    std::shared_ptr<backend::data_channel> create_data_channel(std::string_view label) override {
        auto made = pc_.create_data_channel(label);
        if (!made) {
            return nullptr;
        }
        return data_channel::make(runtime_, std::move(*made), factory_, memory_);
    }

    /**
     * Creates the offer or the answer the signaling state calls for and applies it.
     *
     * @param done Called on the executor with the description's type and SDP, or with
     * `error::backend_failure` when the platform refuses, or when the connection goes first.
     */
    void set_local_description(description_handler done) override {
        pc_.async_set_local_description(
            bound([done = std::move(done)](boost::system::error_code ec,
                                           const connection_type::session_description& d) mutable {
                if (ec) {
                    done(make_error_code(error::backend_failure), {}, {});
                    return;
                }
                done({}, type_name(d.type), d.sdp);
            }));
    }

    /**
     * Applies the remote peer's offer or answer.
     *
     * @param type "offer" or "answer", read during the call.
     * @param sdp The description's SDP, read during the call.
     * @param done Called on the executor once it is applied, or with `error::backend_failure` when
     * the platform refuses it, or when `type` is no description's.
     */
    void set_remote_description(std::string_view type, std::string_view sdp,
                                handler<void(backend::error_code)> done) override {
        const auto kind = type_of(type);
        if (!kind) {
            post([done = std::move(done)]() mutable {
                done(make_error_code(error::backend_failure));
            });
            return;
        }
        pc_.async_set_remote_description(
            {.type = *kind, .sdp = sdp},
            bound([done = std::move(done)](boost::system::error_code ec) mutable {
                done(ec ? make_error_code(error::backend_failure) : backend::error_code{});
            }));
    }

    /**
     * Adds a remote ICE candidate; the empty candidate that ends the remote gathering adds none.
     *
     * @param candidate_json The JSON of an `RTCIceCandidateInit`, read during the call, at most
     * `protocol::max_sealed_candidate_bytes`, the bound of a sealed candidate, which its opened
     * JSON never passes.
     * @param done Called on the executor once it is added, or with `error::backend_failure` when
     * the JSON has no candidate or the platform refuses it.
     */
    void add_ice_candidate(std::string_view candidate_json,
                           handler<void(backend::error_code)> done) override {
        // The document lives in the app's resource for the call: the platform copies the views.
        const auto document = protocol::json::parse(
            candidate_json, protocol::json::protocol_limits(protocol::max_sealed_candidate_bytes),
            memory_);
        const protocol::json::const_value object =
            document.has_value() ? document->root() : protocol::json::const_value();
        const protocol::json::const_value text = object["candidate"];
        if (!object.is_object() || !text.is_string()) {
            post([done = std::move(done)]() mutable {
                done(make_error_code(error::backend_failure));
            });
            return;
        }
        ::webcpp::webrtc::ice_candidate_view candidate{.candidate = protocol::json::text_of(text)};
        if (const protocol::json::const_value mid = object["sdpMid"]; mid.is_string()) {
            candidate.sdp_mid = protocol::json::text_of(mid);
        }
        if (const auto index = object["sdpMLineIndex"].get<std::int64_t>();
            index.has_value() && *index >= 0 && *index <= 65535) {
            candidate.sdp_m_line_index = static_cast<std::uint16_t>(*index);
        }
        if (const protocol::json::const_value fragment = object["usernameFragment"];
            fragment.is_string()) {
            candidate.username_fragment = protocol::json::text_of(fragment);
        }
        pc_.async_add_ice_candidate(
            candidate, bound([done = std::move(done)](boost::system::error_code ec) mutable {
                done(ec ? make_error_code(error::backend_failure) : backend::error_code{});
            }));
    }

    /**
     * Whether a remote description was applied.
     *
     * @return Whether the connection has a `remoteDescription`.
     */
    [[nodiscard]] bool has_remote_description() const override {
        return pc_.has_remote_description();
    }

    /** Closes the connection and its channels, as the W3C's `close()`: no state is reported. */
    void close() override { pc_.close(); }

private:
    using state = ::webcpp::webrtc::peer_connection_state;
    using ice_state = ::webcpp::webrtc::ice_connection_state;

    static std::string_view type_name(::webcpp::webrtc::sdp_type type) {
        switch (type) {
            case ::webcpp::webrtc::sdp_type::offer: return "offer";
            case ::webcpp::webrtc::sdp_type::answer: return "answer";
            case ::webcpp::webrtc::sdp_type::pranswer: return "pranswer";
            case ::webcpp::webrtc::sdp_type::rollback: return "rollback";
        }
        return "offer";
    }

    static std::optional<::webcpp::webrtc::sdp_type> type_of(std::string_view name) {
        if (name == "offer") {
            return ::webcpp::webrtc::sdp_type::offer;
        }
        if (name == "answer") {
            return ::webcpp::webrtc::sdp_type::answer;
        }
        if (name == "pranswer") {
            return ::webcpp::webrtc::sdp_type::pranswer;
        }
        if (name == "rollback") {
            return ::webcpp::webrtc::sdp_type::rollback;
        }
        return std::nullopt;
    }

    void start() {
        watch_candidates();
        watch_state();
        watch_ice_state();
        accept_channels();
    }

    // Each wait re-arms itself before it reports, on the executor, while the wrapper lives; the
    // wrapper's end, or the connection's close, ends them with an error.

    void watch_candidates() {
        pc_.async_wait_ice_candidate(
            bound([weak = weak_from_this()](boost::system::error_code ec,
                                            const connection_type::ice_candidate& candidate) {
                if (const auto self = weak.lock(); self && !ec) {
                    // The end of gathering, an empty candidate, ends the candidates: none follows.
                    if (candidate.candidate.empty()) {
                        return;
                    }
                    self->watch_candidates();
                    self->on_candidate(candidate);
                }
            }));
    }

    void watch_state() {
        pc_.async_wait_state_change(
            bound([weak = weak_from_this()](boost::system::error_code ec, state s) {
                if (const auto self = weak.lock(); self && !ec) {
                    self->watch_state();
                    self->state_ = s;
                    self->report();
                }
            }));
    }

    void watch_ice_state() {
        pc_.async_wait_ice_connection_state_change(
            bound([weak = weak_from_this()](boost::system::error_code ec, ice_state s) {
                if (const auto self = weak.lock(); self && !ec) {
                    self->watch_ice_state();
                    self->ice_state_ = s;
                    self->report();
                }
            }));
    }

    void accept_channels() {
        pc_.async_accept_data_channel(
            bound([weak = weak_from_this()](boost::system::error_code ec,
                                            data_channel::channel_type channel) {
                if (const auto self = weak.lock(); self && !ec) {
                    self->accept_channels();
                    self->on_channel(std::move(channel));
                }
            }));
    }

    // The handlers run from a copy of the shared object, which a handler may replace.

    /**
     * Reports `candidate` as the JSON of an `RTCIceCandidateInit`, as the browser writes it; one
     * whose text is not UTF-8, which no browser's is and JSON cannot carry, is not reported.
     */
    void on_candidate(const connection_type::ice_candidate& candidate) {
        const auto h = h_;
        if (!h || !h->ice_candidate) {
            return;
        }
        const auto json = detail::candidate_json(candidate.candidate, candidate.sdp_mid,
                                                 candidate.sdp_m_line_index,
                                                 candidate.username_fragment, memory_);
        if (!json.has_value()) {
            return;
        }
        h->ice_candidate(*json);
    }

    void on_channel(data_channel::channel_type channel) {
        auto wrapped = data_channel::make(runtime_, std::move(channel), factory_, memory_);
        if (const auto h = h_; h && h->data_channel) {
            h->data_channel(std::move(wrapped));
        }
    }

    void report() {
        if (const auto h = h_; h && h->state) {
            h->state(merged());
        }
    }

    /** The connection's state and its ICE state as one, as a page sees them together. */
    [[nodiscard]] backend::connection_state merged() const {
        using backend::connection_state;
        if (state_ == state::failed || ice_state_ == ice_state::failed) {
            return connection_state::failed;
        }
        if (state_ == state::closed || ice_state_ == ice_state::closed) {
            return connection_state::closed;
        }
        if (state_ == state::disconnected || ice_state_ == ice_state::disconnected) {
            return connection_state::disconnected;
        }
        if (state_ == state::connected || ice_state_ == ice_state::connected ||
            ice_state_ == ice_state::completed) {
            return connection_state::connected;
        }
        if (state_ == state::connecting || ice_state_ == ice_state::checking) {
            return connection_state::connecting;
        }
        return connection_state::fresh;
    }

    // The runtime outlives the connection, which goes first.
    std::shared_ptr<::webcpp::webrtc::runtime> runtime_;
    backend::factory& factory_;
    std::pmr::memory_resource* memory_;
    std::shared_ptr<handlers> h_;
    state state_ = state::new_;
    ice_state ice_state_ = ice_state::new_;
    connection_type pc_;
};

}  // namespace webcpp::trystero::webrtc

#endif
