// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences-behaviour): a JSON payload nested deeper
// than 256, holding a lone surrogate, or whose document passes the room's bound. Trystero JS
// delivers all three, as JSON.parse reads them; a room here delivers none, since it reads a JSON
// payload with webcpp::document's defaults, at most 256 deep, and strings that are UTF-8, into an
// image of at most room_options::max_json_document_bytes, which this program's room sets to 64 KiB:
// an array of 1,000 zeros, a node of 192 bytes each, passes it. A Trystero JS page sends such
// payloads, and a C++
// peer sends no lone surrogate, which a document's UTF-8 strings cannot hold, so the other peer of
// this program sends what a page sends: each payload's JSON text, as JSON.stringify writes the
// value, as a text payload, which the backend its app is given marks as JSON on the wire. This
// program's room receives the payloads, and a last one sent after them. Two apps of one program
// meet in a room, as two pages would. Its twin is test/oracle/twins/diff_json_refused.mjs. Both
// meet over the relay whose URL is in the environment, TRYSTERO_RELAY_URL, which their lanes start
// on 127.0.0.1.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/protocol/action_wire.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>
#include <webcpp/trystero/room.hpp>

#include <webcpp/document/document.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace trystero = webcpp::trystero;
namespace protocol = webcpp::trystero::protocol;
namespace wd = webcpp::document;

namespace {

// The action the payloads travel on.
constexpr std::string_view action_name = "payload";

// A data channel of the backend's, shared by its owners.
using channel_ptr = std::shared_ptr<trystero::backend::data_channel>;

// A data channel that forwards to another, and sends each text payload of the action as a JSON
// payload of the same text: the frame a Trystero JS page sends of a value whose JSON.stringify is
// that text.
class json_marking_channel final : public trystero::backend::data_channel {
public:
    json_marking_channel(channel_ptr channel, std::pmr::memory_resource* memory)
        : channel_(std::move(channel)), memory_(memory) {}

    void set_handlers(handlers h) override { channel_->set_handlers(std::move(h)); }

    [[nodiscard]] bool is_open() const override { return channel_->is_open(); }

    void send(std::span<const std::uint8_t> frame) override {
        const auto envelope = protocol::decode_envelope(frame);
        if (envelope && envelope->type == protocol::envelope::kind::room) {
            const auto decoded = protocol::decode_action(envelope->payload);
            const auto* const p =
                decoded.frame ? std::get_if<protocol::proposal>(&*decoded.frame) : nullptr;
            if (p != nullptr && p->kind == protocol::frame_kind::inline_payload &&
                p->format == protocol::payload_format::text && p->name == action_name) {
                const auto action = protocol::encode_inline(p->name, protocol::payload_format::json,
                                                            p->metadata, p->payload, memory_);
                channel_->send(protocol::room_frame(envelope->token, action, memory_));
                return;
            }
        }
        channel_->send(frame);
    }

    [[nodiscard]] std::size_t buffered_amount() const override {
        return channel_->buffered_amount();
    }

    void close() override { channel_->close(); }

private:
    channel_ptr channel_;
    std::pmr::memory_resource* memory_;
};

// The channel `channel` marking its text payloads of the action as JSON, made in `memory`; null for
// none.
channel_ptr marking(channel_ptr channel, std::pmr::memory_resource* memory) {
    if (!channel) {
        return nullptr;
    }
    return std::allocate_shared<json_marking_channel>(std::pmr::polymorphic_allocator<>(memory),
                                                      std::move(channel), memory);
}

// A peer connection that forwards to another, and whose data channels, those it opens and those
// the remote peer opens, are json_marking_channel's.
class json_marking_connection final : public trystero::backend::peer_connection {
public:
    json_marking_connection(std::shared_ptr<trystero::backend::peer_connection> connection,
                            std::pmr::memory_resource* memory)
        : connection_(std::move(connection)), memory_(memory) {}

    void set_handlers(handlers h) override {
        if (h.data_channel) {
            h.data_channel = trystero::handler<void(channel_ptr)>(
                std::allocator_arg, std::pmr::polymorphic_allocator<>(memory_),
                [memory = memory_, deliver = std::move(h.data_channel)](
                    channel_ptr channel) mutable { deliver(marking(std::move(channel), memory)); });
        }
        connection_->set_handlers(std::move(h));
    }

    channel_ptr create_data_channel(std::string_view label) override {
        return marking(connection_->create_data_channel(label), memory_);
    }

    void set_local_description(
        trystero::handler<void(boost::system::error_code, std::string_view, std::string_view)> done)
        override {
        connection_->set_local_description(std::move(done));
    }

    void set_remote_description(std::string_view type, std::string_view sdp,
                                trystero::handler<void(boost::system::error_code)> done) override {
        connection_->set_remote_description(type, sdp, std::move(done));
    }

    void add_ice_candidate(std::string_view candidate_json,
                           trystero::handler<void(boost::system::error_code)> done) override {
        connection_->add_ice_candidate(candidate_json, std::move(done));
    }

    [[nodiscard]] bool has_remote_description() const override {
        return connection_->has_remote_description();
    }

    void close() override { connection_->close(); }

private:
    std::shared_ptr<trystero::backend::peer_connection> connection_;
    std::pmr::memory_resource* memory_;
};

// A backend that forwards to another, and whose peer connections are json_marking_connection's.
class json_marking_backend final : public trystero::backend::factory {
public:
    explicit json_marking_backend(trystero::backend::factory& backend) : backend_(backend) {}

    std::shared_ptr<trystero::backend::websocket> connect(
        std::string_view url, trystero::backend::websocket::handlers h,
        std::pmr::memory_resource* memory) override {
        return backend_.connect(url, std::move(h), memory);
    }

    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
        auto connection = backend_.make_peer_connection(servers, memory);
        if (!connection) {
            return nullptr;
        }
        return std::allocate_shared<json_marking_connection>(
            std::pmr::polymorphic_allocator<>(memory), std::move(connection), memory);
    }

    std::shared_ptr<trystero::backend::timer> make_timer(
        std::pmr::memory_resource* memory) override {
        return backend_.make_timer(memory);
    }

    std::shared_ptr<trystero::backend::sealer> make_sealer(
        std::array<std::uint8_t, 32> key, std::pmr::memory_resource* memory) override {
        return backend_.make_sealer(key, memory);
    }

    void random(std::span<std::uint8_t> out) override { backend_.random(out); }

    std::chrono::sys_seconds unix_now() override { return backend_.unix_now(); }

    std::chrono::steady_clock::time_point steady_now() override { return backend_.steady_now(); }

private:
    trystero::backend::factory& backend_;
};

}  // namespace

int main() {
    const char* const relay = std::getenv("TRYSTERO_RELAY_URL");
    if (relay == nullptr) {
        std::cerr << "TRYSTERO_RELAY_URL names no relay\n";
        return 1;
    }
    boost::asio::io_context io;
    // The peers meet on this machine alone: every connection bound to the loopback, and a STUN
    // server where nothing answers in place of Trystero's public ones.
    trystero::platform::backend backend(io.get_executor(), {.bind_address = "127.0.0.1"});
    const auto config = [relay] {
        trystero::app_config c;
        c.app_id = "webcpp-trystero-differences";
        c.relay_urls = {relay};
        c.ice_servers = {{.urls = {"stun:127.0.0.1:9"}, .username = {}, .credential = {}}};
        // The connection closes once no room uses it, so that the program ends when both leave.
        c.idle_timeout = std::chrono::milliseconds{0};
        return c;
    };
    // The other peer, as a Trystero JS page would be: its app's backend marks the text payloads of
    // the action as JSON.
    json_marking_backend marking_backend(backend);
    trystero::app other_app(io.get_executor(), marking_backend, config());
    auto other = other_app.join("lobby");
    trystero::app app(io.get_executor(), backend, config());
    // tag::bound[]
    // A room whose received JSON payloads take at most 64 KiB of its resource each.
    auto joined = app.join("lobby", {.max_json_document_bytes = std::uint64_t{64} << 10U});
    // end::bound[]
    if (!other || !joined) {
        std::cerr << "not joined\n";
        return 1;
    }
    trystero::room room = std::move(*joined);
    const auto leave = [&] {
        room.async_leave([](boost::system::error_code) {});
        other->async_leave([](boost::system::error_code) {});
    };
    // tag::example[]
    // The payloads the other peer sends, as JSON text, each an array of its label and a value: one
    // nested 257 deep, one holding a lone surrogate, an array of 1,000 zeros past the room's bound,
    // and a last one sent after them.
    std::string zeros = "[0";
    for (std::size_t i = 1; i < 1000; ++i) {
        zeros += ",0";
    }
    const auto payloads = std::to_array<std::pair<std::string_view, std::string>>({
        {"nested 257 deep", std::string(256, '[') + std::string(256, ']')},
        {"holding a lone surrogate", R"("\ud800")"},
        {"of 1,000 zeros", zeros + "]"},
        {"sent after them", R"("plain")"},
    });
    // What this room delivers of them, by label, once the last has come.
    auto payload = room.make_action<wd::document>(action_name);
    std::vector<std::string> delivered;
    payload.on_message([&](const wd::document& value, const trystero::message_info&) {
        delivered.emplace_back(value[0].value_or(std::string_view()));
        if (delivered.back() != payloads.back().first) {
            return;
        }
        for (const auto& sent : payloads) {
            const bool came = std::ranges::find(delivered, sent.first) != delivered.end();
            std::cout << "the payload " << sent.first << ": "
                      << (came ? "delivered" : "not delivered") << '\n';
        }
        leave();
    });
    // end::example[]
    // The other peer sends each payload's text once it meets this room. An inline frame leaves at
    // once, so they leave in the order of the sends, as a Trystero JS page awaits each.
    auto sending = other->make_action<std::string>(action_name);
    other->on_peer_join([&](std::string_view) {
        for (const auto& [label, value] : payloads) {
            sending.async_send("[\"" + std::string(label) + "\"," + value + "]",
                               [](boost::system::error_code) {});
        }
    });
    io.run();  // lint-run: the program's network I/O; libdatachannel's threads post to it
}
