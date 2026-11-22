// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests whole rooms of C++ peers in one process, over the memory backend of
// support/memory_backend.hpp. WEBCPP_TEST_TRYSTERO_VERBOSE in the environment prints each room's
// log on the standard error.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/identity.hpp>
#include <webcpp/trystero/protocol/action_wire.hpp>
#include <webcpp/trystero/protocol/handshake.hpp>
#include <webcpp/trystero/protocol/handshake_reason.hpp>
#include <webcpp/trystero/protocol/ids.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/room.hpp>

#include <webcpp/asio/as_tuple.hpp>
#include <webcpp/asio/co_spawn.hpp>
#include <webcpp/asio/detached.hpp>
#include <webcpp/asio/io_context.hpp>
#include <webcpp/asio/use_awaitable.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "cases.hpp"
#include "require.hpp"
#include "support/memory_backend.hpp"
#include "support/rooms.hpp"

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using namespace std::chrono_literals;

namespace {

using trystero::test::app_config;
using trystero::test::app_id;
using trystero::test::counting_resource;
using trystero::test::document_of;
using trystero::test::endpoint_scalar;
using trystero::test::fixture;
using trystero::test::flooder;
using trystero::test::join;
using trystero::test::label_beside;
using trystero::test::ordered_labels;
using trystero::test::peer_id_of;
using trystero::test::relay_a;
using trystero::test::relay_b;
using trystero::test::relayed_signal;
using trystero::test::run_until;

/** An app's state and a room of it, without their handles, for a case that calls the room as its
 * signaling does. The case abandons the room before the fixture goes. */
struct app_room {
    std::shared_ptr<trystero::detail::app_impl> app;
    std::shared_ptr<trystero::detail::room_impl> room;
};

app_room make_app_room(fixture& f, std::string_view label, std::string_view room_id,
                       std::pmr::memory_resource* memory = std::pmr::get_default_resource()) {
    const std::pmr::polymorphic_allocator<> alloc(memory);
    trystero::app_config config{
        .app_id = app_id,
        .relay_urls = {relay_a, relay_b},
        .memory = memory,
    };
    app_room out;
    const auto scalar = trystero::test::endpoint_scalar(label);
    out.app = std::allocate_shared<trystero::detail::app_impl>(
        alloc, f.io.get_executor(), f.net, std::move(config),
        std::span<const std::uint8_t>(scalar));
    out.room = trystero::detail::room_impl::make(out.app, {}, room_id);
    return out;
}

/** A connection negotiated over the memory network outside any room: its local end, which a case
 * hands a room as its signaling would, and the remote end's channel, which records what it
 * receives. */
struct raw_pair {
    std::shared_ptr<trystero::backend::peer_connection> local_pc;
    std::shared_ptr<trystero::backend::data_channel> local;
    std::shared_ptr<trystero::backend::peer_connection> remote_pc;
    std::shared_ptr<trystero::backend::data_channel> remote;
    std::shared_ptr<std::vector<std::vector<std::uint8_t>>> received =
        std::make_shared<std::vector<std::vector<std::uint8_t>>>();
    /** The local end's descriptions: its offer, and the answer it applied. */
    std::string offer;
    std::string answer;
};

raw_pair negotiate(fixture& f) {
    auto* m = std::pmr::get_default_resource();
    raw_pair p;
    p.local_pc = f.net.make_peer_connection({}, m);
    p.remote_pc = f.net.make_peer_connection({}, m);
    p.local = p.local_pc->create_data_channel("data");
    auto remote = std::make_shared<std::shared_ptr<trystero::backend::data_channel>>();
    p.remote_pc->set_handlers({
        .ice_candidate = {},
        .data_channel =
            [remote](std::shared_ptr<trystero::backend::data_channel> c) {
                *remote = std::move(c);
            },
        .state = {},
    });
    std::pmr::string offer(m);
    std::pmr::string answer(m);
    p.local_pc->set_local_description([&offer](trystero::backend::error_code, std::string_view,
                                               std::string_view sdp) { offer = sdp; });
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return !offer.empty(); })));
    p.remote_pc->set_remote_description("offer", offer, [](trystero::backend::error_code) {});
    p.remote_pc->set_local_description([&answer](trystero::backend::error_code, std::string_view,
                                                 std::string_view sdp) { answer = sdp; });
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return !answer.empty(); })));
    p.local_pc->set_remote_description("answer", answer, [](trystero::backend::error_code) {});
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return p.local->is_open() && *remote && (*remote)->is_open(); })));
    p.remote_pc->set_handlers({});
    p.offer = offer;
    p.answer = answer;
    p.remote = *remote;
    p.remote->set_handlers({
        .open = {},
        .message =
            [received = p.received](trystero::backend::bytes frame) {
                received->emplace_back(frame.begin(), frame.end());
            },
        .close = {},
        .buffered_amount_low = {},
    });
    return p;
}

void two_peers_join_and_exchange_every_payload_format() {
    fixture f{true};
    // A's id sorts below z's, so A offers and z answers.
    const auto labels = ordered_labels(2, "lobby");
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(labels[0]));
    auto a = join(a_app, "lobby");
    trystero::test::app z_app(f.io.get_executor(), f.net, app_config(labels[1]));
    auto z = join(z_app, "lobby");
    std::vector<std::string> joined_a;
    std::vector<std::string> joined_z;
    a.on_peer_join([&](const trystero::peer_event& e) { joined_a.emplace_back(e.peer_id); });
    z.on_peer_join([&](const trystero::peer_event& e) { joined_z.emplace_back(e.peer_id); });

    auto chat_a = a.make_action<std::string>("chat");
    auto chat_z = z.make_action<std::string>("chat");
    auto drink_z = z.make_action<webcpp::document::document>("drink");
    auto pic_z = z.make_action<trystero::bytes>("pic");
    std::vector<std::pair<std::string, trystero::message_info>> chats_z;
    std::optional<webcpp::document::document> drink;
    trystero::bytes pic;
    chat_z.on_message([&](std::string text, const trystero::message_info& info) {
        chats_z.emplace_back(std::move(text), info);
    });
    // tag::json-receive[]
    drink_z.on_message(
        [&](webcpp::document::document v, const trystero::message_info&) { drink = std::move(v); });
    // end::json-receive[]
    pic_z.on_message([&](trystero::bytes b, const trystero::message_info&) { pic = std::move(b); });

    BOOST_TEST(run_until(f, [&] { return joined_a.size() == 1 && joined_z.size() == 1; }));
    BOOST_TEST_EQ(joined_a.at(0), z_app.self_id());
    BOOST_TEST_EQ(joined_z.at(0), a_app.self_id());
    const auto peers = a.peers();
    webcpp::test::require(BOOST_TEST_EQ(peers.size(), 1U));
    BOOST_TEST_EQ(peers.begin()->first, z_app.self_id());
    BOOST_TEST_EQ(peers.begin()->second.user_id, z_app.user_id());

    std::vector<boost::system::error_code> sent;
    chat_a.async_send("hello", trystero::send_options{.targets = {}, .metadata = R"({"n":1})"},
                      [&](boost::system::error_code ec) { sent.push_back(ec); });
    // tag::json-send[]
    if (auto negroni = webcpp::document::parse(R"({"drink":"negroni"})")) {
        a.make_action<webcpp::document::document>("drink").async_send(
            std::move(*negroni), [&](boost::system::error_code ec) { sent.push_back(ec); });
    }
    // end::json-send[]
    a.make_action<trystero::bytes>("pic").async_send(
        trystero::bytes{0, 1, 255}, [&](boost::system::error_code ec) { sent.push_back(ec); });

    BOOST_TEST(
        run_until(f, [&] { return chats_z.size() == 1 && drink.has_value() && pic.size() == 3; }));
    BOOST_TEST_EQ(sent.size(), 3U);
    for (const auto& ec : sent) {
        BOOST_TEST(!ec);
    }
    BOOST_TEST_EQ(chats_z.at(0).first, "hello");
    BOOST_TEST_EQ(chats_z.at(0).second.from, a_app.self_id());
    BOOST_TEST_EQ(chats_z.at(0).second.metadata, R"({"n":1})");
    const trystero::bytes sent_pic{0, 1, 255};
    BOOST_TEST_ALL_EQ(pic.begin(), pic.end(), sent_pic.begin(), sent_pic.end());
    if (!BOOST_TEST(drink.has_value())) {
        return;
    }
    // tag::json-read[]
    BOOST_TEST_EQ((*drink)["drink"].value_or(std::string_view()), "negroni");
    // end::json-read[]
}

void large_payloads_travel_in_chunks_both_ways() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("BBBBBBBBBBBBBBBBBBBB"));
    auto a = join(a_app, "files");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("CCCCCCCCCCCCCCCCCCCC"));
    auto b = join(b_app, "files");
    auto file_a = a.make_action<trystero::bytes>("file");
    auto file_b = b.make_action<trystero::bytes>("file");
    trystero::bytes got_a;
    trystero::bytes got_b;
    std::string meta_b;
    file_a.on_message(
        [&](trystero::bytes v, const trystero::message_info&) { got_a = std::move(v); });
    file_b.on_message([&](trystero::bytes v, const trystero::message_info& info) {
        got_b = std::move(v);
        meta_b = info.metadata;
    });
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }));

    trystero::bytes big(100'000);
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<std::uint8_t>(i * 31);
    }
    std::size_t done = 0;
    file_a.async_send(big,
                      trystero::send_options{.targets = {}, .metadata = R"({"name":"big.bin"})"},
                      [&](boost::system::error_code ec) {
                          BOOST_TEST(!ec);
                          ++done;
                      });
    file_b.async_send(trystero::bytes(big.rbegin(), big.rend()), [&](boost::system::error_code ec) {
        BOOST_TEST(!ec);
        ++done;
    });
    BOOST_TEST(run_until(f, [&] { return done == 2 && !got_a.empty() && !got_b.empty(); }));
    BOOST_TEST_ALL_EQ(got_b.begin(), got_b.end(), big.begin(), big.end());
    const trystero::bytes reversed(big.rbegin(), big.rend());
    BOOST_TEST_ALL_EQ(got_a.begin(), got_a.end(), reversed.begin(), reversed.end());
    BOOST_TEST_EQ(meta_b, R"({"name":"big.bin"})");
}

/** The vectors recorded from 0.27.0's code (fixtures/expected/trystero-0.27.0.json), which main
 * loads from the path it is given before it runs a case. */
std::optional<webcpp::document::document>& vectors() {
    static std::optional<webcpp::document::document> document;
    return document;
}

/** `frame` in lowercase hexadecimal, as the vectors write a frame. */
std::string hex_of(std::span<const std::uint8_t> frame) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(frame.size() * 2);
    for (const auto b : frame) {
        out += digits[b >> 4U];
        out += digits[b & 15U];
    }
    return out;
}

/**
 * Sends, from A to B over `f`'s network, the 200 KiB binary message of the vectors' transfer case
 * `name` of `action_frames` ("file", `{"name":"big.bin"}`), and checks every frame of its transfer
 * against the case's, in order: who sent it, its size, and its bytes where the case recorded them
 * whole (the offer, "accepted") or its header where it recorded only that (a chunk). The payload
 * arrives whole.
 */
void a_transfer_sends_the_frames_of(fixture& f, std::string_view name) {
    namespace tp = trystero::protocol;
    webcpp::test::require(BOOST_TEST(vectors().has_value()));
    webcpp::document::basic_const_value<> recorded;
    for (const auto entry : vectors()->root()["action_frames"]) {
        if (entry["case"].value_or(std::string_view()) == name) {
            recorded = entry;
        }
    }
    webcpp::test::require(BOOST_TEST(recorded["frames"].is_array()));
    // The frames of a transfer: its offer, its answer and its chunks, with the connection that
    // sent each.
    std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> frames;
    f.net.filter = [&](std::size_t from, std::size_t, std::span<const std::uint8_t> frame) {
        if (const auto decoded = tp::decode_action(frame).frame) {
            const auto* p = std::get_if<tp::proposal>(&*decoded);
            if (p == nullptr || p->kind == tp::frame_kind::offer) {
                frames.emplace_back(from, std::vector<std::uint8_t>(frame.begin(), frame.end()));
            }
        }
        return trystero::test::memory_network::verdict::deliver;
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("AAAAAAAAAAAAAAAAAAAA"));
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("BBBBBBBBBBBBBBBBBBBB"));
    auto a = join(a_app, "wide");
    auto b = join(b_app, "wide");
    trystero::bytes got;
    b.make_action<trystero::bytes>("file").on_message(
        [&](trystero::bytes v, const trystero::message_info&) { got = std::move(v); });
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; })));
    trystero::bytes payload(
        static_cast<std::size_t>(recorded["payload_size"].value_or(std::int64_t{0})));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i * 31);
    }
    std::optional<boost::system::error_code> sent;
    a.make_action<trystero::bytes>("file").async_send(
        payload,
        trystero::send_options{
            .targets = {},
            .metadata = std::pmr::string(recorded["metadata"].value_or(std::string_view())),
        },
        [&](boost::system::error_code ec) { sent = ec; });
    BOOST_TEST(run_until(f, [&] { return sent.has_value() && !got.empty(); }));
    BOOST_TEST(sent.has_value() && !*sent);
    BOOST_TEST_ALL_EQ(got.begin(), got.end(), payload.begin(), payload.end());

    std::size_t i = 0;
    for (const auto expected : recorded["frames"]) {
        if (!BOOST_TEST_LT(i, frames.size())) {
            break;
        }
        const auto& [from, frame] = frames[i];
        const bool from_a = from == frames.front().first;
        BOOST_TEST_EQ(from_a ? "A" : "B", expected["from"].value_or(std::string_view()));
        BOOST_TEST_EQ(static_cast<std::int64_t>(frame.size()),
                      expected["size"].value_or(std::int64_t{-1}));
        const auto hex = hex_of(frame);
        if (expected["hex"].is_string()) {
            BOOST_TEST_EQ(hex, expected["hex"].value_or(std::string_view()));
        } else {
            const auto head = expected["head"].value_or(std::string_view());
            BOOST_TEST(!head.empty());
            BOOST_TEST_EQ(std::string_view(hex).substr(0, head.size()), head);
        }
        ++i;
    }
    BOOST_TEST_EQ(frames.size(), i);
}

// 0.27.0 sends a transfer's chunks wide to a peer whose connection's SCTP takes 64 KiB messages
// (`peer-transport.ts`'s `maxBulkFrameBytes`), as the memory network's does by default, Chrome's
// 256 KiB: the offer carries the flag 64 and each chunk fills a 64 KiB message.
void a_wide_link_sends_wide_chunks() {
    fixture f{true};
    a_transfer_sends_the_frames_of(f, "bulk_wide");
}

// To a peer whose connection takes smaller messages, or does not say, 0.27.0 sends its chunks
// narrow, each a 16 KiB frame.
void a_narrow_link_sends_narrow_chunks() {
    for (const auto size :
         {std::optional<std::size_t>(std::size_t{16} * 1024),
          std::optional<std::size_t>(trystero::protocol::max_bulk_frame_bytes - 1),
          std::optional<std::size_t>()}) {
        fixture f{true};
        f.net.max_message_size = size;
        a_transfer_sends_the_frames_of(f, "bulk");
    }
}

// A channel that refuses a chunk its connection announced it takes (the memory network says 256 KiB
// and its channels send at most a narrow frame) closes, as the platform's channel closes on a
// refused send: the transfer ends with the peer (peer_disconnected), rather than waiting on a chunk
// that was never sent until it times out (transfer_timed_out).
void a_refused_chunk_ends_the_transfer_with_the_peer() {
    fixture f{true};
    f.net.send_limit = trystero::protocol::max_action_frame_bytes;
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("AAAAAAAAAAAAAAAAAAAA"));
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("BBBBBBBBBBBBBBBBBBBB"));
    auto a = join(a_app, "refused");
    auto b = join(b_app, "refused");
    bool got = false;
    b.make_action<trystero::bytes>("file").on_message(
        [&](const trystero::bytes&, const trystero::message_info&) { got = true; });
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; })));
    // The peers' departure is seen as events: the apps meet again by their announcements, so that
    // "no peers" is a state that a reconnection may already have left by the time it is looked at.
    std::size_t a_left = 0;
    std::size_t b_left = 0;
    a.on_peer_leave([&](const trystero::peer_event&) { ++a_left; });
    b.on_peer_leave([&](const trystero::peer_event&) { ++b_left; });
    std::optional<boost::system::error_code> sent;
    a.make_action<trystero::bytes>("file").async_send(
        trystero::bytes(std::size_t{200} * 1024, 7), {},
        [&](boost::system::error_code ec) { sent = ec; });
    BOOST_TEST(run_until(f, [&] { return sent.has_value() && a_left == 1 && b_left == 1; }));
    BOOST_TEST(sent == std::optional<boost::system::error_code>(
                           make_error_code(trystero::error::peer_disconnected)));
    BOOST_TEST(a_left == 1);
    BOOST_TEST(b_left == 1);
    BOOST_TEST(!got);
}

// The action wire of a room alone, fed a peer's frames as a data channel hands them over: what it
// sends back, what it delivers, and the peers it drops.
struct wire_bench {
    // A channel whose buffered bytes the case sets; channel_link's destructor is protected.
    struct link final : trystero::detail::channel_link {
        std::vector<trystero::backend::bytes> sent;
        std::size_t buffered = 0;

        bool send(std::span<const std::uint8_t> frame) override {
            sent.emplace_back(frame.begin(), frame.end());
            return true;
        }

        [[nodiscard]] std::size_t buffered_amount() const override { return buffered; }

        void wait_drained(trystero::handler<void(bool)> done) override { done(true); }

        /** Narrow, as a transport that says no SCTP size: the bench's transfers are its cases'. */
        [[nodiscard]] std::size_t max_bulk_frame_bytes() const override {
            return trystero::protocol::max_action_frame_bytes;
        }
    };

    fixture f{true};
    std::shared_ptr<link> channel = std::make_shared<link>();
    /** The actions the room made, and those of them that listen. */
    std::set<std::string, std::less<>> actions{"file"};
    std::set<std::string, std::less<>> listeners{"file"};
    /** Whether dropping a peer clears it from the wire at once, as a room's kick does. */
    bool clear_dropped = false;
    std::vector<trystero::detail::incoming> delivered;
    std::vector<std::string> dropped;
    /** The reasons the wire gave for refusing a message. */
    std::vector<std::string> refused;
    std::shared_ptr<trystero::detail::action_wire> wire;
    // The virtual clock starts with the network's first timer: one made here, so that the clock
    // the wire reads moves with advance() before the wire makes a timer of its own.
    std::shared_ptr<trystero::backend::timer> clock_start;

    /** @param max_receive_bytes The wire's bound, the room's default unless a case lowers it. */
    explicit wire_bench(
        std::size_t max_receive_bytes = trystero::room_options{}.max_receive_bytes) {
        using trystero::detail::action_wire;
        auto* const memory = std::pmr::get_default_resource();
        clock_start = f.net.make_timer(memory);
        wire = std::make_shared<action_wire>(
            f.net, max_receive_bytes,
            action_wire::callbacks{
                .find_link = [this](std::string_view, bool) -> std::optional<action_wire::link> {
                    return action_wire::link{
                        std::weak_ptr<trystero::detail::channel_link>(channel),
                    };
                },
                .peers = [memory](bool) { return std::pmr::vector<std::string_view>(memory); },
                .may_receive = [](std::string_view, std::string_view) { return true; },
                .has_action = [this](std::string_view name) { return actions.contains(name); },
                .has_listener = [this](std::string_view name) { return listeners.contains(name); },
                .deliver =
                    [this](trystero::detail::incoming&& m) { delivered.push_back(std::move(m)); },
                .fail_peer =
                    [this](std::string_view peer, std::string_view reason) {
                        dropped.emplace_back(reason);
                        if (clear_dropped) {
                            wire->clear_peer(peer,
                                             make_error_code(trystero::error::peer_disconnected));
                        }
                    },
                .rejected = [this](std::string_view, std::string_view, std::string_view,
                                   std::string_view reason) { refused.emplace_back(reason); },
            },
            memory);
    }

    /** Hands the wire an action frame of `peer`. */
    void receive(std::string_view peer, trystero::backend::bytes frame) const {
        wire->handle(peer, std::move(frame));
    }

    /** The action frames the wire sent, bare as it writes them. */
    [[nodiscard]] std::vector<trystero::protocol::decoded_action> sent() const {
        std::vector<trystero::protocol::decoded_action> out;
        for (const auto& frame : channel->sent) {
            out.push_back(*trystero::protocol::decode_action(frame).frame);
        }
        return out;
    }
};

// A transfer offered with 0.27.0's wide flag arrives in chunks of 65,522 bytes, which may come in
// any order, each once; a chunk of another length, or one that came already, drops the peer.
void a_wide_transfer_arrives_in_wide_chunks_in_any_order() {
    namespace tp = trystero::protocol;
    trystero::bytes payload(std::size_t{200} * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i * 31);
    }
    wire_bench b;
    b.receive("A", tp::encode_offer("file", tp::payload_format::binary, R"({"n":1})", 5,
                                    payload.size(), tp::chunk_width::wide));
    const auto answers = b.sent();
    if (!BOOST_TEST_EQ(answers.size(), 1U)) {
        return;
    }
    const auto& accepted = std::get<tp::control_message>(answers[0]);
    BOOST_TEST(accepted.kind == tp::frame_kind::accepted);
    BOOST_TEST_EQ(accepted.id, 5U);
    for (const std::size_t index : {3U, 1U, 0U, 2U}) {
        const auto offset = index * tp::wide_chunk_size;
        const auto length = std::min(tp::wide_chunk_size, payload.size() - offset);
        b.receive("A", tp::encode_chunk(5, offset, std::span(payload).subspan(offset, length)));
    }
    BOOST_TEST(b.dropped.empty());
    if (BOOST_TEST_EQ(b.delivered.size(), 1U)) {
        const auto got = b.delivered[0].payload();
        BOOST_TEST_ALL_EQ(got.begin(), got.end(), payload.begin(), payload.end());
        BOOST_TEST_EQ(b.delivered[0].metadata(), R"({"n":1})");
        BOOST_TEST_EQ(b.delivered[0].name(), "file");
    }

    wire_bench narrow;
    narrow.receive("A", tp::encode_offer("file", tp::payload_format::binary, "", 6, payload.size(),
                                         tp::chunk_width::wide));
    narrow.receive("A", tp::encode_chunk(6, 0, std::span(payload).first(tp::chunk_size)));
    if (BOOST_TEST_EQ(narrow.dropped.size(), 1U)) {
        BOOST_TEST_EQ(narrow.dropped[0], "invalid action chunk");
    }

    wire_bench twice;
    twice.receive("A", tp::encode_offer("file", tp::payload_format::binary, "", 7, payload.size()));
    twice.receive("A",
                  tp::encode_chunk(7, tp::chunk_size,
                                   std::span(payload).subspan(tp::chunk_size, tp::chunk_size)));
    BOOST_TEST(twice.dropped.empty());
    twice.receive("A",
                  tp::encode_chunk(7, tp::chunk_size,
                                   std::span(payload).subspan(tp::chunk_size, tp::chunk_size)));
    BOOST_TEST_EQ(twice.dropped.size(), 1U);
    if (!twice.dropped.empty()) {
        BOOST_TEST_EQ(twice.dropped[0], "invalid action chunk");
    }
}

// 0.27.0 keeps at most 1024 transfers pending across peers and 64 of one peer, and rejects the
// next offer as "too many pending transfers"; it keeps at most 32 messages of one action and peer
// until the action listens, a newer one dropping the oldest, for 10 s, and no bound across actions.
void the_wire_keeps_0_27_0s_bounds() {
    namespace tp = trystero::protocol;
    const auto rejections = [](const wire_bench& b) {
        std::size_t n = 0;
        for (const auto& frame : b.sent()) {
            const auto* m = std::get_if<tp::control_message>(&frame);
            n += m != nullptr && m->kind == tp::frame_kind::rejected &&
                         m->reason == "too many pending transfers"
                     ? 1
                     : 0;
        }
        return n;
    };
    {
        wire_bench b;
        b.actions.insert("later");
        for (std::uint32_t id = 0; id <= tp::max_pending_transfers_per_peer; ++id) {
            b.receive("A", tp::encode_offer("later", tp::payload_format::binary, "", id, 20000));
        }
        BOOST_TEST_EQ(rejections(b), 1U);
        for (std::size_t peer = 1;
             peer < tp::max_pending_transfers / tp::max_pending_transfers_per_peer; ++peer) {
            for (std::uint32_t id = 0; id < tp::max_pending_transfers_per_peer; ++id) {
                b.receive("P" + std::to_string(peer),
                          tp::encode_offer("later", tp::payload_format::binary, "", id, 20000));
            }
        }
        BOOST_TEST_EQ(rejections(b), 1U);
        b.receive("Z", tp::encode_offer("later", tp::payload_format::binary, "", 0, 20000));
        BOOST_TEST_EQ(rejections(b), 2U);
        BOOST_TEST(b.dropped.empty());
    }
    {
        wire_bench b;
        b.actions.insert({"later", "other"});
        for (std::size_t i = 0; i <= tp::max_pre_listener_messages; ++i) {
            b.receive("A", tp::encode_inline("later", tp::payload_format::binary, "",
                                             trystero::bytes{static_cast<std::uint8_t>(i)}));
        }
        b.receive("A",
                  tp::encode_inline("other", tp::payload_format::binary, "", trystero::bytes{99}));
        b.listeners.insert("later");
        b.wire->listener_added();
        if (BOOST_TEST_EQ(b.delivered.size(), tp::max_pre_listener_messages)) {
            for (std::size_t i = 0; i < b.delivered.size(); ++i) {
                BOOST_TEST_EQ(b.delivered[i].payload()[0], i + 1);
            }
        }
        trystero::test::advance(b.f, 10'001ms);
        b.listeners.insert("other");
        b.wire->listener_added();
        BOOST_TEST_EQ(b.delivered.size(), tp::max_pre_listener_messages);
    }
    {
        // Each action of a peer has a ring of its own: three full ones keep 96 messages.
        wire_bench b;
        const std::array<std::string, 3> names{"n0", "n1", "n2"};
        for (const auto& name : names) {
            b.actions.insert(name);
            for (std::size_t i = 0; i < tp::max_pre_listener_messages; ++i) {
                b.receive("A", tp::encode_inline(name, tp::payload_format::binary, "",
                                                 trystero::bytes{1}));
            }
        }
        b.listeners.insert(names.begin(), names.end());
        b.wire->listener_added();
        BOOST_TEST_EQ(b.delivered.size(), names.size() * tp::max_pre_listener_messages);
        BOOST_TEST(b.dropped.empty());
    }
}

// As 0.27.0's handleData: an inline frame of an action the room has not made is dropped, and not
// delivered once it is made; an offer of one is rejected at once as an "unregistered action", and
// takes no pending slot.
void frames_of_an_action_not_made_are_dropped_or_rejected() {
    namespace tp = trystero::protocol;
    wire_bench b;
    b.receive("A", tp::encode_inline("late", tp::payload_format::text, "", trystero::bytes{'x'}));
    BOOST_TEST(b.channel->sent.empty());
    b.actions.insert("late");
    b.listeners.insert("late");
    b.wire->listener_added();
    BOOST_TEST(b.delivered.empty());

    wire_bench offered;
    offered.receive("A", tp::encode_offer("late", tp::payload_format::binary, "", 3, 20000));
    const auto answers = offered.sent();
    if (BOOST_TEST_EQ(answers.size(), 1U)) {
        const auto& m = std::get<tp::control_message>(answers[0]);
        BOOST_TEST(m.kind == tp::frame_kind::rejected);
        BOOST_TEST_EQ(m.id, 3U);
        BOOST_TEST_EQ(m.reason, "unregistered action");
    }
    offered.actions.insert("later");
    for (std::uint32_t id = 0; id < tp::max_pending_transfers_per_peer; ++id) {
        offered.receive("A", tp::encode_offer("later", tp::payload_format::binary, "", id, 20000));
    }
    BOOST_TEST_EQ(offered.sent().size(), 1U);
    BOOST_TEST(offered.dropped.empty());
}

// An inline frame whose payload is above `max_receive_bytes` is dropped, reported as "payload
// exceeds receiver size limit", as 0.27.0's payloadLimit applies to it; one at the bound is
// delivered. Nothing is sent back for an inline frame, and the peer is kept.
void an_inline_frame_above_the_receive_bound_is_dropped() {
    namespace tp = trystero::protocol;
    wire_bench b(8);
    b.receive("A", tp::encode_inline("file", tp::payload_format::binary, "",
                                     trystero::bytes(9, std::uint8_t{1})));
    BOOST_TEST(b.delivered.empty());
    if (BOOST_TEST_EQ(b.refused.size(), 1U)) {
        BOOST_TEST_EQ(b.refused.front(), "payload exceeds receiver size limit");
    }
    b.receive("A", tp::encode_inline("file", tp::payload_format::binary, "",
                                     trystero::bytes(8, std::uint8_t{2})));
    BOOST_TEST_EQ(b.delivered.size(), 1U);
    BOOST_TEST_EQ(b.refused.size(), 1U);
    BOOST_TEST(b.channel->sent.empty());
    BOOST_TEST(b.dropped.empty());
}

// A control frame goes out only while the channel buffers at most 1 MiB, as 0.27.0's
// sendControlFrame checks: past it, the peer is dropped. A room drops it at once, from within the
// wire's drain, which goes on to the next peer.
void a_control_frame_into_a_full_channel_drops_the_peer() {
    namespace tp = trystero::protocol;
    wire_bench b;
    b.channel->buffered = tp::max_control_buffered_bytes + 1;
    b.receive("A", tp::encode_offer("file", tp::payload_format::binary, "", 1, 20000));
    BOOST_TEST(b.channel->sent.empty());
    if (BOOST_TEST_EQ(b.dropped.size(), 1U)) {
        BOOST_TEST_EQ(b.dropped[0], "unable to send action control");
    }
    const wire_bench at_bound;
    at_bound.channel->buffered = tp::max_control_buffered_bytes;
    at_bound.receive("A", tp::encode_offer("file", tp::payload_format::binary, "", 1, 20000));
    BOOST_TEST_EQ(at_bound.channel->sent.size(), 1U);
    BOOST_TEST(at_bound.dropped.empty());

    wire_bench cleared;
    cleared.clear_dropped = true;
    cleared.actions.insert("later");
    cleared.receive("A", tp::encode_offer("later", tp::payload_format::binary, "", 1, 20000));
    cleared.receive("B", tp::encode_offer("later", tp::payload_format::binary, "", 1, 20000));
    cleared.channel->buffered = tp::max_control_buffered_bytes + 1;
    cleared.listeners.insert("later");
    cleared.wire->listener_added();
    BOOST_TEST(cleared.channel->sent.empty());
    BOOST_TEST_EQ(cleared.dropped.size(), 2U);
    cleared.channel->buffered = 0;
    cleared.receive("A", tp::encode_offer("later", tp::payload_format::binary, "", 1, 20000));
    BOOST_TEST_EQ(cleared.channel->sent.size(), 1U);  // A's transfer is gone: id 1 is new again
}

void ping_measures_a_round_trip() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("DDDDDDDDDDDDDDDDDDDD"));
    auto a = join(a_app, "ping");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("EEEEEEEEEEEEEEEEEEEE"));
    const auto b = join(b_app, "ping");
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; }));
    std::optional<boost::system::error_code> result;
    a.async_ping(b.self_id(), [&](boost::system::error_code ec, std::chrono::milliseconds rtt) {
        result = ec;
        BOOST_TEST_GE(rtt.count(), 0);
    });
    BOOST_TEST(run_until(f, [&] { return result.has_value(); }));
    BOOST_TEST(!*result);
    std::optional<boost::system::error_code> missing;
    a.async_ping("nobody",
                 [&](boost::system::error_code ec, std::chrono::milliseconds) { missing = ec; });
    BOOST_TEST(run_until(f, [&] { return missing.has_value(); }));
    BOOST_TEST(*missing == trystero::error::no_such_peer);
}

// Peers of another password never meet: 0.27.0 keeps them out by the sealing of their signals
// alone, with no password challenge on the connection, which is never made.
void two_passwords_never_meet() {
    fixture f{true};
    const auto labels = ordered_labels(3, "private");
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(labels[0], "s3cret"));
    auto a = join(a_app, "private");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[1], "s3cret"));
    auto b = join(b_app, "private");
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }));

    // A and B, whose ids are lower, offer to the intruder, which cannot open their offers with its
    // room key, so it never connects. It reports each of them, with its peer.
    std::vector<boost::system::error_code> errors;
    std::set<std::string> refused;
    trystero::test::app intruder_app(f.io.get_executor(), f.net, app_config(labels[2], "guess"));
    auto intruder = join(intruder_app, "private");
    intruder.on_error([&](const trystero::room_error& e) {
        errors.push_back(e.code);
        if (e.code == trystero::error::decrypt_failed) {
            refused.emplace(e.peer_id);
        }
    });
    a.on_error([&](const trystero::room_error& e) { errors.push_back(e.code); });
    BOOST_TEST(run_until(f, [&] { return refused.size() == 2; }, 20'000ms));
    BOOST_TEST(refused ==
               (std::set<std::string>{std::string(a_app.self_id()), std::string(b_app.self_id())}));
    BOOST_TEST(intruder.peers().empty());
    BOOST_TEST_EQ(a.peers().size(), 1U);
    BOOST_TEST(!errors.empty());
    for (const auto& ec : errors) {
        BOOST_TEST(ec == trystero::error::decrypt_failed);
    }
}

/** How many announcements of `peers` a watcher of `room`'s root topic sees. */
struct announcement_watcher {
    announcement_watcher(fixture& f, std::string_view room, std::vector<std::string> peers)
        : socket(f, room), of(std::move(peers)) {
        socket.on_text = [this](std::string_view text) {
            const auto signal = relayed_signal(text);
            if (signal && signal->announcement && std::ranges::find(of, signal->peer) != of.end()) {
                ++seen;
            }
        };
        socket.watch(trystero::protocol::root_topic(app_id, room));
    }

    flooder socket;
    std::vector<std::string> of;
    std::size_t seen = 0;
};

// Two rooms of apps with one password, over a memory backend that holds their derivations, as a
// page's WebCrypto takes seconds: each join returns before its derivation completes, and while
// they are held, for as long as their announcements would have gone out many times, no room
// announces anything. Released, the two derive the same keys and meet.
void password_room_keys_complete_after_the_join_returns() {
    fixture f{true};
    f.net.derive_delay = true;
    const announcement_watcher watcher(f, "keyed", {peer_id_of("keyed-a"), peer_id_of("keyed-b")});
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("keyed-a", "s3cret"));
    auto a = join(a_app, "keyed");
    BOOST_TEST_EQ(f.net.derivations, 1U);
    BOOST_TEST_EQ(f.net.held_derivations(), 1U);
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("keyed-b", "s3cret"));
    auto b = join(b_app, "keyed");
    BOOST_TEST_EQ(f.net.derivations, 2U);
    // Qualified: a std::chrono argument finds std::advance by ADL, which libstdc++ prefers.
    trystero::test::advance(f, 30s);
    BOOST_TEST_EQ(f.net.held_derivations(), 2U);
    BOOST_TEST_EQ(watcher.seen, 0U);
    BOOST_TEST(a.peers().empty());
    BOOST_TEST(b.peers().empty());

    f.net.derive_delay = false;
    f.net.release_derivations();
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }));
    BOOST_TEST_GE(watcher.seen, 1U);
    BOOST_TEST_EQ(f.net.derivations, 2U);
}

// An app derives a room's keys once per room id: two rooms, two derivations; the first room left
// and joined again, none, its keys kept by the app. A room without a password derives none.
void rooms_of_one_password_derive_once_per_room() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net,
                              app_config("0aaaaaaaaaaaaaaaaaaa", "s3cret"));
    {
        auto first = join(a_app, "first");
        const auto second = join(a_app, "second");
        webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
        BOOST_TEST_EQ(f.net.derivations, 2U);
        bool left = false;
        first.async_leave([&](boost::system::error_code ec) { left = !ec; });
        BOOST_TEST(run_until(f, [&] { return left; }));
    }
    const auto again = join(a_app, "first");
    webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
    BOOST_TEST_EQ(f.net.derivations, 2U);

    trystero::test::app open_app(f.io.get_executor(), f.net, app_config("0ccccccccccccccccccc"));
    const auto open = join(open_app, "first");
    webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
    BOOST_TEST_EQ(f.net.derivations, 2U);
}

// A derivation that fails, as WebCrypto's may: the room reports error::backend_failure through
// its error handler and announces nothing; joined again, the room derives again, the failure
// forgotten, and announces.
void a_failed_derivation_is_retried() {
    fixture f{true};
    const std::string a_label = "failing-a";
    announcement_watcher watcher(f, "failing", {peer_id_of(a_label)});
    f.net.fail_derivations = 1;
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(a_label, "s3cret"));
    std::vector<boost::system::error_code> errors;
    {
        auto a = join(a_app, "failing");
        a.on_error([&](const trystero::room_error& e) {
            BOOST_TEST(e.peer_id.empty());
            errors.push_back(e.code);
        });
        trystero::test::advance(f, 30s);
        BOOST_TEST_EQ(f.net.derivations, 1U);
        if (BOOST_TEST_EQ(errors.size(), 1U)) {
            BOOST_TEST(errors.front() == trystero::error::backend_failure);
        }
        BOOST_TEST_EQ(watcher.seen, 0U);
        bool left = false;
        a.async_leave([&](boost::system::error_code ec) { left = !ec; });
        BOOST_TEST(run_until(f, [&] { return left; }));
    }
    const auto again = join(a_app, "failing");
    BOOST_TEST(run_until(f, [&] { return watcher.seen >= 1; }));
    BOOST_TEST_EQ(f.net.derivations, 2U);
    BOOST_TEST_EQ(errors.size(), 1U);
}

// A room left, and its app ended, while the room's derivation is held: once released, the
// derivation completes into nothing, as the room and the app are gone. No announcement leaves, no
// handler of the room runs, and nothing it allocated is touched (the allocation test covers its
// memory; here the room's own resource is checked empty). A
// room of the same id joined again while the derivation is still held completes from it.
void a_room_left_while_its_keys_derive_announces_nothing() {
    counting_resource memory;  // first: the io_context's pending operations free into it
    fixture f{true};
    f.net.derive_delay = true;
    const std::string a_id = "leaving-a";  // the label of A's endpoint key
    announcement_watcher watcher(f, "leaving", {peer_id_of(a_id)});
    std::size_t errors = 0;
    {
        auto c = app_config(a_id, "s3cret");
        c.memory = &memory;
        trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
        {
            auto a = join(a_app, "leaving");
            a.on_error([&errors](const trystero::room_error&) { ++errors; });
            bool left = false;
            a.async_leave([&](boost::system::error_code ec) { left = !ec; });
            BOOST_TEST(run_until(f, [&] { return left; }));
        }
        BOOST_TEST_EQ(f.net.held_derivations(), 1U);
        // Joined again while the first derivation is held: it waits for that one.
        const auto again = join(a_app, "leaving");
        webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
        BOOST_TEST_EQ(f.net.derivations, 1U);
        f.net.derive_delay = false;
        f.net.release_derivations();
        BOOST_TEST(run_until(f, [&] { return watcher.seen >= 1; }));
        BOOST_TEST_EQ(f.net.derivations, 1U);
    }
    // And once more, the app ended while held.
    watcher.seen = 0;
    f.net.derive_delay = true;
    {
        auto c = app_config(a_id, "s3cret");
        c.memory = &memory;
        trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
        auto a = join(a_app, "leaving");
        a.on_error([&errors](const trystero::room_error&) { ++errors; });
        BOOST_TEST_EQ(f.net.held_derivations(), 1U);
    }
    f.net.derive_delay = false;
    f.net.release_derivations();
    trystero::test::advance(f, 30'000ms);
    BOOST_TEST_EQ(f.net.derivations, 2U);
    BOOST_TEST_EQ(watcher.seen, 0U);
    BOOST_TEST_EQ(errors, 0U);
    BOOST_TEST_EQ(memory.in_use, 0U);
}

// The app's cache keeps the keys of 64 rooms, the least recently asked for leaving first, as
// 0.27.0's derivedRoomKeysCache: 65 rooms derive 65 times, the first, asked for again, once more,
// and the last, asked for again, not at all; a room asked for again while it derives waits for
// that derivation. Every key of one room is the same, and two rooms' keys differ.
void the_room_keys_cache_keeps_64_rooms() {
    fixture f{true};
    const auto cache = std::allocate_shared<trystero::detail::room_keys_cache>(
        std::pmr::polymorphic_allocator<>(std::pmr::get_default_resource()), f.io.get_executor(),
        f.net, app_id, "s3cret", std::pmr::get_default_resource());
    std::map<std::string, std::vector<std::array<std::uint8_t, 32>>> keys;
    const auto get = [&](const std::string& room) {
        // The room's list, made now: the completion holds a pointer, which moves without
        // allocating, where a copy of the name would.
        auto* const list = &keys[room];
        cache->get(room,
                   [list](boost::system::error_code ec, const trystero::protocol::room_keys& k) {
                       BOOST_TEST(!ec);
                       std::array<std::uint8_t, 32> aes{};
                       std::ranges::copy(k.aes.bytes(), aes.begin());
                       list->push_back(aes);
                   });
    };
    constexpr std::size_t capacity = trystero::detail::room_keys_cache::capacity;
    BOOST_TEST_EQ(capacity, 64U);
    get("room0");
    get("room0");  // while it derives
    for (std::size_t i = 1; i <= capacity; ++i) {
        get("room" + std::to_string(i));
    }
    webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
    BOOST_TEST_EQ(f.net.derivations, capacity + 1);
    BOOST_TEST_EQ(cache->size(), capacity);
    get(std::string("room") + std::to_string(capacity));
    webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
    BOOST_TEST_EQ(f.net.derivations, capacity + 1);
    get("room0");
    webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
    BOOST_TEST_EQ(f.net.derivations, capacity + 2);
    BOOST_TEST_EQ(cache->size(), capacity);
    BOOST_TEST_EQ(keys["room0"].size(), 3U);
    BOOST_TEST(keys["room0"][0] == keys["room0"][1]);
    BOOST_TEST(keys["room0"][0] == keys["room0"][2]);
    BOOST_TEST(keys["room0"][0] != keys["room1"][0]);
}

void leaving_is_seen_by_the_others_and_a_return_reconnects() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("IIIIIIIIIIIIIIIIIIII"));
    auto a = join(a_app, "leave");
    std::vector<std::string> left;
    std::vector<std::string> joined;
    a.on_peer_leave([&](const trystero::peer_event& e) { left.emplace_back(e.peer_id); });
    a.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });
    {
        // B's app goes with its room, so that its return negotiates again: a living app would keep
        // the connection, idle, and bind the returning room over it.
        trystero::test::app b_app(f.io.get_executor(), f.net, app_config("JJJJJJJJJJJJJJJJJJJJ"));
        auto b = join(b_app, "leave");
        BOOST_TEST(run_until(f, [&] { return joined.size() == 1; }));
        bool done = false;
        b.async_leave([&](boost::system::error_code ec) { done = !ec; });
        BOOST_TEST(run_until(f, [&] { return done && left.size() == 1; }));
        BOOST_TEST(a.peers().empty());
    }
    trystero::test::app b2_app(f.io.get_executor(), f.net, app_config("JJJJJJJJJJJJJJJJJJJJ"));
    const auto b2 = join(b2_app, "leave");
    BOOST_TEST(run_until(f, [&] { return joined.size() == 2; }));
}

// A room moved over a joined one leaves it as destroying it does: the other peer sees it go, and
// its pending operations complete with room_left, while an action of it still holds its state.
void moving_a_room_over_a_joined_one_leaves_it() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("KKKKKKKKKKKKKKKKKKKK"));
    auto a = join(a_app, "move");
    std::vector<std::string> left;
    std::vector<std::string> joined;
    a.on_peer_leave([&](const trystero::peer_event& e) { left.emplace_back(e.peer_id); });
    a.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("LLLLLLLLLLLLLLLLLLLL"));
    auto b = join(b_app, "move");
    auto pending = b.make_action<std::string>("pending");
    std::optional<boost::system::error_code> received;
    pending.async_receive([&](boost::system::error_code ec, const std::string& /*text*/,
                              const trystero::message_info& /*info*/) { received = ec; });
    BOOST_TEST(run_until(f, [&] { return joined.size() == 1; }));

    b = join(b_app, "elsewhere");
    BOOST_TEST(run_until(f, [&] { return left.size() == 1 && received.has_value(); }));
    BOOST_TEST(a.peers().empty());
    if (BOOST_TEST(received.has_value())) {
        BOOST_TEST(*received == trystero::error::room_left);
    }
}

// The URLs WebRTC accepts for an ICE server, and some it refuses.
void ice_urls_are_checked_as_webrtc_checks_them() {
    constexpr std::array<std::string_view, 8> accepted{
        "STUN:stun.example",
        "Turn:host:3478?transport=udp",
        "TURNS:relay.example",
        "stun:stun.l.google.com:19302",
        "stun:127.0.0.1",
        "stuns:host:5349",
        "turn:[::1]:3478?transport=udp",
        "turns:relay.example?transport=tcp",
    };
    for (const auto url : accepted) {
        BOOST_TEST(trystero::detail::ice_url_fault(url).empty());
    }
    constexpr std::array<std::string_view, 12> refused{
        "http://host",
        "stun:",
        "stun:host:",
        "stun:host:0",
        "stun:host:70000",
        "stun:host:x",
        "stun://host",
        "stun:host?transport=udp",
        "turn:host?transport=sctp",
        "stun:[::1",
        "stun:::1",
        "no scheme",
    };
    for (const auto url : refused) {
        BOOST_TEST(!trystero::detail::ice_url_fault(url).empty());
    }
}

// A server WebRTC would refuse is dropped when the app is made, and reported to the app's on_error
// with no peer, after the constructor returns; its room goes on with the usable one, and joins.
void unusable_ice_servers_are_dropped_and_reported() {
    fixture f{true};
    f.net.record_servers = true;
    auto c = app_config("NNNNNNNNNNNNNNNNNNNN");
    const auto server = [](const char* url, const char* username, const char* credential) {
        return trystero::backend::ice_server{
            .urls = {std::pmr::string(url)},
            .username = std::pmr::string(username),
            .credential = std::pmr::string(credential),
        };
    };
    c.ice_servers.push_back(server("stun:127.0.0.1:9", "", ""));
    c.ice_servers.push_back(server("http://127.0.0.1:9", "", ""));  // not stun: or turn:
    c.ice_servers.push_back(server("turn:127.0.0.1:9", "", ""));    // no credentials
    c.ice_servers.push_back(server("turn:127.0.0.1:9", "user", "secret"));
    c.ice_servers.push_back(server("STUN:127.0.0.2:9", "", ""));  // a scheme in capitals
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    auto a = join(a_app, "ice");
    std::vector<std::pair<std::string, boost::system::error_code>> errors;
    a_app.on_error([&](const trystero::room_error& e) {
        BOOST_TEST_EQ(e.app_id, app_id);
        BOOST_TEST(e.room_id.empty());
        errors.emplace_back(e.peer_id, e.code);
    });
    std::vector<std::string> joined;
    a.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("OOOOOOOOOOOOOOOOOOOO"));
    const auto b = join(b_app, "ice");
    BOOST_TEST(run_until(f, [&] { return joined.size() == 1 && errors.size() == 2; }));
    for (const auto& [peer, ec] : errors) {
        BOOST_TEST(peer.empty());
        BOOST_TEST(ec == trystero::error::invalid_configuration);
    }
    for (const auto& url : f.net.server_urls) {
        BOOST_TEST(url != "http://127.0.0.1:9");
    }
    BOOST_TEST(std::ranges::count(f.net.server_urls, "turn:127.0.0.1:9") > 0);
    // The backends are given the scheme in lowercase, as a URL parser writes it.
    BOOST_TEST(std::ranges::count(f.net.server_urls, "stun:127.0.0.2:9") > 0);
}

// A backend that makes no connection, as a browser at its limit of connections, has the
// negotiation with that peer reported to on_error; the next announcement starts another, which
// connects.
void a_refused_peer_connection_is_reported_and_tried_again() {
    fixture f{true};
    f.net.refuse_peer_connections = 1;
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("PPPPPPPPPPPPPPPPPPPP"));
    auto a = join(a_app, "refused");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("QQQQQQQQQQQQQQQQQQQQ"));
    auto b = join(b_app, "refused");
    std::vector<boost::system::error_code> errors;
    const auto on_error = [&](const trystero::room_error& e) { errors.push_back(e.code); };
    a.on_error(on_error);
    b.on_error(on_error);
    std::vector<std::string> joined;
    a.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });
    BOOST_TEST(run_until(f, [&] { return joined.size() == 1; }, 30s));
    if (BOOST_TEST_EQ(errors.size(), 1U)) {
        BOOST_TEST(errors.at(0) == trystero::error::backend_failure);
    }
}

// Announcements are not sealed: anyone who knows the app and the room ids can announce any number
// of fresh ids. A room offers to at most max_unconnected_peers of them at once, its memory and the
// page's connections stay bounded, the peer it is connected to keeps working, and once the offers
// to the flooders expire their negotiations are gone and a new peer joins.
//
// The case runs on the virtual clock, so that it sees the same on every machine: the clock stands
// still while the rooms handle a thousand announcements, and moves a minute before the next. On
// the wall clock, a thousand left several thousand relay texts queued ahead of the message that
// must go through, which a loaded machine took longer to handle than the case could wait, and the
// slower it ran, the more often the rooms' timers fired meanwhile, which changed the connections
// and the memory the case compares.
//
// The memory is bounded by the room's own limits, not by the flood: the negotiations under way,
// the event ids, offers and answers, and refused peers it remembers, each up to a count of its
// own. A flood of more announcements than the room remembers events fills every one of them, and
// a second flood twice as large leaves the room holding no more: compared, not a number of bytes,
// which the standard library's own per-allocation overhead sets (MSVC's debug library more than
// doubles it). Without the bound, what a flood leaves grows with it.
void a_flood_of_announcements_is_bounded() {
    constexpr std::size_t flood = trystero::detail::nostr_strategy::remembered_events + 1000;
    constexpr std::size_t bound = trystero::detail::signaling::max_unconnected_peers;
    // Longer than an offer lasts (57.3 s): a thousand finds the offers to the one before expired.
    constexpr std::chrono::milliseconds minute = 1min;
    counting_resource memory;  // first: the io_context's pending operations free into it
    fixture f{true};
    auto c = app_config("00000000000000000000");  // lower than every flooder: it offers to each
    c.memory = &memory;
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    auto a = join(a_app, "flood");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("BBBBBBBBBBBBBBBBBBBB"));
    auto b = join(b_app, "flood");
    std::vector<std::string> joined;
    a.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });
    auto chat_a = a.make_action<std::string>("chat");
    auto chat_b = b.make_action<std::string>("chat");
    std::vector<std::string> heard;
    chat_b.on_message([&](const std::string& text, const trystero::message_info& /*info*/) {
        heard.push_back(text);
    });
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return joined.size() == 1; })));
    const std::size_t joined_in_use = memory.in_use;
    const std::size_t joined_connections = f.net.live_connections();

    // Announces `count` fresh ids, a thousand a minute, each thousand handled by the relay and the
    // rooms before the clock moves, then has a message go through, and returns what the room holds
    // then, beyond what it held once joined. The memory relay queues what it forwards in each
    // room's resource, and a flood sent at once would hold it all there, more than Emscripten's
    // 16 MiB. The page's connections are counted from those it holds when the flood starts, the
    // late peer's among them in the second. The flood adds at most `bound`, the offers of the
    // negotiations under way: the clock moves only once every queued completion ran, so that the
    // connection of an offer that expired is gone before the next thousand. Without the bound on
    // negotiations, the room fills its window, 64 connections (measured).
    flooder attacker(f, "flood");
    const auto flooded = [&](std::size_t count, std::string_view prefix) {
        const std::size_t held = f.net.live_connections();
        f.net.peak_connections = held;
        constexpr std::size_t batch = 1000;
        for (std::size_t sent = 0; sent < count; sent += batch) {
            if (sent > 0) {
                trystero::test::advance(f, minute);
            }
            const std::size_t now = std::min(batch, count - sent);
            const auto before = f.net.relay(relay_a).events_seen;
            attacker.announce(now, prefix);
            BOOST_TEST(
                run_until(f, [&] { return f.net.relay(relay_a).events_seen >= before + now; }));
        }
        const std::size_t messages = heard.size() + 1;
        chat_a.async_send("after the flood", [](boost::system::error_code) {});
        BOOST_TEST(run_until(f, [&] { return heard.size() == messages; }));
        BOOST_TEST_LE(f.net.peak_connections, held + bound);
        return memory.in_use - joined_in_use;
    };
    const std::size_t first = flooded(flood, "1flood");

    // Once every offer to a flooder expired, its negotiation is gone, and a new peer joins.
    BOOST_TEST(
        run_until(f, [&] { return f.net.live_connections() == joined_connections; }, minute));
    trystero::test::app late_app(f.io.get_executor(), f.net, app_config("CCCCCCCCCCCCCCCCCCCC"));
    const auto late = join(late_app, "flood");
    BOOST_TEST(run_until(f, [&] { return joined.size() == 2; }));
    BOOST_TEST_EQ(heard.size(), 1U);

    // A flood twice as large keeps the room as bounded, and the room working. The half of
    // `first` allows for the late peer, joined since, and for the records not yet full after the
    // first flood; without the bound on negotiations, the second flood leaves 2.6 times as much,
    // and 2.95 times without the one on the event ids remembered (measured).
    const std::size_t second = flooded(2 * flood, "1again");
    BOOST_TEST_EQ(heard.size(), 2U);
    BOOST_TEST_LE(second, first + (first / 2));
}

// A watcher without the room key answers every offer of A to B with garbage, in B's name, before
// B's own answer: A opens an answer before acting on it, so the forged ones change nothing, A and
// B join, and A reports the failed open once for B, at the answer stage, with 0.27.0's message.
void a_forged_answer_changes_nothing() {
    fixture f{true};
    const auto labels = ordered_labels(2, "forged");  // A offers to B
    const std::string a_id = peer_id_of(labels[0]);
    const std::string b_id = peer_id_of(labels[1]);
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(labels[0]));
    auto a = join(a_app, "forged");
    std::vector<std::string> joined;
    a.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });

    struct report {
        boost::system::error_code code;
        std::string message;
        std::string peer_id;
        std::optional<trystero::signal_stage> stage;
    };

    std::vector<report> errors;
    a.on_error([&](const trystero::room_error& e) {
        errors.push_back({
            .code = e.code,
            .message = std::string(e.message),
            .peer_id = std::string(e.peer_id),
            .stage = e.stage,
        });
    });
    flooder attacker(f, "forged");
    const auto a_topic = trystero::protocol::peer_topic(app_id, "forged", a_id);
    std::size_t forged = 0;
    attacker.on_text = [&](std::string_view text) {
        const auto signal = relayed_signal(text);
        if (signal && signal->peer == a_id && !signal->offer.empty()) {
            attacker.publish(
                a_topic,
                {.peer_id = b_id, .offer_id = signal->offer_id, .answer = "not a sealed answer"});
            ++forged;
        }
    };
    attacker.watch(trystero::protocol::peer_topic(app_id, "forged", b_id));
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[1]));
    const auto b = join(b_app, "forged");
    BOOST_TEST(run_until(f, [&] { return joined.size() == 1; }));
    BOOST_TEST_GE(forged, 1U);
    // The forged answers reach A before B's own, so the first one is reported, and only it.
    BOOST_TEST_EQ(errors.size(), 1U);
    for (const auto& e : errors) {
        BOOST_TEST(e.code == trystero::error::decrypt_failed);
        BOOST_TEST_EQ(e.peer_id, b_id);
        BOOST_TEST(e.stage == trystero::signal_stage::answer);
        BOOST_TEST_EQ(e.message,
                      "failed to decrypt answer (incorrect password or corrupted signal)");
    }
}

// Offers from fresh ids that are not sealed with the room key make no connection: a negotiation
// changes only once its offer opened. The case counts the offers the room could not open, each
// logged once, and looks at the connections once it has every one.
void forged_offers_make_no_connection() {
    constexpr std::size_t flood = 200;
    fixture f{true};
    auto c = app_config(label_beside("1", false));  // above the flood's ids, which offer
    std::size_t refused = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refused += text.starts_with("could not open the offer from 0offer") ? 1 : 0;
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    const auto a = join(a_app, "offers");
    flooder attacker(f, "offers");
    const auto a_topic = trystero::protocol::peer_topic(app_id, "offers", a_app.self_id());
    // Twenty at a time, all at one instant, each twenty refused before the next comes. A new id
    // beyond the room's 32 negotiations takes the place of one already refused, which is idle: one
    // whose offer is being opened keeps its place. More than 32 at once would have the later ids
    // refused at the bound, unopened (a_flood_of_offers_at_one_instant_is_bounded).
    const auto instant = f.net.steady_now();
    for (std::size_t i = 0; i < flood; ++i) {
        const std::string id = "0offer" + std::to_string(i);
        attacker.publish(a_topic,
                         {.peer_id = id, .offer_id = "offer", .offer = "not a sealed offer"});
        if (i % 20 == 19 && !BOOST_TEST(run_until(f, [&] { return refused == i + 1; }))) {
            break;
        }
    }
    BOOST_TEST_EQ(refused, flood);
    BOOST_TEST_EQ(f.net.made_connections, 0U);
    BOOST_TEST(f.net.steady_now() == instant);
}

// Forged offers from fresh ids, all at one instant: the room opens each in the call that reads it,
// as 0.27.0 opens a signal, and refuses it for not opening, each logged once. A refused offer
// leaves its negotiation idle, so that the next id takes its place and none is refused at the
// bound. None makes a connection, and a peer who comes next joins.
void a_flood_of_offers_at_one_instant_is_bounded() {
    constexpr std::size_t flood = 200;
    fixture f{true};
    const auto z_label = label_beside("1", false);  // above the flood's ids, which offer
    const auto z_id = peer_id_of(z_label);
    auto c = app_config(z_label);
    std::size_t not_opened = 0;
    std::size_t refused = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        not_opened += text.starts_with("could not open the offer from 0offer") ? 1 : 0;
        refused += text.starts_with("refusing 0offer") ? 1 : 0;
    };
    trystero::test::app z_app(f.io.get_executor(), f.net, std::move(c));
    auto z = join(z_app, "instant");
    std::vector<std::string> joined;
    z.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });
    webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
    flooder attacker(f, "instant");
    const auto z_topic = trystero::protocol::peer_topic(app_id, "instant", z_id);
    const auto instant = f.net.steady_now();
    for (std::size_t i = 0; i < flood; ++i) {
        attacker.publish(z_topic, {
                                      .peer_id = "0offer" + std::to_string(i),
                                      .offer_id = "offer",
                                      .offer = "not a sealed offer",
                                  });
    }
    webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
    BOOST_TEST_EQ(not_opened, flood);
    BOOST_TEST_EQ(refused, 0U);
    BOOST_TEST_EQ(f.net.made_connections, 0U);
    BOOST_TEST(f.net.steady_now() == instant);

    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(label_beside(z_id, true)));
    const auto a = join(a_app, "instant");
    BOOST_TEST(run_until(f, [&] { return joined.size() == 1; }));
}

// Offers in one peer's name, all at one instant: first one sealed with the room key, then 40
// others, each a text of its own and 48 KiB long, and the first again, as an event of its own
// through the other relay. The first opens and is answered at once, and the room then answers
// only the same offer again, as Trystero answers one at a time: the others are ignored unopened,
// and none is kept in the room's resource. The first again has the answer sent through its relay
// too, as Trystero's replay records the relay.
void offers_in_one_name_are_answered_one_at_a_time() {
    constexpr std::size_t offers = 40;
    const std::string padding(std::size_t{48} * 1024, 'x');
    counting_resource memory;  // first: the io_context's pending operations free into it
    fixture f{true};
    const auto z_label = label_beside("1", false);  // above 0name, which offers
    const auto z_id = peer_id_of(z_label);
    auto c = app_config(z_label);
    c.memory = &memory;
    trystero::test::app z_app(f.io.get_executor(), f.net, std::move(c));
    const auto z = join(z_app, "one");
    const auto z_topic = trystero::protocol::peer_topic(app_id, "one", z_id);
    // Two sockets in the name of 0name, one on each relay, which see the room's answers to it.
    std::set<std::string> answered_through;
    flooder on_a(f, "one");
    flooder on_b(f, "one", relay_b);
    for (auto [socket, relay] : {std::pair{&on_a, relay_a}, std::pair{&on_b, relay_b}}) {
        socket->on_text = [&answered_through, relay, &z_id](std::string_view text) {
            const auto signal = relayed_signal(text);
            if (signal && signal->peer == z_id && !signal->answer.empty()) {
                answered_through.emplace(relay);
            }
        };
        socket->watch(trystero::protocol::peer_topic(app_id, "one", "0name"));
    }
    webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
    const std::size_t joined = memory.in_use;
    const auto sealed = flooder::sealed("one", "mem:999", "0name", "first");
    const trystero::protocol::signal_message first{
        .peer_id = "0name",
        .offer_id = "first",
        .offer = sealed,
    };
    on_a.publish(z_topic, first);
    for (std::size_t i = 0; i < offers; ++i) {
        on_a.publish(z_topic, {
                                  .peer_id = "0name",
                                  .offer_id = "o" + std::to_string(i),
                                  .offer = padding + std::to_string(i),
                              });
    }
    on_b.publish(z_topic, first, "again");
    webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
    BOOST_TEST_LT(memory.in_use - joined, padding.size());
    BOOST_TEST_EQ(f.net.made_connections, 1U);
    const std::set<std::string> both{relay_a, relay_b};
    BOOST_TEST_ALL_EQ(answered_through.begin(), answered_through.end(), both.begin(), both.end());
}

// A room member, who holds the key, sends offers from fresh ids whose SDP every connection refuses:
// each ends its negotiation at once and frees its place, but the room makes no more than
// max_connections_per_window connections in the window, and refuses the rest.
void sealed_offers_that_fail_at_once_are_bounded() {
    constexpr std::size_t flood = 500;
    constexpr std::size_t bound = trystero::detail::signaling::max_connections_per_window;
    fixture f{true};
    auto c = app_config(label_beside("2", false));  // above the member's ids, which offer
    std::size_t refusals = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refusals +=
            text.find(" connections were made in the last ") != std::string_view::npos ? 1 : 0;
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    const auto a = join(a_app, "churn");
    flooder member(f, "churn");
    const auto a_topic = trystero::protocol::peer_topic(app_id, "churn", a_app.self_id());
    // One at a time, each once the previous one made its connection or was refused: sent at once,
    // the ids beyond the room's 32 negotiations would be refused at the bound before their offers
    // open, and the window would go untested. Each is a text of its own, as a member makes them:
    // the room refuses an offer it used before.
    for (std::size_t i = 0; i < flood; ++i) {
        const auto offer =
            flooder::sealed("churn", "not an SDP the connection takes " + std::to_string(i),
                            "1churn" + std::to_string(i), "o");
        member.publish(a_topic,
                       {.peer_id = "1churn" + std::to_string(i), .offer_id = "o", .offer = offer});
        if (!BOOST_TEST(run_until(f, [&] { return f.net.made_connections + refusals > i; }))) {
            break;
        }
    }
    BOOST_TEST_LE(f.net.made_connections, bound);
    BOOST_TEST_EQ(f.net.made_connections + refusals, flood);
}

// A watcher without the key announces fresh ids above ours and answers each of our offers to them
// with garbage, in their names: an answer that does not open frees no place, so the room makes a
// connection for at most max_unconnected_peers of them.
void announcements_with_forged_answers_are_bounded() {
    constexpr std::size_t flood = 300;
    constexpr std::size_t bound = trystero::detail::signaling::max_unconnected_peers;
    fixture f{true};
    const auto a_label = label_beside("1", true);  // below the ids it offers to
    const auto a_id = peer_id_of(a_label);
    auto c = app_config(a_label);
    std::size_t refusals = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refusals += text.starts_with("refusing ") ? 1 : 0;
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    const auto a = join(a_app, "churn");
    flooder attacker(f, "churn");
    const auto a_topic = trystero::protocol::peer_topic(app_id, "churn", a_id);
    std::map<std::string, std::string, std::less<>> owners;  // peer topic -> its peer id
    std::size_t forged = 0;
    attacker.on_text = [&](std::string_view text) {
        const auto signal = relayed_signal(text);
        const auto owner = signal ? owners.find(signal->topic) : owners.end();
        if (owner == owners.end()) {
            return;
        }
        if (signal->peer == a_id && !signal->offer.empty()) {
            attacker.publish(a_topic, {
                                          .peer_id = owner->second,
                                          .offer_id = signal->offer_id,
                                          .answer = "not a sealed answer",
                                      });
            ++forged;
        }
    };
    for (std::size_t i = 0; i < flood; ++i) {
        const auto id = "1churn" + std::to_string(i);
        const auto topic = trystero::protocol::peer_topic(app_id, "churn", id);
        attacker.watch(topic);
        owners.emplace(std::string(topic), id);
        attacker.announce_as(id, 1);
    }
    BOOST_TEST(run_until(f, [&] { return forged >= bound && refusals >= flood - bound; }));
    BOOST_TEST_LE(f.net.made_connections, bound);
}

// The replay cache remembers the last texts it was given, as many as its capacity, the oldest
// leaving first.
void the_replay_cache_remembers_the_last_ones() {
    trystero::detail::replay_cache cache(3, std::pmr::get_default_resource());
    BOOST_TEST(!cache.contains("one"));
    for (const char* text : {"one", "two", "three"}) {
        cache.insert(text);
    }
    BOOST_TEST(cache.contains("one") && cache.contains("two") && cache.contains("three"));
    cache.insert("four");
    BOOST_TEST(!cache.contains("one"));
    BOOST_TEST(cache.contains("two") && cache.contains("three") && cache.contains("four"));
    cache.insert("five");
    BOOST_TEST(!cache.contains("two"));
    BOOST_TEST(cache.contains("five"));
    trystero::detail::replay_cache none(0, std::pmr::get_default_resource());
    none.insert("one");
    BOOST_TEST(!none.contains("one"));
}

/** A NIP-01 event id, 64 lowercase hexadecimal digits, all `digit`. */
std::string event_id_of(char digit) {
    std::string id(trystero::detail::recent_event_ids::id_size, digit);
    return id;
}

// A relay may repeat an event, and every relay carries the events of the room: the strategy acts on
// an event id once among the newest it remembers, and on the oldest again once forgotten.
void recent_event_ids_drop_a_repeat_and_forget_the_oldest() {
    counting_resource memory;
    {
        const std::array<std::uint8_t, 16> key{};
        trystero::detail::recent_event_ids ids(3, key, &memory);
        const auto a = event_id_of('a');
        const auto b = event_id_of('b');
        const auto c = event_id_of('c');
        const auto d = event_id_of('d');
        BOOST_TEST(ids.remember(a));
        BOOST_TEST(!ids.remember(a));
        BOOST_TEST(ids.remember(b));
        BOOST_TEST(ids.remember(c));
        BOOST_TEST(!ids.remember(a));
        BOOST_TEST(ids.remember(d));
        BOOST_TEST_EQ(ids.size(), 3U);
        BOOST_TEST(ids.remember(a));
        BOOST_TEST(!ids.remember(c));
        BOOST_TEST(!ids.remember(d));
        BOOST_TEST(ids.remember(b));
        BOOST_TEST_EQ(ids.size(), 3U);
        BOOST_TEST_GT(memory.in_use, 0U);
    }
    BOOST_TEST_EQ(memory.in_use, 0U);
}

// Trystero takes every event a relay sends, repeats included. The port drops a repeat only of an
// id NIP-01 defines, 64 lowercase hexadecimal digits: any other id, of any length, is acted on as
// Trystero acts on it, every time, and never stored, so that a relay cannot fill the room's
// resource with the 4096 ids remembered. What the index holds once made is its own: libstdc++'s
// deque and MSVC's debug containers allocate even when empty.
void event_ids_other_than_nip01_ones_are_never_remembered() {
    counting_resource memory;
    const std::array<std::uint8_t, 16> key{};
    trystero::detail::recent_event_ids ids(3, key, &memory);
    const std::size_t empty = memory.in_use;
    std::string uppercase = event_id_of('a');
    uppercase.back() = 'A';
    std::string not_hex = event_id_of('a');
    not_hex.front() = 'g';
    for (const std::string& id : {
             std::string(),
             std::string(63, 'a'),
             std::string(65, 'a'),
             uppercase,
             not_hex,
             std::string(std::size_t{1} << 20U, 'a'),
         }) {
        BOOST_TEST(ids.remember(id));
        BOOST_TEST(ids.remember(id));
    }
    BOOST_TEST_EQ(ids.size(), 0U);
    BOOST_TEST_EQ(memory.in_use, empty);
}

// The ids are whatever a relay sends, so the index hashes them with SipHash under a key of the
// strategy's: under another key, an id hashes elsewhere, so that ids a relay chose to collide under
// one key do not under the next.
void the_event_id_hash_is_keyed() {
    std::array<std::uint8_t, trystero::detail::recent_event_ids::key_size> one{};
    one.fill(1);
    std::array<std::uint8_t, trystero::detail::recent_event_ids::key_size> two{};
    two.fill(2);
    const trystero::detail::recent_event_ids::keyed_hash under_one(one);
    const trystero::detail::recent_event_ids::keyed_hash under_two(two);
    const auto id = event_id_of('e');
    BOOST_TEST_NE(under_one(id), under_two(id));
    BOOST_TEST_EQ(under_one(id), trystero::detail::recent_event_ids::keyed_hash(one)(id));
}

// A strategy draws its index's key from the backend's randomness when it is made, and nothing
// else: a relay cannot know it, and the room's signer is made at its first event from
// webcpp::crypto's generator. Its index hashes under those 16 bytes, read again from a twin of the
// backend's stream, so that strategies over other streams hash an id elsewhere.
void a_strategy_draws_the_key_of_its_event_ids() {
    constexpr std::size_t key_size = trystero::detail::recent_event_ids::key_size;
    const auto id = event_id_of('e');
    std::vector<std::size_t> hashes;
    for (const std::uint64_t seed : {1U, 2U}) {
        asio::io_context io;
        trystero::test::memory_network net(io, seed);
        const auto strategy = std::make_shared<trystero::detail::nostr_strategy>(
            net, std::span<const std::pmr::string>(), std::pmr::vector<std::pmr::string>(), "topic",
            trystero::detail::nostr_strategy::announcement_handler(),
            trystero::detail::nostr_strategy::message_handler(), trystero::detail::logger(),
            std::pmr::get_default_resource());
        BOOST_TEST_EQ(net.random_bytes, key_size);
        asio::io_context twin_io;
        trystero::test::memory_network twin(twin_io, seed);
        std::array<std::uint8_t, key_size> key{};
        twin.random(key);
        const std::size_t hash = strategy->seen().hash_function()(id);
        BOOST_TEST_EQ(hash, trystero::detail::recent_event_ids::keyed_hash(key)(id));
        hashes.push_back(hash);
    }
    BOOST_TEST_NE(hashes.at(0), hashes.at(1));
}

/** A client of the relay `url` of `f`, once it is open: another user of the relay, to a room. */
std::shared_ptr<trystero::backend::websocket> client_of(fixture& f, std::string_view url) {
    const auto open = std::make_shared<bool>(false);
    auto socket = f.net.connect(url, {.open = [open] { *open = true; }, .message = {}, .close = {}},
                                std::pmr::get_default_resource());
    webcpp::test::require(BOOST_TEST(run_until(f, [&open] { return *open; })));
    return socket;
}

/** Where a client's `["EVENT",{...}]`, as event_message writes it, holds its id. */
std::size_t id_position(std::string_view message) {
    constexpr std::string_view key = R"("id":")";
    return message.find(key) + key.size();
}

// A relay chooses the id it sends with an event, and a strategy acts on an id once. A relay on a
// peer's list sends a forged event under a genuine event's id before an honest relay carries the
// genuine one, as it can for a counter-announcement, whose text holds no nonce. The strategy
// remembers only an id it checked to be its event's own, so it acts on both, as Trystero, which
// remembers none, does: on the forged one each time it comes, and on the genuine one once.
void a_forged_event_under_a_genuine_id_hides_nothing() {
    fixture f(true);
    const std::string_view topic = "a-topic-of-the-room";
    std::vector<std::string> heard;
    const std::array<std::pmr::string, 2> urls{
        std::pmr::string(relay_a),
        std::pmr::string(relay_b),
    };
    std::pmr::vector<std::pmr::string> topics;
    topics.emplace_back(topic);
    const auto strategy = std::make_shared<trystero::detail::nostr_strategy>(
        f.net, urls, std::move(topics), "another-topic",
        trystero::detail::nostr_strategy::announcement_handler(
            [](std::string_view /*nonce*/) { return std::pmr::string("{}"); }),
        trystero::detail::nostr_strategy::message_handler(
            [&heard](std::size_t /*relay*/, std::string_view /*topic*/, std::string_view content) {
                heard.emplace_back(content);
            }),
        trystero::detail::logger(), std::pmr::get_default_resource());
    strategy->start();
    // A relay that saw the strategy's first announcement saw its subscription, sent before it.
    webcpp::test::require(BOOST_TEST(run_until(f, [&f] {
        return f.net.relay(relay_a).events_seen > 0 && f.net.relay(relay_b).events_seen > 0;
    })));
    const auto dishonest = client_of(f, relay_a);
    const auto honest = client_of(f, relay_b);
    std::array<std::uint8_t, 32> secret{};
    secret.fill(5);
    const auto key = trystero::protocol::nostr::signer::from_secret(secret);
    webcpp::test::require(BOOST_TEST(key.has_value()));
    const auto event = [&](std::string_view content) {
        return std::string(trystero::protocol::nostr::event_message(
                               *key, trystero::detail::unix_now(f.net), topic, content)
                               .value());
    };
    const std::string genuine = event("genuine");
    std::string forged = event("forged");
    const std::size_t id_at = id_position(genuine);
    forged.replace(id_position(forged), trystero::detail::recent_event_ids::id_size, genuine, id_at,
                   trystero::detail::recent_event_ids::id_size);
    dishonest->send(forged);
    webcpp::test::require(BOOST_TEST(run_until(f, [&heard] { return heard.size() == 1; })));
    honest->send(genuine);
    BOOST_TEST(run_until(f, [&heard] { return heard.size() == 2; }));
    dishonest->send(genuine);
    dishonest->send(forged);
    dishonest->send(event("later"));
    BOOST_TEST(run_until(f, [&heard] { return !heard.empty() && heard.back() == "later"; }));
    const std::array<std::string_view, 4> expected{"forged", "genuine", "forged", "later"};
    BOOST_TEST_ALL_EQ(heard.begin(), heard.end(), expected.begin(), expected.end());
    strategy->stop();
}

/** The public keys that signed the events on `topic` the relay `url` took, in order. */
std::vector<std::string> signers_of(fixture& f, std::string_view url, std::string_view topic) {
    std::vector<std::string> keys;
    for (const auto& e : f.net.relay(url).published) {
        if (e.topic == topic) {
            keys.emplace_back(e.pubkey);
        }
    }
    return keys;
}

// Each room signs its events with a key of its own, as 0.27.0 keeps a signer per room: the
// announcements of one room carry one key on every relay, another room of the same app another
// key, and a room joined again after it left, a new one. A rejoin makes a strategy of its own, so
// this does not tell a key made at the first event and dropped on stop from one made with the
// strategy: signaling_test's the_room_key_is_made_at_the_first_event_and_dropped_on_stop does.
void each_room_signs_with_its_own_key() {
    fixture f(true);
    f.net.relay(relay_a).record_published = true;
    f.net.relay(relay_b).record_published = true;
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("SSSSSSSSSSSSSSSSSSSS"));
    const auto one_topic = trystero::protocol::root_topic(app_id, "one");
    const auto two_topic = trystero::protocol::root_topic(app_id, "two");
    const auto announced_twice = [&f](std::string_view topic) {
        return [&f, topic] {
            return signers_of(f, relay_a, topic).size() >= 2 &&
                   signers_of(f, relay_b, topic).size() >= 2;
        };
    };
    std::set<std::string> keys;
    {
        auto one = join(a_app, "one");
        const auto two = join(a_app, "two");
        webcpp::test::require(BOOST_TEST(run_until(f, announced_twice(one_topic))));
        webcpp::test::require(BOOST_TEST(run_until(f, announced_twice(two_topic))));
        for (const auto& topic : {one_topic, two_topic}) {
            std::set<std::string> room_keys;
            for (const auto* const url : {relay_a, relay_b}) {
                const auto signers = signers_of(f, url, topic);
                room_keys.insert(signers.begin(), signers.end());
            }
            BOOST_TEST_EQ(room_keys.size(), 1U);
            keys.insert(room_keys.begin(), room_keys.end());
        }
        BOOST_TEST_EQ(keys.size(), 2U);
        bool left = false;
        one.async_leave([&left](boost::system::error_code ec) { left = !ec; });
        webcpp::test::require(BOOST_TEST(run_until(f, [&left] { return left; })));
    }
    f.net.relay(relay_a).published.clear();
    f.net.relay(relay_b).published.clear();
    const auto again = join(a_app, "one");
    webcpp::test::require(BOOST_TEST(run_until(f, announced_twice(one_topic))));
    const auto signers = signers_of(f, relay_a, one_topic);
    BOOST_TEST(!keys.contains(signers.front()));
    BOOST_TEST(
        std::ranges::all_of(signers, [&](const std::string& k) { return k == signers.front(); }));
}

// A room subscribes its own topic before its root topic, as Trystero's rooms do: its REQ lists
// them in that order, and their kinds in the same order, as 0.27.0's batcher writes them.
void a_room_subscribes_its_self_topic_before_its_root_topic() {
    fixture f(true);
    auto& relay = f.net.relay(relay_a);
    relay.record_requests = true;
    auto c = app_config("TTTTTTTTTTTTTTTTTTTT");
    c.relay_urls = {relay_a};
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    const auto r = join(a_app, "ordered");
    webcpp::test::require(BOOST_TEST(run_until(f, [&relay] { return !relay.requests.empty(); })));
    const auto request = webcpp::document::parse(relay.requests.front());
    webcpp::test::require(BOOST_TEST(request.has_value()));
    const auto filter = request->root()[2];
    const std::array<std::string, 2> topics{
        std::string(trystero::protocol::peer_topic(app_id, "ordered", a_app.self_id())),
        std::string(trystero::protocol::root_topic(app_id, "ordered")),
    };
    std::vector<std::string> listed;
    for (const auto t : filter["#x"]) {
        listed.emplace_back(t.value_or(std::string_view()));
    }
    BOOST_TEST_ALL_EQ(listed.begin(), listed.end(), topics.begin(), topics.end());
    std::vector<std::int64_t> kinds;
    for (const auto k : filter["kinds"]) {
        kinds.push_back(k.value_or(std::int64_t{-1}));
    }
    std::vector<std::int64_t> expected;
    for (const auto& t : topics) {
        if (const std::int64_t k = trystero::protocol::nostr::topic_kind(t);
            std::ranges::find(expected, k) == expected.end()) {
            expected.push_back(k);
        }
    }
    BOOST_TEST_ALL_EQ(kinds.begin(), kinds.end(), expected.begin(), expected.end());
}

// A boost::circular_buffer moves by swapping its allocators, which std::pmr's cannot: the classes
// that hold one over the room's resource say they cannot move, rather than fail to compile when
// moved.
void classes_over_a_pmr_circular_buffer_do_not_move() {
    BOOST_TEST(!std::is_move_constructible_v<trystero::detail::construction_window>);
    BOOST_TEST(!std::is_move_assignable_v<trystero::detail::construction_window>);
    BOOST_TEST(!std::is_move_constructible_v<trystero::detail::replay_cache>);
    BOOST_TEST(!std::is_move_assignable_v<trystero::detail::replay_cache>);
}

/** Joins B to A, records what `record` keeps of the relays meanwhile, and has B leave; B's app goes
 * with it, so that B's return negotiates again. */
void join_and_leave(fixture& f, std::string_view b_id, std::string_view room_id,
                    const std::vector<std::string>& joined, const std::vector<std::string>& left,
                    const std::function<bool()>& recorded) {
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(b_id));
    auto b = join(b_app, room_id);
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return joined.size() == 1 && recorded(); })));
    bool done = false;
    b.async_leave([&](boost::system::error_code) { done = true; });
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return done && left.size() == 1; })));
}

// A watcher without the room key records B's sealed answer to A. Once B has left and come back with
// the same id, as a Trystero JS page does after a network drop, it sends that answer in B's name
// with each of A's new offers to B, before B's own answer. The answer was sealed for its own offer
// id, which its AAD binds (0.27.0's `buildSignalAad`), so it does not open: A counts it as a signal
// that did not open, changes nothing else, and B joins again.
//
// The watcher is armed as A sees B leave: an announcement B made before leaving may still reach A
// after it, and A's offer to that announcement is the one the returning B answers, on a relay
// where A does not send it again.
void a_replayed_answer_does_not_open() {
    fixture f{true};
    const auto labels = ordered_labels(2, "replay");  // the apps, by ascending peer id
    const std::string a_id = peer_id_of(labels[0]);
    const std::string b_id = peer_id_of(labels[1]);
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(labels[0]));
    auto a = join(a_app, "replay");
    std::vector<std::string> joined;
    std::vector<std::string> left;
    std::vector<boost::system::error_code> errors;
    bool armed = false;
    a.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });
    a.on_peer_leave([&](const trystero::peer_event& e) {
        left.emplace_back(e.peer_id);
        armed = true;
    });
    a.on_error([&](const trystero::room_error& e) { errors.push_back(e.code); });
    flooder watcher(f, "replay");
    const auto a_topic = std::string(trystero::protocol::peer_topic(app_id, "replay", a_id));
    const auto b_topic = std::string(trystero::protocol::peer_topic(app_id, "replay", b_id));
    std::string recorded;
    std::size_t replays = 0;
    watcher.on_text = [&](std::string_view text) {
        const auto s = relayed_signal(text);
        if (!s) {
            return;
        }
        if (s->topic == a_topic && s->peer == b_id && !s->answer.empty() && recorded.empty()) {
            recorded = s->answer;
        }
        if (armed && s->topic == b_topic && s->peer == a_id && !s->offer.empty()) {
            watcher.publish(a_topic,
                            {.peer_id = b_id, .offer_id = s->offer_id, .answer = recorded});
            ++replays;
        }
    };
    watcher.watch(a_topic);
    watcher.watch(b_topic);
    join_and_leave(f, labels[1], "replay", joined, left, [&] { return !recorded.empty(); });
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[1]));
    const auto b = join(b_app, "replay");
    BOOST_TEST(run_until(f, [&] { return joined.size() == 2; }));
    BOOST_TEST_GE(replays, 1U);
    BOOST_TEST(std::ranges::any_of(errors, [](boost::system::error_code ec) {
        return ec == trystero::error::decrypt_failed;
    }));
}

// The same for an offer: B is lower and offers. The watcher records B's sealed offer to A and its
// offer id, and once B is back, sends both in B's name every 0.5 s of the virtual clock. Its AAD
// is B's and its id's, so it opens with the room key, but A answered that offer before: A refuses
// each replay, and B's own offer joins them.
void a_replayed_offer_is_refused() {
    fixture f{true};
    const auto labels = ordered_labels(2, "replayo");  // the apps, by ascending peer id
    const std::string a_id = peer_id_of(labels[1]);
    const std::string b_id = peer_id_of(labels[0]);
    auto c = app_config(labels[1]);
    std::size_t refused = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refused += text.starts_with("refused the offer from " + b_id) ? 1 : 0;
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    auto a = join(a_app, "replayo");
    std::vector<std::string> joined;
    std::vector<std::string> left;
    a.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });
    a.on_peer_leave([&](const trystero::peer_event& e) { left.emplace_back(e.peer_id); });
    flooder watcher(f, "replayo");
    const auto a_topic = std::string(trystero::protocol::peer_topic(app_id, "replayo", a_id));
    std::string recorded;
    std::string recorded_id;
    watcher.on_text = [&](std::string_view text) {
        const auto s = relayed_signal(text);
        if (s && s->topic == a_topic && s->peer == b_id && !s->offer.empty() && recorded.empty()) {
            recorded = s->offer;
            recorded_id = s->offer_id;
        }
    };
    watcher.watch(a_topic);
    join_and_leave(f, labels[0], "replayo", joined, left, [&] { return !recorded.empty(); });
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[0]));
    const auto b = join(b_app, "replayo");
    std::size_t replays = 0;
    const auto end = f.net.steady_now() + 10s;
    while (joined.size() < 2 && f.net.steady_now() < end) {
        watcher.publish(a_topic, {.peer_id = b_id, .offer_id = recorded_id, .offer = recorded},
                        std::to_string(replays++));
        trystero::test::advance(f, 500ms);
    }
    BOOST_TEST_EQ(joined.size(), 2U);
    BOOST_TEST_GE(replays, 1U);
    BOOST_TEST_GE(refused, 1U);
}

// A watcher without the room key sends A's own sealed offer to B back to A, at once, as B's answer
// to it. Sealed as A's offer, which its AAD binds, it does not open as B's answer: A changes
// nothing, and B's own answer joins them.
void a_reflected_offer_is_refused_as_an_answer() {
    fixture f{true};
    const auto labels = ordered_labels(2, "reflect");  // the apps, by ascending peer id
    const std::string a_id = peer_id_of(labels[0]);
    const std::string b_id = peer_id_of(labels[1]);
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(labels[0]));
    auto a = join(a_app, "reflect");
    std::vector<std::string> joined;
    a.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });
    flooder watcher(f, "reflect");
    const auto a_topic = std::string(trystero::protocol::peer_topic(app_id, "reflect", a_id));
    const auto b_topic = std::string(trystero::protocol::peer_topic(app_id, "reflect", b_id));
    std::size_t reflected = 0;
    watcher.on_text = [&](std::string_view text) {
        const auto s = relayed_signal(text);
        if (s && s->topic == b_topic && s->peer == a_id && !s->offer.empty()) {
            watcher.publish(a_topic,
                            {.peer_id = b_id, .offer_id = s->offer_id, .answer = s->offer});
            ++reflected;
        }
    };
    watcher.watch(b_topic);
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[1]));
    const auto b = join(b_app, "reflect");
    BOOST_TEST(run_until(f, [&] { return joined.size() == 1; }));
    BOOST_TEST_GE(reflected, 1U);
}

// A watcher without the room key announces a decoy id above A, records A's sealed offer to it, and
// sends that offer to A in B's name whenever B announces itself, before B's own offer. Sealed as
// A's, which its AAD binds, it does not open as B's: A never answers one in B's name but B's own,
// and B's own offer joins them.
void a_reflected_offer_is_refused_as_an_offer() {
    fixture f{true};
    const auto labels = ordered_labels(2, "reflecto");  // the apps, by ascending peer id
    const std::string a_id = peer_id_of(labels[1]);
    const std::string b_id = peer_id_of(labels[0]);
    const std::string decoy = "zzzzzzzzzzzzzzzzzzzz";
    auto c = app_config(labels[1]);
    std::size_t answered_reflections = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        answered_reflections += text.starts_with("offer reflected") ? 1 : 0;
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    auto a = join(a_app, "reflecto");
    std::vector<std::string> joined;
    a.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });
    flooder watcher(f, "reflecto");
    const auto root = std::string(trystero::protocol::root_topic(app_id, "reflecto"));
    const auto a_topic = std::string(trystero::protocol::peer_topic(app_id, "reflecto", a_id));
    const auto decoy_topic = std::string(trystero::protocol::peer_topic(app_id, "reflecto", decoy));
    std::string recorded;
    std::size_t reflected = 0;
    watcher.on_text = [&](std::string_view text) {
        const auto s = relayed_signal(text);
        if (!s) {
            return;
        }
        if (s->topic == decoy_topic && s->peer == a_id && !s->offer.empty() && recorded.empty()) {
            recorded = s->offer;
        }
        if (!recorded.empty() && s->topic == root && s->peer == b_id && s->announcement) {
            watcher.publish(a_topic, {
                                         .peer_id = b_id,
                                         .offer_id = "reflected" + std::to_string(reflected++),
                                         .offer = recorded,
                                     });
        }
    };
    watcher.watch(root);
    watcher.watch(decoy_topic);
    watcher.announce_as(decoy, 1);
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return !recorded.empty(); })));
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[0]));
    const auto b = join(b_app, "reflecto");
    BOOST_TEST(run_until(f, [&] { return joined.size() == 1; }));
    BOOST_TEST_GE(reflected, 1U);
    BOOST_TEST_EQ(answered_reflections, 0U);
}

// A answers B (B's id is lower), then offers to C (C's id is higher). A watcher without the room
// key sends A's own sealed answer to B back to A as C's answer to that offer. Sealed as A's answer
// to B's offer, which its AAD binds, it does not open as C's: A changes nothing, and C's own answer
// joins them.
void a_reflected_answer_does_not_open_as_an_answer() {
    fixture f{true};
    const auto labels = ordered_labels(3, "reflans");  // the apps, by ascending peer id
    const std::string a_id = peer_id_of(labels[1]);
    const std::string b_id = peer_id_of(labels[0]);
    const std::string c_id = peer_id_of(labels[2]);
    auto c = app_config(labels[1]);
    bool refused = false;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refused = refused || text.starts_with("could not open the answer from " + c_id);
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    auto a = join(a_app, "reflans");
    std::vector<std::string> joined;
    a.on_peer_join([&](const trystero::peer_event& e) { joined.emplace_back(e.peer_id); });
    flooder watcher(f, "reflans");
    const auto a_topic = std::string(trystero::protocol::peer_topic(app_id, "reflans", a_id));
    const auto b_topic = std::string(trystero::protocol::peer_topic(app_id, "reflans", b_id));
    const auto c_topic = std::string(trystero::protocol::peer_topic(app_id, "reflans", c_id));
    std::string a_answer;
    std::size_t reflected = 0;
    watcher.on_text = [&](std::string_view text) {
        const auto s = relayed_signal(text);
        if (!s) {
            return;
        }
        if (s->topic == b_topic && s->peer == a_id && !s->answer.empty() && a_answer.empty()) {
            a_answer = s->answer;
        }
        if (s->topic == c_topic && s->peer == a_id && !s->offer.empty() && !a_answer.empty()) {
            watcher.publish(a_topic,
                            {.peer_id = c_id, .offer_id = s->offer_id, .answer = a_answer});
            ++reflected;
        }
    };
    watcher.watch(b_topic);
    watcher.watch(c_topic);
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[0]));
    const auto b = join(b_app, "reflans");
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return joined.size() == 1 && !a_answer.empty(); })));
    trystero::test::app peer_c_app(f.io.get_executor(), f.net, app_config(labels[2]));
    const auto peer_c = join(peer_c_app, "reflans");
    BOOST_TEST(run_until(f, [&] { return joined.size() == 2; }));
    BOOST_TEST_GE(reflected, 1U);
    BOOST_TEST(refused);
}

// A relay that closes each connection right after its subscription, as one does after a text
// longer than a room reads, is tried again within a bound that doubles every time, to 60 s; the
// bound comes back to 3.3 s only after a connection that stayed open for 60 s. On the virtual
// clock the close comes at the instant of the open: on a wall clock 100 times faster, the process
// paused for 0.6 s between the two made the connection one that had lasted 60 s.
void a_relay_that_closes_at_once_is_tried_less_and_less_often() {
    fixture f{true};
    auto& relay = f.net.relay(relay_a);
    relay.close_after_subscribe = true;
    auto c = app_config("AAAAAAAAAAAAAAAAAAAA");
    c.relay_urls = {relay_a};
    std::vector<std::int64_t> bounds;
    std::size_t reconnections = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        constexpr std::string_view within = ", next attempt within ";
        if (const auto at = text.find(within); at != std::string_view::npos) {
            bounds.push_back(std::stoll(std::string(text.substr(at + within.size()))));
        }
        reconnections += text.starts_with("reconnected to ") ? 1 : 0;
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    const auto a = join(a_app, "backoff");
    BOOST_TEST(run_until(f, [&] { return bounds.size() >= 6; }, 2min));
    const std::vector<std::int64_t> growing{3333, 6666, 13332, 26664, 53328, 60000};
    BOOST_TEST_ALL_EQ(bounds.begin(),
                      bounds.begin() + std::min<std::ptrdiff_t>(6, std::ssize(bounds)),
                      growing.begin(), growing.end());

    // A connection that lasts: once it has been open for 60 s, the relay goes away.
    relay.close_after_subscribe = false;
    const std::size_t before = reconnections;
    BOOST_TEST(run_until(f, [&] { return reconnections > before; }, 1min));
    const auto opened = f.net.steady_now();
    BOOST_TEST(run_until(f, [&] { return f.net.steady_now() - opened >= 60s; }, 2min));
    const auto lost = bounds.size();
    relay.close_all();
    BOOST_TEST(run_until(f, [&] { return bounds.size() > lost; }));
    if (bounds.size() > lost) {
        BOOST_TEST_EQ(bounds[lost], 3333);
    }
}

// A flood of fresh ids below ours asks the room for a counter-announcement each: the room sends
// at most one per relay in 5.3 s, Trystero's announce interval, so that no relay rate-limits it.
// On the virtual clock the interval is checked at its end: a flood at once asks once, an id 1 ms
// before the interval ends asks nothing, and one as it ends asks again.
void counter_announcements_are_paced() {
    constexpr std::size_t flood = 300;
    fixture f{true};
    auto c = app_config("zzzzzzzzzzzzzzzzzzzz");
    std::size_t asked = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        asked += text.ends_with("asking them to offer") ? 1 : 0;
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    const auto a = join(a_app, "counter");
    flooder attacker(f, "counter");
    // Announces `count` fresh ids, and returns once the relay took them and the room handled them,
    // the clock still.
    const auto announced = [&](std::size_t count, std::string_view prefix) {
        const auto before = f.net.relay(relay_a).events_seen;
        attacker.announce(count, prefix);
        BOOST_TEST(
            run_until(f, [&] { return f.net.relay(relay_a).events_seen >= before + count; }));
    };
    announced(flood, "0low");
    BOOST_TEST_EQ(asked, 1U);
    trystero::test::advance(f, 5332ms);
    announced(1, "0early");
    BOOST_TEST_EQ(asked, 1U);
    trystero::test::advance(f, 1ms);
    announced(1, "0due");
    BOOST_TEST_EQ(asked, 2U);
}

// Beyond the bound, a new peer is refused, and one refused again and again is logged once: the
// offers to the first ids hold their places (the clock runs at its own pace here).
void a_refused_peer_is_logged_once() {
    fixture f{true};
    auto c = app_config(label_beside("1", true));  // below the announced ids, to which it offers
    std::size_t refusals_of_one = 0;
    std::size_t refusals = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refusals += text.starts_with("refusing ") ? 1 : 0;
        refusals_of_one += text.starts_with("refusing 1same") ? 1 : 0;
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(c));
    const auto a = join(a_app, "refusal");
    flooder attacker(f, "refusal");
    const auto before = f.net.relay(relay_a).events_seen;
    const std::size_t bound = trystero::detail::signaling::max_unconnected_peers;
    attacker.announce(bound + 8, "1fill");
    attacker.announce_as("1same", 5);
    BOOST_TEST(
        run_until(f, [&] { return f.net.relay(relay_a).events_seen >= before + bound + 8 + 5; }));
    BOOST_TEST(run_until(f, [&] { return refusals == 9; }));
    BOOST_TEST_EQ(refusals_of_one, 1U);
    BOOST_TEST_LE(f.net.live_connections(), 2 * bound);
}

// A peer id longer than protocol::max_peer_id_bytes is ignored in a signal; an app's own, drawn,
// is 20 characters, in its rooms too.
void long_peer_ids_are_bounded() {
    const std::string longest(trystero::protocol::max_peer_id_bytes, 'x');
    const std::string too_long(trystero::protocol::max_peer_id_bytes + 1, 'x');
    // {"peerId":<id>}, read as a room reads a signal.
    const auto signal = [](std::string_view id) {
        std::pmr::string text("{");
        trystero::protocol::json::append_key(text, "peerId");
        webcpp::test::require(BOOST_TEST(trystero::protocol::json::append_string(text, id)));
        text += '}';
        auto parsed = webcpp::document::parse(
            text,
            trystero::protocol::json::protocol_limits(trystero::protocol::max_parsed_signal_bytes),
            std::pmr::polymorphic_allocator<std::byte>(std::pmr::get_default_resource()));
        webcpp::test::require(BOOST_TEST(parsed.has_value()));
        return std::move(*parsed);
    };
    const auto longest_signal = signal(longest);
    const auto too_long_signal = signal(too_long);
    BOOST_TEST(trystero::protocol::read_signal(longest_signal.root()).has_value());
    BOOST_TEST(!trystero::protocol::read_signal(too_long_signal.root()).has_value());

    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(""));
    const auto a = join(a_app, "long");
    BOOST_TEST_EQ(a.self_id().size(), trystero::protocol::peer_id_size);
    BOOST_TEST_EQ(a.self_id(), a_app.self_id());
}

void three_peers_form_a_mesh() {
    fixture f{true};
    std::vector<trystero::test::app> apps;
    std::vector<trystero::room> rooms;
    for (const char* id :
         {"KKKKKKKKKKKKKKKKKKKK", "LLLLLLLLLLLLLLLLLLLL", "MMMMMMMMMMMMMMMMMMMM"}) {
        apps.emplace_back(f.io.get_executor(), f.net, app_config(id));
        rooms.push_back(join(apps.back(), "mesh"));
    }
    BOOST_TEST(run_until(f, [&] {
        return std::ranges::all_of(rooms,
                                   [](const trystero::room& r) { return r.peers().size() == 2; });
    }));
    // A broadcast reaches both others.
    auto shout = rooms[0].make_action<std::string>("shout");
    std::size_t heard = 0;
    for (std::size_t i = 1; i < rooms.size(); ++i) {
        rooms[i].make_action<std::string>("shout").on_message(
            [&](const std::string& text, const trystero::message_info& /*info*/) {
                heard += text == "all" ? 1 : 0;
            });
    }
    shout.async_send("all", [](boost::system::error_code ec) { BOOST_TEST(!ec); });
    BOOST_TEST(run_until(f, [&] { return heard == 2; }));
}

// A room keeps one connection per peer, as 0.27.0's connectPeer keeps a connected peer that is not
// stale: a second connection that opens while the peer's link is live is closed, and the peer
// stays; once the link is no longer live, the next connection replaces it, the peer leaving first.
void a_room_keeps_one_connection_per_peer() {
    counting_resource a_memory;  // first: A's connections are made in it, which names A's ends
    fixture f{true};
    const auto labels = ordered_labels(2, "one");  // A's id below B's: A offers
    const std::string b_id = peer_id_of(labels[1]);
    const auto a = make_app_room(f, labels[0], "one", &a_memory);
    std::size_t a_leaves = 0;
    a.room->on_peer_leave(a.room->bind<void(const trystero::peer_event&)>(
                              [&](const trystero::peer_event&) { ++a_leaves; }),
                          {});
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[1]));
    auto b = join(b_app, "one");
    std::size_t b_leaves = 0;
    b.on_peer_leave([&](const trystero::peer_event&) { ++b_leaves; });
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return a.room->peers().size() == 1 && b.peers().size() == 1; })));
    // The connection the room uses: the one open pair left once the negotiations settled.
    const auto live_before = f.net.live_connections();
    {
        auto second = negotiate(f);
        a.room->on_connected(b_id, second.local_pc, second.local, second.offer, second.answer);
        BOOST_TEST(!second.local->is_open());
        BOOST_TEST(run_until(f, [&] { return !second.remote->is_open(); }));
        trystero::test::advance(f, 1000ms);
        BOOST_TEST_EQ(a.room->peers().size(), 1U);
        BOOST_TEST_EQ(b.peers().size(), 1U);
        BOOST_TEST_EQ(a_leaves, 0U);
        BOOST_TEST_EQ(b_leaves, 0U);
        BOOST_TEST(second.received->empty());
    }
    BOOST_TEST(run_until(f, [&] { return f.net.live_connections() == live_before; }));
    // The live link's channel closes at A's end; its close is still to be reported when the next
    // connection opens, which replaces it.
    std::shared_ptr<trystero::backend::data_channel> a_end;
    for (std::size_t id = 0; id < f.net.made_connections && !a_end; ++id) {
        const auto pc = f.net.find(id);
        const auto channel = pc ? pc->channel() : nullptr;
        const auto remote = channel ? channel->remote() : nullptr;
        // A's end of the pair the rooms use: in A's resource, both ends open.
        if (channel && channel->is_open() && remote && remote->is_open() &&
            channel->memory() == &a_memory) {
            a_end = channel;
        }
    }
    auto third = negotiate(f);
    webcpp::test::require(BOOST_TEST(a_end != nullptr));
    a_end->close();
    a.room->on_connected(b_id, third.local_pc, third.local, third.offer, third.answer);
    BOOST_TEST(third.local->is_open());
    BOOST_TEST(run_until(f, [&] { return a_leaves == 1; }));
    // The new link carries the room's handshake: A, whose id is lower, sends its step 1.
    BOOST_TEST(run_until(f, [&] { return !third.received->empty(); }));
    if (!third.received->empty()) {
        BOOST_TEST_EQ(static_cast<int>(third.received->front().front()),
                      static_cast<int>(trystero::protocol::action_wire_version));
    }
    a.room->abandon();
}

// A room that starts to leave negotiates nothing more, as 0.27.0's signal handler drops every
// signal while its room is leaving: a peer that announces itself during the farewell, with an id
// above the room's own, gets no offer, and no connection is made for it.
void a_leaving_room_negotiates_nothing() {
    fixture f{true};
    const auto a_label = label_beside("y", true);  // below the newcomers' ids, to which A offers
    const std::string a_id = peer_id_of(a_label);
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(a_label));
    auto a = join(a_app, "farewell");
    flooder peer(f, "farewell");
    std::size_t offers = 0;
    peer.on_text = [&](std::string_view text) {
        if (const auto s = relayed_signal(text); s && s->peer == a_id && !s->offer.empty()) {
            ++offers;
        }
    };
    const std::string newcomer = "zzzzzzzzzzzzzzzzzzzz";
    peer.watch(trystero::protocol::peer_topic(app_id, "farewell", newcomer));
    // A answers a newcomer while joined: the case would see its offer.
    peer.announce_as(newcomer, 1);
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return offers == 1; })));
    const auto made = f.net.made_connections;
    bool gone = false;
    peer.watch(trystero::protocol::peer_topic(app_id, "farewell", "yyyyyyyyyyyyyyyyyyyy"));
    webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
    a.async_leave([&](boost::system::error_code) { gone = true; });
    peer.announce_as("yyyyyyyyyyyyyyyyyyyy", 1);
    BOOST_TEST(run_until(f, [&] { return gone; }));
    BOOST_TEST_EQ(offers, 1U);
    BOOST_TEST_EQ(f.net.made_connections, made);
}

/** Two apps, A and B, whose rooms "r1" and "r2" have met: each room sees the other app's peer. */
struct two_rooms_each {
    fixture f{true};
    trystero::test::app a_app{f.io.get_executor(), f.net, app_config("AAAAAAAAAAAAAAAAAAAA")};
    trystero::test::app b_app{f.io.get_executor(), f.net, app_config("BBBBBBBBBBBBBBBBBBBB")};
    std::optional<trystero::room> a1;
    std::optional<trystero::room> a2;
    std::optional<trystero::room> b1;
    std::optional<trystero::room> b2;

    two_rooms_each() {
        a1.emplace(join(a_app, "r1"));
        a2.emplace(join(a_app, "r2"));
        b1.emplace(join(b_app, "r1"));
        b2.emplace(join(b_app, "r2"));
        webcpp::test::require(BOOST_TEST(run_until(f, [&] {
            return a1->peers().size() == 1 && a2->peers().size() == 1 && b1->peers().size() == 1 &&
                   b2->peers().size() == 1;
        })));
    }
};

// Each room of an app has its own connection to each peer, as 0.27.0's rooms do: two apps that
// share two rooms hold two connections, one per room, both ends of each open.
void rooms_of_one_app_have_their_own_connections() {
    two_rooms_each t;
    // The negotiations' losing connections close; the two connections the rooms use stay.
    BOOST_TEST(run_until(t.f, [&] { return t.f.net.live_connections() == 4; }));
    // Each room's messages travel on its own connection.
    std::vector<std::string> heard;
    t.b1->make_action<std::string>("say").on_message(
        [&](const std::string& text, const trystero::message_info&) {
            heard.push_back("1" + text);
        });
    t.b2->make_action<std::string>("say").on_message(
        [&](const std::string& text, const trystero::message_info&) {
            heard.push_back("2" + text);
        });
    t.a1->make_action<std::string>("say").async_send(
        "a", [](boost::system::error_code ec) { BOOST_TEST(!ec); });
    t.a2->make_action<std::string>("say").async_send(
        "b", [](boost::system::error_code ec) { BOOST_TEST(!ec); });
    BOOST_TEST(run_until(t.f, [&] { return heard.size() == 2; }));
    std::ranges::sort(heard);
    BOOST_TEST(heard == (std::vector<std::string>{"1a", "2b"}));
}

// Leaving one room ends that room's connection alone: the app's other room keeps its connection to
// the peer, its messages flow, and no departure is seen there.
void leaving_one_room_keeps_the_other_rooms_connection() {
    two_rooms_each t;
    BOOST_TEST(run_until(t.f, [&] { return t.f.net.live_connections() == 4; }));
    std::size_t left_r1 = 0;
    std::size_t left_r2 = 0;
    t.b1->on_peer_leave([&](const trystero::peer_event&) { ++left_r1; });
    t.b2->on_peer_leave([&](const trystero::peer_event&) { ++left_r2; });
    bool done = false;
    t.a1->async_leave([&](boost::system::error_code ec) {
        BOOST_TEST(!ec);
        done = true;
    });
    BOOST_TEST(run_until(t.f, [&] { return done && left_r1 == 1; }));
    // r1's connection is closed at both ends; r2's stays.
    BOOST_TEST(run_until(t.f, [&] { return t.f.net.live_connections() == 2; }));
    std::string heard;
    t.b2->make_action<std::string>("say").on_message(
        [&](const std::string& text, const trystero::message_info&) { heard = text; });
    t.a2->make_action<std::string>("say").async_send(
        "still here", [](boost::system::error_code ec) { BOOST_TEST(!ec); });
    BOOST_TEST(run_until(t.f, [&] { return heard == "still here"; }));
    BOOST_TEST_EQ(left_r2, 0U);
    BOOST_TEST_EQ(t.b2->peers().size(), 1U);
    BOOST_TEST_EQ(t.a2->peers().size(), 1U);
}

// A room writes its action frames bare on its connection, as 0.27.0's sendFrame does: every frame
// starts with the action wire's version, 3, and decodes as an action frame, with no envelope
// around it.
void frames_travel_bare() {
    fixture f{true};
    std::vector<std::vector<std::uint8_t>> frames;
    f.net.filter = [&](std::size_t, std::size_t, std::span<const std::uint8_t> frame) {
        frames.emplace_back(frame.begin(), frame.end());
        return trystero::test::memory_network::verdict::deliver;
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("AAAAAAAAAAAAAAAAAAAA"));
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("BBBBBBBBBBBBBBBBBBBB"));
    auto a = join(a_app, "bare");
    auto b = join(b_app, "bare");
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }));
    webcpp::test::require(BOOST_TEST(!frames.empty()));
    for (const auto& frame : frames) {
        BOOST_TEST_EQ(static_cast<int>(frame.front()),
                      static_cast<int>(trystero::protocol::action_wire_version));
        BOOST_TEST(trystero::protocol::decode_action(frame).frame.has_value());
    }
}

// An app joins as many rooms as it is asked to, as 0.27.0's joinRoom does: no room bound, since no
// connection carries several rooms' tokens any more.
void an_app_joins_more_than_64_rooms() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("AAAAAAAAAAAAAAAAAAAA"));
    std::vector<trystero::room> rooms;
    for (std::size_t i = 0; i < 65; ++i) {
        auto r = a_app.join("room-" + std::to_string(i));
        if (!BOOST_TEST(r.has_value())) {
            return;
        }
        rooms.push_back(std::move(*r));
    }
    BOOST_TEST_EQ(rooms.size(), 65U);
}

void async_receive_works_in_a_coroutine() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("NNNNNNNNNNNNNNNNNNNN"));
    auto a = join(a_app, "coro");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("OOOOOOOOOOOOOOOOOOOO"));
    auto b = join(b_app, "coro");
    auto inbox = b.make_action<trystero::payload>("any");
    std::vector<std::string> seen;
    asio::co_spawn(
        f.io,
        [&]() -> asio::awaitable<void> {
            for (std::size_t i = 0; i < 2; ++i) {
                auto [ec, value, info] =
                    co_await inbox.async_receive(asio::as_tuple(asio::use_awaitable));
                if (ec) {
                    co_return;
                }
                seen.push_back(std::holds_alternative<std::string>(value)
                                   ? std::get<std::string>(value)
                                   : std::get<webcpp::document::document>(value).stringify());
            }
        },
        asio::detached);
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; }));
    a.make_action<std::string>("any").async_send("text", asio::detached);
    a.make_action<webcpp::document::document>("any").async_send(document_of("[1,2]"),
                                                                asio::detached);
    BOOST_TEST(run_until(f, [&] { return seen.size() == 2; }));
    const std::vector<std::string> both{"text", "[1,2]"};
    BOOST_TEST_ALL_EQ(seen.begin(), seen.end(), both.begin(), both.end());
}

// A JSON payload is parsed in the receiving room's resource, as a webcpp::document::pmr::document,
// into an image of at most the room's max_json_document_bytes, whatever type receives it: an array
// of 1,000 zeros is delivered, as is the longest array of zeros whose document fits the bound,
// and the one a zero longer is not, while the messages after it are.
void json_payloads_are_read_in_the_rooms_resource_up_to_its_bound() {
    constexpr std::uint64_t bound = std::uint64_t{256} << 10U;
    std::pmr::unsynchronized_pool_resource z_memory;  // outlives the io_context
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("TTTTTTTTTTTTTTTTTTTT"));
    auto a = join(a_app, "json");
    trystero::test::app z_app(f.io.get_executor(), f.net, app_config("UUUUUUUUUUUUUUUUUUUU"));
    auto z = join(z_app, "json",
                  trystero::room_options{.max_json_document_bytes = bound, .memory = &z_memory});
    std::vector<std::size_t> in_resource;
    std::vector<std::size_t> in_heap;
    std::size_t elsewhere = 0;
    z.make_action<webcpp::document::pmr::document>("pmr").on_message(
        [&](const webcpp::document::pmr::document& v, const trystero::message_info&) {
            elsewhere += v.get_allocator().resource() == &z_memory ? 0 : 1;
            in_resource.push_back(v.root().size());
        });
    z.make_action<webcpp::document::document>("std").on_message(
        [&](const webcpp::document::document& v, const trystero::message_info&) {
            in_heap.push_back(v.root().size());
        });
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; }));
    const auto zeros = [](std::size_t n) {
        std::string text = "[0";
        for (std::size_t i = 1; i < n; ++i) {
            text += ",0";
        }
        return text + "]";
    };
    // The longest array of zeros whose document fits the bound, found by parsing as the room does.
    const auto fits = [&zeros](std::size_t n) {
        webcpp::document::parse_options o;
        o.max_document_bytes = bound;
        return webcpp::document::parse(zeros(n), o).has_value();
    };
    // Each zero is a node of 192 bytes, so no more than bound / 192 of them fit.
    std::size_t at_bound = 1000;
    std::size_t past_bound = (bound / 192) + 1;
    while (past_bound - at_bound > 1) {
        const std::size_t middle = at_bound + ((past_bound - at_bound) / 2);
        (fits(middle) ? at_bound : past_bound) = middle;
    }
    BOOST_TEST(fits(at_bound) && !fits(at_bound + 1));
    for (const char* name : {"pmr", "std"}) {
        auto to_z = a.make_action<webcpp::document::document>(name);
        for (const std::size_t n : {std::size_t{1000}, at_bound, at_bound + 1, std::size_t{3}}) {
            to_z.async_send(document_of(zeros(n)), asio::detached);
        }
    }
    // A data channel keeps its order: the last of each action arrives after the other three.
    BOOST_TEST(run_until(f, [&] { return in_resource.size() >= 3 && in_heap.size() >= 3; }));
    const std::vector<std::size_t> delivered{1000, at_bound, 3};
    BOOST_TEST_ALL_EQ(in_resource.begin(), in_resource.end(), delivered.begin(), delivered.end());
    BOOST_TEST_ALL_EQ(in_heap.begin(), in_heap.end(), delivered.begin(), delivered.end());
    BOOST_TEST_EQ(elsewhere, 0U);
}

// A JSON payload nested as deep as webcpp::document's default, 256, is delivered, to either
// document type: its parse recurses once a level, on the room's executor, on every target and in
// both suspension variants, within the engine's default stack (doc: #trystero-protocol-json).
void a_json_payload_nested_256_deep_is_delivered() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("XXXXXXXXXXXXXXXXXXXX"));
    auto a = join(a_app, "deep");
    trystero::test::app z_app(f.io.get_executor(), f.net, app_config("YYYYYYYYYYYYYYYYYYYY"));
    auto z = join(z_app, "deep");
    std::size_t delivered = 0;
    z.make_action<webcpp::document::pmr::document>("pmr").on_message(
        [&](const webcpp::document::pmr::document& v, const trystero::message_info&) {
            delivered += v.root().is_array() ? 1 : 0;
        });
    z.make_action<webcpp::document::document>("std").on_message(
        [&](const webcpp::document::document& v, const trystero::message_info&) {
            delivered += v.root().is_array() ? 1 : 0;
        });
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; }));
    const std::string deepest = std::string(256, '[') + std::string(256, ']');
    for (const char* name : {"pmr", "std"}) {
        a.make_action<webcpp::document::document>(name).async_send(document_of(deepest),
                                                                   asio::detached);
    }
    BOOST_TEST(run_until(f, [&] { return delivered == 2; }));
}

// A message of another format than a document action's completes its receive with
// error::transfer_rejected and a document that holds nothing, which allocates nothing: its root
// reads undefined and it has no image, in the room's resource for a pmr::document.
void a_rejected_document_receive_holds_nothing() {
    std::pmr::unsynchronized_pool_resource z_memory;  // outlives the io_context
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("VVVVVVVVVVVVVVVVVVVV"));
    auto a = join(a_app, "rejected");
    trystero::test::app z_app(f.io.get_executor(), f.net, app_config("WWWWWWWWWWWWWWWWWWWW"));
    auto z = join(z_app, "rejected", trystero::room_options{.memory = &z_memory});
    std::size_t completed = 0;
    z.make_action<webcpp::document::pmr::document>("pmr").async_receive(
        [&](boost::system::error_code ec, webcpp::document::pmr::document v,
            const trystero::message_info& info) {
            ++completed;
            BOOST_TEST(ec == trystero::error::transfer_rejected);
            BOOST_TEST(v.root().is_undefined());
            BOOST_TEST(v.image().empty());
            BOOST_TEST(v.get_allocator().resource() == &z_memory);
            BOOST_TEST_EQ(info.from, a_app.self_id());
        });
    z.make_action<webcpp::document::document>("std").async_receive(
        [&](boost::system::error_code ec, webcpp::document::document v,
            const trystero::message_info& /*info*/) {
            ++completed;
            BOOST_TEST(ec == trystero::error::transfer_rejected);
            BOOST_TEST(v.root().is_undefined());
            BOOST_TEST(v.image().empty());
        });
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; }));
    a.make_action<std::string>("pmr").async_send("not JSON", asio::detached);
    a.make_action<std::string>("std").async_send("not JSON", asio::detached);
    BOOST_TEST(run_until(f, [&] { return completed == 2; }));
}

void view_handlers_read_the_frame_without_copying() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("RRRRRRRRRRRRRRRRRRRR"));
    auto a = join(a_app, "views");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("SSSSSSSSSSSSSSSSSSSS"));
    auto b = join(b_app, "views");
    std::string text_seen;
    std::size_t bytes_seen = 0;
    std::string metadata_seen;
    b.make_action<std::string_view>("note").on_message(
        [&](std::string_view text, const trystero::message_info&) { text_seen = text; });
    b.make_action<std::span<const std::uint8_t>>("blob").on_message(
        [&](std::span<const std::uint8_t> data, const trystero::message_info& info) {
            bytes_seen = data.size();
            metadata_seen = info.metadata;
        });
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; }));
    a.make_action<std::string_view>("note").async_send(
        std::string_view("a view, copied once at the call"), asio::detached);
    a.make_action<trystero::bytes>("blob").async_send(
        trystero::bytes(50'000, 7), trystero::send_options{.targets = {}, .metadata = R"({"k":1})"},
        asio::detached);
    BOOST_TEST(run_until(f, [&] { return !text_seen.empty() && bytes_seen != 0; }));
    BOOST_TEST_EQ(text_seen, "a view, copied once at the call");
    BOOST_TEST_EQ(bytes_seen, 50'000U);
    BOOST_TEST_EQ(metadata_seen, R"({"k":1})");
}

void a_rate_limited_relay_does_not_stop_discovery() {
    fixture f{true};
    f.net.relay(relay_a).rate_limit_next = 1000;  // relay A refuses everything; B carries the room
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("PPPPPPPPPPPPPPPPPPPP"));
    auto a = join(a_app, "limits");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("QQQQQQQQQQQQQQQQQQQQ"));
    auto b = join(b_app, "limits");
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }));
    // Backoff: after the first refusal each peer stops publishing on relay A, so it sees only a
    // handful of attempts.
    BOOST_TEST_LT(1000 - f.net.relay(relay_a).rate_limit_next, 12U);
}

// A join listener added with `replay` is called at once, within the call, with each peer already
// joined, and its user, before it is in place; without `replay` it waits for the next join. Several
// listeners run in the order added, the user's join after its peer's.
void replay_reports_current_peers() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("RRRRRRRRRRRRRRRRRRRR"));
    auto a = join(a_app, "replay");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("SSSSSSSSSSSSSSSSSSSS"));
    const auto b = join(b_app, "replay");
    std::vector<std::string> order;
    a.on_peer_join(
        [&](const trystero::peer_event& e) { order.push_back("peer " + std::string(e.peer_id)); });
    a.on_user_join(
        [&](const trystero::user_event& e) { order.push_back("user " + std::string(e.user_id)); });
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return order.size() == 2; })));
    const std::string b_id(b_app.self_id());
    const std::string b_user(b_app.user_id());
    const std::vector<std::string> first{"peer " + b_id, "user " + b_user};
    BOOST_TEST_ALL_EQ(order.begin(), order.end(), first.begin(), first.end());

    std::vector<std::pair<std::string, std::string>> replayed;
    const auto all = a.on_peer_join(
        [&](const trystero::peer_event& e) { replayed.emplace_back(e.peer_id, e.user_id); },
        {.once = false, .replay = true});
    // Within the call: B, with the user its handshake verified.
    if (BOOST_TEST_EQ(replayed.size(), 1U)) {
        BOOST_TEST_EQ(replayed[0].first, b_id);
        BOOST_TEST_EQ(replayed[0].second, b_user);
    }
    std::vector<std::string> users;
    const auto user_once =
        a.on_user_join([&](const trystero::user_event& e) { users.emplace_back(e.user_id); },
                       {.once = true, .replay = true});
    const std::vector<std::string> only_b{b_user};
    BOOST_TEST_ALL_EQ(users.begin(), users.end(), only_b.begin(), only_b.end());
    BOOST_TEST(!user_once.active());  // removed before its replay ran
    std::size_t later = 0;
    const auto plain = a.on_peer_join([&](const trystero::peer_event&) { ++later; });
    BOOST_TEST_EQ(later, 0U);

    // The next join reaches every listener still in place, the once one excepted.
    trystero::test::app c_app(f.io.get_executor(), f.net, app_config("TTTTTTTTTTTTTTTTTTTT"));
    const auto c = join(c_app, "replay");
    BOOST_TEST(run_until(f, [&] { return later == 1 && replayed.size() == 2; }));
    BOOST_TEST_EQ(replayed.back().first, c_app.self_id());
    BOOST_TEST_EQ(users.size(), 1U);
    BOOST_TEST(all.active());
    BOOST_TEST(plain.active());
}

// A request action takes one request listener at a time, and one receive gate, as Trystero's
// throws "handler already registered; unsubscribe first"; once it is removed, another is taken.
void second_on_request_is_refused() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("UUUUUUUUUUUUUUUUUUUU"));
    auto a = join(a_app, "requests");
    auto ask = a.make_request_action<std::string>("ask");
    auto first = ask.on_request([](std::string q, const trystero::message_info&) { return q; });
    webcpp::test::require(BOOST_TEST(first.has_value()));
    const auto second =
        ask.on_request([](std::string q, const trystero::message_info&) { return q; });
    BOOST_TEST(second.has_error() && second.error() == trystero::error::already_subscribed);
    // Another handle of the action meets the same listener.
    const auto through_another = a.make_request_action<std::string>("ask").on_request(
        [](std::string q, const trystero::message_info&) { return q; });
    BOOST_TEST(through_another.has_error());
    first->unsubscribe();
    const auto third =
        ask.on_request([](std::string q, const trystero::message_info&) { return q; });
    BOOST_TEST(third.has_value() && third->active());

    const auto gate = ask.on_receive([](const trystero::receive_info&) { return true; });
    webcpp::test::require(BOOST_TEST(gate.has_value()));
    BOOST_TEST(ask.on_receive([](const trystero::receive_info&) { return true; }).has_error());
    auto chat = a.make_action<std::string>("chat");
    BOOST_TEST(chat.on_receive([](const trystero::receive_info&) { return true; }).has_value());
    BOOST_TEST(chat.on_receive([](const trystero::receive_info&) { return true; }).has_error());
}

// A request is answered by the peer asked, with the request's own metadata; an answer that is an
// error refuses it; a peer with no listener lets it wait, so that it times out; an absent target is
// not connected. The request travels in 0.27.0's envelope, `{"r":<uuid>,"m":<metadata>}`.
void a_request_is_answered_or_refused() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("VVVVVVVVVVVVVVVVVVVV"));
    auto a = join(a_app, "answers");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("WWWWWWWWWWWWWWWWWWWW"));
    auto b = join(b_app, "answers");
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; })));
    std::string seen_metadata;
    std::string seen_from;
    auto upper = b.make_request_action<std::string>("upper");
    const auto answering = upper.on_request(
        [&](std::string q,
            const trystero::message_info& info) -> boost::system::result<std::string> {
            seen_metadata = info.metadata;
            seen_from = info.from;
            if (q == "no") {
                return make_error_code(trystero::error::backend_failure);
            }
            std::ranges::transform(q, q.begin(), [](char c) { return c == 'a' ? 'A' : c; });
            return q;
        });
    BOOST_TEST(answering.has_value());
    // Without a listener, B's requests of "later" wait for one, as messages do.
    const auto later_b = b.make_request_action<std::string>("later");

    auto ask = a.make_request_action<std::string>("upper");
    std::optional<std::pair<boost::system::error_code, std::string>> answered;
    ask.async_request(
        "banana",
        trystero::request_options{
            .target = trystero::peer_id(b.self_id()),
            .metadata = R"({"k":[1,2]})",
            .timeout = std::nullopt,
        },
        [&](boost::system::error_code ec, std::string r) { answered.emplace(ec, std::move(r)); });
    BOOST_TEST(run_until(f, [&] { return answered.has_value(); }));
    BOOST_TEST(!answered->first);
    BOOST_TEST_EQ(answered->second, "bAnAnA");
    BOOST_TEST_EQ(seen_metadata, R"({"k":[1,2]})");
    BOOST_TEST_EQ(seen_from, a.self_id());

    std::optional<boost::system::error_code> refused;
    ask.async_request("no", trystero::request_options{.target = trystero::peer_id(b.self_id())},
                      [&](boost::system::error_code ec, const std::string& r) {
                          refused = ec;
                          BOOST_TEST(r.empty());
                      });
    BOOST_TEST(run_until(f, [&] { return refused.has_value(); }));
    BOOST_TEST(*refused == trystero::error::request_rejected);
    BOOST_TEST(seen_metadata.empty());  // the request named none

    std::optional<boost::system::error_code> timed_out;
    a.make_request_action<std::string>("later").async_request(
        "hello",
        trystero::request_options{
            .target = trystero::peer_id(b.self_id()),
            .metadata = {},
            .timeout = 50ms,
        },
        [&](boost::system::error_code ec, const std::string&) { timed_out = ec; });
    BOOST_TEST(run_until(f, [&] { return timed_out.has_value(); }));
    BOOST_TEST(*timed_out == trystero::error::request_timed_out);

    std::optional<boost::system::error_code> absent;
    ask.async_request("x", trystero::request_options{.target = "nobody"},
                      [&](boost::system::error_code ec, const std::string&) { absent = ec; });
    BOOST_TEST(run_until(f, [&] { return absent.has_value(); }));
    BOOST_TEST(*absent == trystero::error::not_connected);
}

// An action without a listener lets its messages wait, as 0.27.0's pre-listener ring does, and
// delivers them once a listener comes; a listener removed makes them wait again. Several listeners
// each read every message, in the order added, a binary one each in a copy of its own.
void messages_wait_for_a_listener() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("XXXXXXXXXXXXXXXXXXXX"));
    auto a = join(a_app, "waiting");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("YYYYYYYYYYYYYYYYYYYY"));
    auto b = join(b_app, "waiting");
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; })));
    auto send = a.make_action<trystero::pmr_bytes>("bin");
    auto receive = b.make_action<trystero::pmr_bytes>("bin");
    std::size_t sent = 0;
    for (std::uint8_t i = 0; i < 3; ++i) {
        send.async_send(trystero::pmr_bytes(1, i), [&](boost::system::error_code ec) {
            BOOST_TEST(!ec);
            ++sent;
        });
    }
    BOOST_TEST(run_until(f, [&] { return sent == 3; }));
    run_until(f, [] { return false; }, 100ms);  // they arrive, and wait
    std::vector<std::string> got;
    auto first =
        receive.on_message([&](const trystero::pmr_bytes& v, const trystero::message_info&) {
            got.push_back("first " + std::to_string(v.at(0)));
        });
    auto second =
        receive.on_message([&](const trystero::pmr_bytes& v, const trystero::message_info&) {
            got.push_back("second " + std::to_string(v.at(0)));
        });
    BOOST_TEST(run_until(f, [&] { return got.size() == 6; }));
    const std::vector<std::string> both{
        "first 0", "second 0", "first 1", "second 1", "first 2", "second 2",
    };
    BOOST_TEST_ALL_EQ(got.begin(), got.end(), both.begin(), both.end());

    first.unsubscribe();
    second.unsubscribe();
    got.clear();
    send.async_send(trystero::pmr_bytes(1, 9), [&](boost::system::error_code) { ++sent; });
    BOOST_TEST(run_until(f, [&] { return sent == 4; }));
    run_until(f, [] { return false; }, 100ms);
    BOOST_TEST(got.empty());
    const auto once = receive.on_message(
        [&](const trystero::pmr_bytes& v, const trystero::message_info&) {
            got.push_back("once " + std::to_string(v.at(0)));
        },
        {.once = true, .replay = false});
    BOOST_TEST(run_until(f, [&] { return got.size() == 1; }));
    BOOST_TEST_EQ(got.at(0), "once 9");
    BOOST_TEST(!once.active());
}

// A receive gate decides before the action receives: a refused inline message is dropped, a
// refused transfer rejected, whose send fails; the progress of an accepted transfer is reported
// after its chunks, rising, and 1 once it is whole.
void a_receive_gate_decides_and_progress_is_reported() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("ZZZZZZZZZZZZZZZZZZZY"));
    auto a = join(a_app, "gate");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("ZZZZZZZZZZZZZZZZZZZX"));
    auto b = join(b_app, "gate");
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; })));
    auto send = a.make_action<trystero::bytes>("file");
    auto receive = b.make_action<trystero::bytes>("file");
    std::vector<std::size_t> asked;
    const auto gate = receive.on_receive([&](const trystero::receive_info& info) {
        asked.push_back(info.byte_length);
        BOOST_TEST_EQ(info.peer_id, a.self_id());
        BOOST_TEST_EQ(info.user_id, a.self().user_id);
        return info.metadata != R"({"keep":false})";
    });
    BOOST_TEST(gate.has_value());
    std::vector<std::size_t> got;
    receive.on_message(
        [&](const trystero::bytes& v, const trystero::message_info&) { got.push_back(v.size()); });
    std::vector<double> shares;
    receive.on_receive_progress([&](double share, const trystero::progress_info& info) {
        shares.push_back(share);
        BOOST_TEST_EQ(info.peer_id, a.self_id());
    });
    std::vector<boost::system::error_code> sent;
    const auto drop = trystero::send_options{.targets = {}, .metadata = R"({"keep":false})"};
    send.async_send(trystero::bytes(10, 1), drop,
                    [&](boost::system::error_code ec) { sent.push_back(ec); });
    send.async_send(trystero::bytes(100'000, 2), drop,
                    [&](boost::system::error_code ec) { sent.push_back(ec); });
    BOOST_TEST(run_until(f, [&] { return sent.size() == 2; }));
    // The inline frame went into the channel; the transfer was rejected.
    BOOST_TEST(!sent.at(0));
    BOOST_TEST(sent.at(1) == trystero::error::transfer_rejected);
    // Four wide chunks, as the memory network's connections take 64 KiB messages.
    send.async_send(trystero::bytes(200'000, 3),
                    [&](boost::system::error_code ec) { sent.push_back(ec); });
    BOOST_TEST(
        run_until(f, [&] { return got.size() == 1 && !shares.empty() && shares.back() == 1.0; }));
    BOOST_TEST_EQ(got.at(0), 200'000U);
    const std::vector<std::size_t> sizes{10, 100'000, 200'000};
    BOOST_TEST_ALL_EQ(asked.begin(), asked.end(), sizes.begin(), sizes.end());
    BOOST_TEST_GT(shares.size(), 2U);
    BOOST_TEST(std::ranges::is_sorted(shares));
    BOOST_TEST_LT(shares.front(), 1.0);
}

// A listener may release the last handles of its room while it is reported: the room stays whole
// until the emission ends, whose other listeners run, as Trystero's leave clears listeners only
// once it completes; nothing is reported after it. A progress listener that drops its room and its
// action at the first share, as a page torn down on a finished upload would, and a join listener
// that drops its room.
void a_listener_may_end_its_room_while_it_is_reported() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("endsAAAAAAAAAAAAAAAA"));
    std::optional<trystero::room> a(join(a_app, "teardown"));
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("endsBBBBBBBBBBBBBBBB"));
    std::optional<trystero::room> b(join(b_app, "teardown"));
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a->peers().size() == 1 && b->peers().size() == 1; })));
    std::optional<trystero::action<trystero::bytes>> send(a->make_action<trystero::bytes>("file"));
    std::optional<trystero::action<trystero::bytes>> receive(
        b->make_action<trystero::bytes>("file"));
    bool b_ended = false;
    std::size_t rest_b = 0;
    std::size_t after_b = 0;
    receive->on_message([&](const trystero::bytes&, const trystero::message_info&) { ++after_b; });
    receive->on_receive_progress([&](double, const trystero::progress_info&) {
        b_ended = true;
        receive.reset();
        b.reset();
    });
    receive->on_receive_progress([&](double, const trystero::progress_info&) { ++rest_b; });
    send->async_send(trystero::bytes(100'000, 1), [](boost::system::error_code) {});
    BOOST_TEST(run_until(f, [&] { return b_ended; }));
    run_until(f, [] { return false; }, 100ms);
    BOOST_TEST_EQ(rest_b, 1U);
    BOOST_TEST_EQ(after_b, 0U);

    bool a_ended = false;
    std::size_t rest_a = 0;
    a->on_peer_join([&](const trystero::peer_event&) {
        a_ended = true;
        send.reset();
        a.reset();
    });
    a->on_peer_join([&](const trystero::peer_event&) { ++rest_a; });
    a->on_user_join([&](const trystero::user_event&) { ++rest_a; });
    trystero::test::app c_app(f.io.get_executor(), f.net, app_config("endsCCCCCCCCCCCCCCCC"));
    const auto c = join(c_app, "teardown");
    BOOST_TEST(run_until(f, [&] { return a_ended; }));
    run_until(f, [] { return false; }, 100ms);
    BOOST_TEST_EQ(rest_a, 2U);  // the peer's join, then its user's, once each
}

// A subscription may outlive its room: it is inactive once the room's last handles are gone, or
// once the room is left, as Trystero clears its listeners then, and unsubscribing does nothing.
void a_subscription_outlives_its_room() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("outlivesAAAAAAAAAAAA"));
    std::optional<trystero::room> a(join(a_app, "outlives"));
    std::optional<trystero::action<std::string>> chat(a->make_action<std::string>("chat"));
    auto joined = a->on_peer_join([](const trystero::peer_event&) {});
    auto said = chat->on_message([](const std::string&, const trystero::message_info&) {});
    auto gate = chat->on_receive([](const trystero::receive_info&) { return true; });
    webcpp::test::require(BOOST_TEST(gate.has_value()));
    BOOST_TEST(joined.active() && said.active() && gate->active());
    chat.reset();
    a.reset();
    run_until(f, [] { return false; }, 50ms);
    BOOST_TEST(!joined.active());
    BOOST_TEST(!said.active());
    BOOST_TEST(!gate->active());
    joined.unsubscribe();
    said.unsubscribe();
    gate->unsubscribe();
    BOOST_TEST(!joined.active());

    auto b = join(a_app, "left");
    const auto left = b.on_peer_leave([](const trystero::peer_event&) {});
    bool done = false;
    b.async_leave([&](boost::system::error_code) { done = true; });
    BOOST_TEST(run_until(f, [&] { return done; }));
    BOOST_TEST(!left.active());
}

// A join the room posted just before it was left is not reported, as 0.27.0 reports none once its
// room is leaving; the listeners go with the leave.
void a_join_posted_before_the_leave_is_not_reported() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("postedAAAAAAAAAAAAAA"));
    auto a = join(a_app, "posted");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("postedBBBBBBBBBBBBBB"));
    const auto b = join(b_app, "posted");
    std::size_t joins = 0;
    const auto peers = a.on_peer_join([&](const trystero::peer_event&) { ++joins; });
    const auto users = a.on_user_join([&](const trystero::user_event&) { ++joins; });
    // One handler at a time, until A has B: the join it posted then has not run yet.
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (a.peers().empty() && std::chrono::steady_clock::now() < end) {
        f.io.run_one_for(20ms);
        if (f.io.stopped()) {
            f.io.restart();
        }
    }
    webcpp::test::require(BOOST_TEST_EQ(a.peers().size(), 1U));
    BOOST_TEST_EQ(joins, 0U);
    bool left = false;
    a.async_leave([&](boost::system::error_code) { left = true; });
    BOOST_TEST(run_until(f, [&] { return left; }));
    BOOST_TEST_EQ(joins, 0U);
    BOOST_TEST(!peers.active());
    BOOST_TEST(!users.active());
}

// A receive gate asked about the messages that waited, as a listener is added, may release the
// room's last handles, that of the action being listened to among them: the room stays whole until
// the call returns.
void a_gate_may_end_its_room_while_waiting_messages_are_delivered() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("gateAAAAAAAAAAAAAAAA"));
    auto a = join(a_app, "gated");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("gateBBBBBBBBBBBBBBBB"));
    std::optional<trystero::room> b(join(b_app, "gated"));
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b->peers().size() == 1; })));
    auto send = a.make_action<trystero::bytes>("kept");
    std::optional<trystero::action<trystero::bytes>> receive(
        b->make_action<trystero::bytes>("kept"));
    std::size_t sent = 0;
    for (std::uint8_t i = 0; i < 2; ++i) {
        send.async_send(trystero::bytes(1, i), [&](boost::system::error_code) { ++sent; });
    }
    BOOST_TEST(run_until(f, [&] { return sent == 2; }));
    run_until(f, [] { return false; }, 100ms);  // they arrive, and wait for a listener
    std::size_t asked = 0;
    const auto gate = receive->on_receive([&](const trystero::receive_info&) {
        ++asked;
        receive.reset();
        b.reset();
        return false;
    });
    BOOST_TEST(gate.has_value());
    BOOST_TEST_EQ(asked, 0U);  // nothing is delivered yet: the action listens to nothing
    std::size_t got = 0;
    const auto listening =
        receive->on_message([&](const trystero::bytes&, const trystero::message_info&) { ++got; });
    BOOST_TEST_EQ(asked, 2U);  // each message that waited, the room gone after the first
    BOOST_TEST(!listening.active());
    BOOST_TEST(!receive.has_value() && !b.has_value());
    run_until(f, [] { return false; }, 100ms);
    BOOST_TEST_EQ(got, 0U);
}

// Leaves are reported as joins are: the peer's, then its user's.
void leaves_are_reported_peer_then_user() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("leavesAAAAAAAAAAAAAA"));
    auto a = join(a_app, "leaves");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("leavesBBBBBBBBBBBBBB"));
    std::optional<trystero::room> b(join(b_app, "leaves"));
    std::vector<std::string> order;
    a.on_user_leave(
        [&](const trystero::user_event& e) { order.push_back("user " + std::string(e.user_id)); });
    a.on_peer_leave(
        [&](const trystero::peer_event& e) { order.push_back("peer " + std::string(e.peer_id)); });
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; })));
    b.reset();
    BOOST_TEST(run_until(f, [&] { return order.size() == 2; }));
    const std::string b_id(b_app.self_id());
    const std::vector<std::string> expected{"peer " + b_id, "user " + std::string(b_app.user_id())};
    BOOST_TEST_ALL_EQ(order.begin(), order.end(), expected.begin(), expected.end());
}

/** An app's setup of `label` that presents `identity`. */
trystero::test::app_setup app_config_of(std::string_view label,
                                        const trystero::user_identity& identity) {
    auto config = app_config(label);
    config.identity = identity;
    return config;
}

/** A user identity of the tests, made or the case ends. */
trystero::user_identity make_user_identity() {
    auto made = trystero::create_user_identity();
    webcpp::test::require(BOOST_TEST(made.has_value()));
    return std::move(*made);
}

// Two peers of one user, two apps that import one user identity, and a peer of a user of its own.
struct one_user_two_peers {
    explicit one_user_two_peers(fixture& f)
        : identity(make_user_identity()),
          user(identity.user_id()),
          a_app(f.io.get_executor(), f.net, app_config("usersAAAAAAAAAAAAAAA")),
          b1_app(f.io.get_executor(), f.net, app_config_of("usersBBBBBBBBBBBBBB1", identity)),
          b2_app(f.io.get_executor(), f.net, app_config_of("usersBBBBBBBBBBBBBB2", identity)),
          a(join(a_app, "users")) {
        a.on_peer_join([this](const trystero::peer_event& e) {
            events.push_back("peer join " + std::string(e.peer_id) + " " + std::string(e.user_id));
        });
        a.on_user_join([this](const trystero::user_event& e) {
            events.push_back("user join " + std::string(e.user_id));
        });
        a.on_peer_leave([this](const trystero::peer_event& e) {
            events.push_back("peer leave " + std::string(e.peer_id) + " " + std::string(e.user_id));
        });
        a.on_user_leave([this](const trystero::user_event& e) {
            events.push_back("user leave " + std::string(e.user_id) + " " + std::string(e.peer_id));
        });
        b1.emplace(join(b1_app, "users"));
        webcpp::test::require(BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; })));
        b2.emplace(join(b2_app, "users"));
        webcpp::test::require(BOOST_TEST(run_until(f, [&] {
            // A's join events are posted, so they can trail its peers: wait for all three.
            return a.peers().size() == 2 && b1->peers().size() == 2 && b2->peers().size() == 2 &&
                   events.size() == 3;
        })));
    }

    trystero::user_identity identity;
    std::string user;  // the identity's id, which b1 and b2 present
    trystero::test::app a_app;
    trystero::test::app b1_app;
    trystero::test::app b2_app;
    trystero::room a;
    std::optional<trystero::room> b1;
    std::optional<trystero::room> b2;
    std::vector<std::string> events;
};

// A user id as a send's target reaches each of its peers; `users()` and `peers()` list them, and
// the user joined once, after its first peer.
void send_to_a_user_reaches_each_of_its_peers() {
    fixture f{true};
    one_user_two_peers t(f);
    const std::string b1_id(t.b1_app.self_id());
    const std::string b2_id(t.b2_app.self_id());
    const std::string user = t.user;
    const std::vector<std::string> joins{
        "peer join " + b1_id + " " + user,
        "user join " + user,
        "peer join " + b2_id + " " + user,
    };
    BOOST_TEST_ALL_EQ(t.events.begin(), t.events.end(), joins.begin(), joins.end());

    const auto users = t.a.users();
    webcpp::test::require(BOOST_TEST_EQ(users.size(), 1U));
    const auto it = users.find(std::string_view(user));
    webcpp::test::require(BOOST_TEST(it != users.end()));
    const std::vector<std::string_view> both{b1_id, b2_id};
    BOOST_TEST_ALL_EQ(it->second.peer_ids.begin(), it->second.peer_ids.end(), both.begin(),
                      both.end());
    BOOST_TEST(it->second.roles.empty());
    const auto peers = t.a.peers();
    webcpp::test::require(BOOST_TEST_EQ(peers.size(), 2U));
    for (const auto& id : both) {
        const auto peer = peers.find(id);
        webcpp::test::require(BOOST_TEST(peer != peers.end()));
        BOOST_TEST_EQ(peer->second.user_id, std::string_view(user));
        BOOST_TEST(peer->second.direct);
        BOOST_TEST_EQ(peer->second.hops, 1U);
        BOOST_TEST(peer->second.roles.empty());
    }

    // A replay reports the user once, with its first peer.
    std::vector<std::string> replayed;
    const auto replay = t.a.on_user_join(
        [&](const trystero::user_event& e) {
            replayed.push_back(std::string(e.user_id) + " " + std::string(e.peer_id));
        },
        {.once = false, .replay = true});
    const std::vector<std::string> once{user + " " + b1_id};
    BOOST_TEST_ALL_EQ(replayed.begin(), replayed.end(), once.begin(), once.end());

    auto note = t.a.make_action<std::string>("note");
    std::vector<std::string> heard;
    t.b1->make_action<std::string>("note").on_message(
        [&](const std::string& text, const trystero::message_info&) {
            heard.push_back("b1 " + text);
        });
    t.b2->make_action<std::string>("note").on_message(
        [&](const std::string& text, const trystero::message_info&) {
            heard.push_back("b2 " + text);
        });
    std::optional<boost::system::error_code> sent;
    note.async_send("to the user",
                    trystero::send_options{.targets = {trystero::peer_id(user)}, .metadata = {}},
                    [&](boost::system::error_code ec) { sent = ec; });
    BOOST_TEST(run_until(f, [&] { return sent.has_value() && heard.size() == 2; }));
    BOOST_TEST(sent && !*sent);
    std::ranges::sort(heard);
    const std::vector<std::string> expected{"b1 to the user", "b2 to the user"};
    BOOST_TEST_ALL_EQ(heard.begin(), heard.end(), expected.begin(), expected.end());

    // A user and one of its peers together reach each peer once.
    heard.clear();
    sent.reset();
    note.async_send("once each",
                    trystero::send_options{
                        .targets =
                            {
                                trystero::peer_id(b2_id),
                                trystero::peer_id(user),
                                trystero::peer_id(b1_id),
                            },
                        .metadata = {},
                    },
                    [&](boost::system::error_code ec) { sent = ec; });
    BOOST_TEST(run_until(f, [&] { return sent.has_value() && heard.size() == 2; }));
    run_until(f, [] { return false; }, 100ms);
    BOOST_TEST_EQ(heard.size(), 2U);
    BOOST_TEST(sent && !*sent);

    // A request to the user asks one of its peers, the first by id, as 0.27.0 picks it.
    auto ask = t.a.make_request_action<std::string>("ask");
    for (auto* b : {&*t.b1, &*t.b2}) {
        const auto answered = b->make_request_action<std::string>("ask").on_request(
            [b](const std::string&, const trystero::message_info&) {
                return std::string(b->self_id());
            });
        webcpp::test::require(BOOST_TEST(answered.has_value()));
    }
    std::optional<std::string> answer;
    ask.async_request("who", trystero::request_options{.target = trystero::peer_id(user)},
                      [&](boost::system::error_code ec, std::string who) {
                          BOOST_TEST(!ec);
                          answer = std::move(who);
                      });
    BOOST_TEST(run_until(f, [&] { return answer.has_value(); }));
    BOOST_TEST_EQ(answer.value_or(""), std::min(b1_id, b2_id));
}

// The scenario of the specification: of two peers of one user, the first leaves and its user
// stays; the second leaves and its user leaves after it.
void a_user_leaves_with_its_last_peer() {
    fixture f{true};
    one_user_two_peers t(f);
    const std::string b1_id(t.b1_app.self_id());
    const std::string b2_id(t.b2_app.self_id());
    const std::string user = t.user;
    t.events.clear();
    t.b1.reset();
    BOOST_TEST(run_until(f, [&] { return t.a.peers().size() == 1; }));
    run_until(f, [] { return false; }, 100ms);
    const std::vector<std::string> first{"peer leave " + b1_id + " " + user};
    BOOST_TEST_ALL_EQ(t.events.begin(), t.events.end(), first.begin(), first.end());
    BOOST_TEST_EQ(t.a.users().size(), 1U);
    t.b2.reset();
    BOOST_TEST(run_until(f, [&] { return t.events.size() == 3; }));
    const std::vector<std::string> all{
        "peer leave " + b1_id + " " + user,
        "peer leave " + b2_id + " " + user,
        "user leave " + user + " " + b2_id,
    };
    BOOST_TEST_ALL_EQ(t.events.begin(), t.events.end(), all.begin(), all.end());
    BOOST_TEST(t.a.users().empty());
    BOOST_TEST(t.a.peers().empty());
}

// A peer of this room's own user is a peer and no other user, as 0.27.0 leaves its own user out
// of `getUsers` and of `onUserJoin`.
void a_peer_of_its_own_user_is_no_other_user() {
    fixture f{true};
    const auto identity = make_user_identity();
    trystero::test::app a_app(f.io.get_executor(), f.net,
                              app_config_of("ownUserAAAAAAAAAAAAA", identity));
    auto a = join(a_app, "own");
    trystero::test::app b_app(f.io.get_executor(), f.net,
                              app_config_of("ownUserBBBBBBBBBBBBB", identity));
    std::size_t user_events = 0;
    a.on_user_join([&](const trystero::user_event&) { ++user_events; });
    a.on_user_leave([&](const trystero::user_event&) { ++user_events; });
    std::optional<trystero::room> b(join(b_app, "own"));
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; })));
    BOOST_TEST_EQ(a.peers().begin()->second.user_id, a_app.user_id());
    BOOST_TEST(a.users().empty());
    std::size_t replayed = 0;
    a.on_user_join([&](const trystero::user_event&) { ++replayed; },
                   {.once = false, .replay = true});
    BOOST_TEST_EQ(replayed, 0U);
    b.reset();
    BOOST_TEST(run_until(f, [&] { return a.peers().empty(); }));
    run_until(f, [] { return false; }, 100ms);
    BOOST_TEST_EQ(user_events, 0U);
}

// A send to a target that is neither an active peer nor a user of one completes with
// `not_connected`, alone or in a list; a broadcast to a room without peers succeeds.
void send_to_an_absent_target_fails() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("absentAAAAAAAAAAAAAA"));
    auto a = join(a_app, "absent");
    auto note = a.make_action<std::string>("note");
    std::vector<boost::system::error_code> sent;
    const auto record = [&](boost::system::error_code ec) { sent.push_back(ec); };
    note.async_send("anyone", record);
    note.async_send("nobody", trystero::send_options{.targets = {"nobody"}, .metadata = {}},
                    record);
    BOOST_TEST(run_until(f, [&] { return sent.size() == 2; }));
    if (sent.size() == 2) {
        BOOST_TEST(!sent[0]);
        BOOST_TEST(sent[1] == trystero::error::not_connected);
    }

    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("absentBBBBBBBBBBBBBB"));
    auto b = join(b_app, "absent");
    std::size_t heard = 0;
    b.make_action<std::string>("note").on_message(
        [&](const std::string&, const trystero::message_info&) { ++heard; });
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; })));
    sent.clear();
    note.async_send("both",
                    trystero::send_options{
                        .targets = {trystero::peer_id(b_app.self_id()), "nobody"},
                        .metadata = {},
                    },
                    record);
    BOOST_TEST(run_until(f, [&] { return sent.size() == 1 && heard == 1; }));
    if (!sent.empty()) {
        BOOST_TEST(sent[0] == trystero::error::not_connected);
    }
}

/** How many subscriptions the two memory relays hold, each. */
std::pair<std::size_t, std::size_t> subscriptions(fixture& f) {
    return {f.net.relay(relay_a).subscription_count(), f.net.relay(relay_b).subscription_count()};
}

// Offline closes the app's relay sockets and holds them closed: no reconnection is tried however
// long it lasts, a room joined meanwhile opens none, and a peer online in that room is not met.
// Online reconnects them at once: the room joined before subscribes again, the one joined
// offline subscribes for the first time, and the two peers meet. Another app's room never moves.
void offline_holds_and_online_reconnects() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("heldAAAAAAAAAAAAAAAA"));
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("heldBBBBBBBBBBBBBBBB"));
    const auto first = join(a_app, "first");
    webcpp::test::require(BOOST_TEST(run_until(
        f, [&] { return subscriptions(f) == std::pair<std::size_t, std::size_t>{1, 1}; })));
    BOOST_TEST_EQ(f.net.websocket_connects, 2U);

    BOOST_TEST(a_app.set_relay_connectivity(trystero::relay_connectivity::offline) ==
               trystero::relay_connectivity::offline);
    BOOST_TEST(a_app.relay_connectivity() == trystero::relay_connectivity::offline);
    BOOST_TEST((subscriptions(f) == std::pair<std::size_t, std::size_t>{0, 0}));
    auto a = join(a_app, "lobby");
    auto b = join(b_app, "lobby");
    trystero::test::advance(f, 10min);
    BOOST_TEST_EQ(f.net.websocket_connects, 4U);  // b's two, and none of a's
    BOOST_TEST((subscriptions(f) == std::pair<std::size_t, std::size_t>{1, 1}));
    BOOST_TEST(a.peers().empty());
    BOOST_TEST(b.peers().empty());

    BOOST_TEST(a_app.set_relay_connectivity(trystero::relay_connectivity::online) ==
               trystero::relay_connectivity::online);
    BOOST_TEST(a_app.relay_connectivity() == trystero::relay_connectivity::online);
    BOOST_TEST_EQ(f.net.websocket_connects, 8U);  // each room of a, on each relay, at once
    BOOST_TEST(run_until(
        f, [&] { return subscriptions(f) == std::pair<std::size_t, std::size_t>{3, 3}; }));
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }, 1min));
    BOOST_TEST(b_app.relay_connectivity() == trystero::relay_connectivity::automatic);
}

// Offline also holds a relay waiting to reconnect: the reconnection pending when the app goes
// offline never fires, however long the app stays offline.
void offline_holds_a_relay_waiting_to_reconnect() {
    fixture f{true};
    f.net.make_unreachable(relay_a);
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("backoffAAAAAAAAAAAAA"));
    const auto a = join(a_app, "backoff");
    // relay_b subscribed and relay_a's first attempt failed: its reconnection is pending.
    webcpp::test::require(BOOST_TEST(run_until(
        f, [&] { return subscriptions(f) == std::pair<std::size_t, std::size_t>{0, 1}; })));
    BOOST_TEST_EQ(f.net.websocket_connects, 2U);
    a_app.set_relay_connectivity(trystero::relay_connectivity::offline);
    trystero::test::advance(f, 10min);
    BOOST_TEST_EQ(f.net.websocket_connects, 2U);
    BOOST_TEST((subscriptions(f) == std::pair<std::size_t, std::size_t>{0, 0}));
}

// Online reconnects a relay waiting in its backoff at once, and its delay bound is back at its
// least, 3333 ms, as 0.27.0's `reconnectRelaySockets` clears `socketRetryPeriods`: the attempt
// that fails right after waits within 3333 ms, not within the minute the backoff had reached.
void online_reconnects_a_backoff_relay_at_once() {
    fixture f{true};
    f.net.make_unreachable(relay_a);
    auto config = app_config("backoffAAAAAAAAAAAAA");
    std::vector<std::string> waits;
    config.log = [&](std::string_view category, std::string_view text) {
        const std::string_view lost = "could not reach wss://relay-a.test, next attempt within ";
        if (category == "nostr" && text.starts_with(lost)) {
            waits.emplace_back(text.substr(lost.size()));
        }
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(config));
    const auto a = join(a_app, "backoff");
    // Ten minutes of failures take relay_a's bound to its most, 60 s.
    trystero::test::advance(f, 10min);
    webcpp::test::require(BOOST_TEST(!waits.empty()));
    BOOST_TEST_EQ(waits.front(), "3333 ms");
    BOOST_TEST_EQ(waits.back(), "60000 ms");
    const auto connects = f.net.websocket_connects;
    const auto failures = waits.size();
    a_app.set_relay_connectivity(trystero::relay_connectivity::online);
    BOOST_TEST_EQ(f.net.websocket_connects, connects + 1);  // relay_a alone, at once
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return waits.size() == failures + 1; })));
    BOOST_TEST_EQ(waits.back(), "3333 ms");
    trystero::test::advance(f, 3333ms);
    BOOST_TEST_EQ(f.net.websocket_connects, connects + 2);
    BOOST_TEST((subscriptions(f) == std::pair<std::size_t, std::size_t>{0, 1}));
}

// Online while online leaves the connected relays alone: no socket is closed or opened, and their
// subscriptions stay.
void online_while_online_leaves_connected_relays_alone() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("steadyAAAAAAAAAAAAAA"));
    const auto a = join(a_app, "steady");
    webcpp::test::require(BOOST_TEST(run_until(
        f, [&] { return subscriptions(f) == std::pair<std::size_t, std::size_t>{1, 1}; })));
    BOOST_TEST_EQ(f.net.websocket_connects, 2U);
    a_app.set_relay_connectivity(trystero::relay_connectivity::online);
    a_app.set_relay_connectivity(trystero::relay_connectivity::online);
    trystero::test::advance(f, 1min);
    BOOST_TEST_EQ(f.net.websocket_connects, 2U);
    BOOST_TEST((subscriptions(f) == std::pair<std::size_t, std::size_t>{1, 1}));
}

// Natively the app has no page to follow, as the memory network has none unless asked: it is
// automatic from the start and its relays connect, automatic again reconnects nothing, and
// automatic after offline reconnects the relays, as 0.27.0's `'auto'` does after a program's
// `'offline'`.
void automatic_is_online_natively() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("autoAAAAAAAAAAAAAAAA"));
    BOOST_TEST(a_app.relay_connectivity() == trystero::relay_connectivity::automatic);
    const auto a = join(a_app, "auto");
    webcpp::test::require(BOOST_TEST(run_until(
        f, [&] { return subscriptions(f) == std::pair<std::size_t, std::size_t>{1, 1}; })));
    BOOST_TEST(a_app.set_relay_connectivity(trystero::relay_connectivity::automatic) ==
               trystero::relay_connectivity::automatic);
    trystero::test::advance(f, 1min);
    BOOST_TEST_EQ(f.net.websocket_connects, 2U);
    a_app.set_relay_connectivity(trystero::relay_connectivity::offline);
    trystero::test::advance(f, 1min);
    BOOST_TEST((subscriptions(f) == std::pair<std::size_t, std::size_t>{0, 0}));
    BOOST_TEST(a_app.set_relay_connectivity(trystero::relay_connectivity::automatic) ==
               trystero::relay_connectivity::automatic);
    BOOST_TEST(run_until(
        f, [&] { return subscriptions(f) == std::pair<std::size_t, std::size_t>{1, 1}; }));
    BOOST_TEST_EQ(f.net.websocket_connects, 4U);
}

// Where the page has `online` and `offline` events, an automatic app follows them: its relays
// close after `offline` and stay closed, and reconnect after `online`.
void automatic_follows_the_pages_events() {
    fixture f{true};
    f.net.page_events = true;
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("pageAAAAAAAAAAAAAAAA"));
    const auto a = join(a_app, "page");
    webcpp::test::require(BOOST_TEST(run_until(
        f, [&] { return subscriptions(f) == std::pair<std::size_t, std::size_t>{1, 1}; })));
    f.net.page_connectivity(false);
    BOOST_TEST(run_until(
        f, [&] { return subscriptions(f) == std::pair<std::size_t, std::size_t>{0, 0}; }));
    trystero::test::advance(f, 10min);
    BOOST_TEST_EQ(f.net.websocket_connects, 2U);
    BOOST_TEST(a_app.relay_connectivity() == trystero::relay_connectivity::automatic);
    f.net.page_connectivity(true);
    BOOST_TEST(run_until(
        f, [&] { return subscriptions(f) == std::pair<std::size_t, std::size_t>{1, 1}; }));
    BOOST_TEST_EQ(f.net.websocket_connects, 4U);
}

// `self()` names this peer, its user, the room and the room it was joined as, synchronously.
void self_names_the_room() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("selfAAAAAAAAAAAAAAAA"));
    const auto a = join(a_app, "kitchen");
    const trystero::self_info self = a.self();
    BOOST_TEST_EQ(self.peer_id, a_app.self_id());
    BOOST_TEST_EQ(self.user_id, a_app.user_id());
    BOOST_TEST(self.persistence == trystero::identity_persistence::ephemeral);
    BOOST_TEST_EQ(self.room_id, "kitchen");
    BOOST_TEST_EQ(self.base_room_id, "kitchen");
    BOOST_TEST(!self.is_admin);
    BOOST_TEST(self.roles.empty());
}

// An app's peer id derives from its endpoint key and its user id from its user key, as 0.27.0
// derives them: two apps of fixed endpoint keys have the ids of those keys, and users of their
// own.
void an_apps_ids_derive_from_its_keys() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("derive-a"));
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("derive-b"));
    BOOST_TEST_EQ(a_app.self_id(), trystero::test::peer_id_of("derive-a"));
    BOOST_TEST_EQ(b_app.self_id(), trystero::test::peer_id_of("derive-b"));
    for (const auto* app : {&a_app, &b_app}) {
        BOOST_TEST_EQ(app->self_id().size(), trystero::protocol::peer_id_size);
        BOOST_TEST_EQ(app->user_id().size(), trystero::protocol::user_id_size);
        BOOST_TEST_EQ(app->user_id().find_first_not_of(trystero::protocol::base62_alphabet),
                      std::string_view::npos);
        // The user key is another key than the endpoint's.
        BOOST_TEST_NE(app->user_id().substr(0, trystero::protocol::peer_id_size), app->self_id());
    }
    BOOST_TEST_NE(a_app.user_id(), b_app.user_id());
    // An app of the same endpoint key has the same peer id, and a user of its own.
    trystero::test::app again(f.io.get_executor(), f.net, app_config("derive-a"));
    BOOST_TEST_EQ(again.self_id(), a_app.self_id());
    BOOST_TEST_NE(again.user_id(), a_app.user_id());
    // An app whose key is drawn has a peer id of its own.
    trystero::test::app drawn(f.io.get_executor(), f.net, app_config(""));
    BOOST_TEST_EQ(drawn.self_id().size(), trystero::protocol::peer_id_size);
    BOOST_TEST_NE(drawn.self_id(), a_app.self_id());
}

// Two apps given one user identity are two endpoints of one user: their peer ids differ, their
// user ids are the identity's, as two pages of one imported identity are in 0.27.0.
void two_apps_of_one_imported_identity_share_a_user_id() {
    fixture f{true};
    const auto identity = trystero::create_user_identity();
    webcpp::test::require(BOOST_TEST(identity.has_value()));
    auto a_config = app_config("shared-a");
    a_config.identity = *identity;
    auto b_config = app_config("shared-b");
    b_config.identity = *identity;
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(a_config));
    trystero::test::app b_app(f.io.get_executor(), f.net, std::move(b_config));
    BOOST_TEST_NE(a_app.self_id(), b_app.self_id());
    BOOST_TEST_EQ(a_app.user_id(), identity->user_id());
    BOOST_TEST_EQ(b_app.user_id(), identity->user_id());
    const auto a = join(a_app, "shared");
    const auto b = join(b_app, "shared");
    BOOST_TEST_EQ(a.self().user_id, b.self().user_id);
    BOOST_TEST_NE(a.self().peer_id, b.self().peer_id);
    // An identity imported from the text of another is that user.
    auto c_config = app_config("shared-c");
    const auto imported = trystero::import_user_identity(trystero::export_user_identity(*identity));
    webcpp::test::require(BOOST_TEST(imported.has_value()));
    c_config.identity = *imported;
    trystero::test::app c_app(f.io.get_executor(), f.net, std::move(c_config));
    BOOST_TEST_EQ(c_app.user_id(), identity->user_id());
}

// `self()` reports where the user identity comes from: made with the app, `ephemeral`; given by
// the program, `imported`, 0.27.0's 'none' and 'custom'.
void self_reports_the_identity() {
    fixture f{true};
    trystero::test::app plain(f.io.get_executor(), f.net, app_config("identity-plain"));
    const auto made = join(plain, "identity");
    BOOST_TEST(made.self().persistence == trystero::identity_persistence::ephemeral);
    BOOST_TEST_EQ(made.self().user_id, plain.user_id());
    BOOST_TEST_EQ(made.self().peer_id, plain.self_id());

    const auto identity = trystero::create_user_identity();
    webcpp::test::require(BOOST_TEST(identity.has_value()));
    auto config = app_config("identity-given");
    config.identity = *identity;
    trystero::test::app given(f.io.get_executor(), f.net, std::move(config));
    const auto imported = join(given, "identity");
    BOOST_TEST(imported.self().persistence == trystero::identity_persistence::imported);
    BOOST_TEST_EQ(imported.self().user_id, identity->user_id());
    BOOST_TEST_EQ(imported.self().peer_id, given.self_id());
}

// An endpoint key that cannot be made leaves the app without a peer id, and each room it joins
// reports `identity_failed` once the join has returned, with 0.27.0's message, then ends without
// a word to a relay, as 0.27.0's failSetup.
void an_endpoint_that_cannot_be_made_reports_identity_failed() {
    fixture f{true};
    const std::array<std::uint8_t, 32> zero{};
    auto a_app =
        trystero::detail::app_access::make(f.io.get_executor(), f.net, app_config("unused"), zero);
    BOOST_TEST(a_app.self_id().empty());
    auto a = join(a_app, "failed");

    struct reported {
        boost::system::error_code code;
        std::string message;
        std::string room_id;
        std::string peer_id;
    };

    std::vector<reported> errors;
    a.on_error([&](const trystero::room_error& e) {
        errors.push_back({
            .code = e.code,
            .message = std::string(e.message),
            .room_id = std::string(e.room_id),
            .peer_id = std::string(e.peer_id),
        });
    });
    BOOST_TEST(errors.empty());  // posted: the join has returned before it is reported
    const auto& impl = trystero::detail::app_access::impl(a_app);
    BOOST_TEST(run_until(f, [&] { return impl.counted_rooms() == 0; }));
    if (BOOST_TEST_EQ(errors.size(), 1U)) {
        BOOST_TEST(errors[0].code == trystero::error::identity_failed);
        BOOST_TEST_EQ(trystero::room_error_code_name(errors[0].code), "identity-failed");
        BOOST_TEST_EQ(errors[0].message, "endpoint identity resolution failed");
        BOOST_TEST_EQ(errors[0].room_id, "failed");
        BOOST_TEST(errors[0].peer_id.empty());
    }
    BOOST_TEST_EQ(f.net.websocket_connects, 0U);
    // The room ended: a leave completes at once, and the room can be joined again, failing again.
    bool left = false;
    a.async_leave([&](boost::system::error_code ec) { left = !ec; });
    BOOST_TEST(run_until(f, [&] { return left; }));
    auto again = join(a_app, "failed");
    std::size_t again_errors = 0;
    again.on_error([&](const trystero::room_error& e) {
        again_errors += e.code == trystero::error::identity_failed ? 1 : 0;
    });
    BOOST_TEST(run_until(f, [&] { return again_errors == 1 && impl.counted_rooms() == 0; }));
    BOOST_TEST_EQ(f.net.websocket_connects, 0U);
}

// A user key that cannot be made keeps the app's peer id and leaves it without a user id; each
// room it joins reports `identity_failed` with 0.27.0's message for the user identity, then ends
// without a word to a relay, as 0.27.0's failSetup.
void a_user_key_that_cannot_be_made_reports_identity_failed() {
    fixture f{true};
    const std::array<std::uint8_t, 32> zero{};
    const auto scalar = endpoint_scalar("user-failed");
    auto a_app = trystero::detail::app_access::make(f.io.get_executor(), f.net,
                                                    app_config("user-failed"), scalar, zero);
    BOOST_TEST_EQ(a_app.self_id(), peer_id_of("user-failed"));
    BOOST_TEST(a_app.user_id().empty());
    auto a = join(a_app, "failed");

    struct reported {
        boost::system::error_code code;
        std::string message;
        std::string room_id;
        std::string peer_id;
    };

    std::vector<reported> errors;
    a.on_error([&](const trystero::room_error& e) {
        errors.push_back({
            .code = e.code,
            .message = std::string(e.message),
            .room_id = std::string(e.room_id),
            .peer_id = std::string(e.peer_id),
        });
    });
    BOOST_TEST(errors.empty());  // posted: the join has returned before it is reported
    const auto& impl = trystero::detail::app_access::impl(a_app);
    BOOST_TEST(run_until(f, [&] { return impl.counted_rooms() == 0; }));
    if (BOOST_TEST_EQ(errors.size(), 1U)) {
        BOOST_TEST(errors[0].code == trystero::error::identity_failed);
        BOOST_TEST_EQ(errors[0].message, "identity resolution failed");
        BOOST_TEST_EQ(errors[0].room_id, "failed");
        BOOST_TEST(errors[0].peer_id.empty());
    }
    BOOST_TEST_EQ(f.net.websocket_connects, 0U);

    // A user scalar fixes the user id as the endpoint scalar fixes the peer id; a given identity
    // takes its place.
    const auto user_scalar = endpoint_scalar("user-fixed");
    auto fixed = trystero::detail::app_access::make(f.io.get_executor(), f.net,
                                                    app_config("user-fixed"), scalar, user_scalar);
    const auto expected =
        trystero::detail::user_identity_access::make(user_scalar, std::pmr::get_default_resource());
    webcpp::test::require(BOOST_TEST(expected.has_value()));
    BOOST_TEST_EQ(fixed.user_id(), expected->user_id());
    BOOST_TEST_NE(fixed.user_id(), fixed.self_id());
    auto config = app_config("user-given");
    const auto given_identity = trystero::create_user_identity();
    webcpp::test::require(BOOST_TEST(given_identity.has_value()));
    config.identity = *given_identity;
    auto given = trystero::detail::app_access::make(f.io.get_executor(), f.net, std::move(config),
                                                    scalar, zero);
    BOOST_TEST_EQ(given.user_id(), given_identity->user_id());
}

/** Refusals of a request with long reasons. */
class reasons final : public boost::system::error_category {
public:
    const char* name() const noexcept override { return "reasons"; }

    std::string message(int ev) const override {
        // 511 UTF-16 code units, then a character of two of them, which the cut splits.
        return ev == 1 ? std::string(511, 'x') + "\xF0\x9F\x98\x80tail" : std::string(600, 'y');
    }
};

// A request whose listener is removed after it arrived, before its post runs, is refused with "no
// active request handler"; one that the gate refuses with "payload rejected by receiver"; a
// listener's reason is cut at 512 UTF-16 code units, as 0.27.0 cuts it, a pair cut in two leaving
// its high surrogate, which the requester reads. The requester's request completes with
// `request_rejected`, and the reason is logged.
void a_request_is_refused_by_a_gone_listener_or_the_gate() {
    fixture f{true};
    auto config = app_config("refusedAAAAAAAAAAAAA");
    std::vector<std::string> reasons_logged;
    config.log = [&](std::string_view category, std::string_view text) {
        if (category == "action" && text.find(" refused by ") != std::string_view::npos) {
            reasons_logged.emplace_back(text.substr(text.find(": ") + 2));
        }
    };
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(config));
    auto a = join(a_app, "refused");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("refusedBBBBBBBBBBBBB"));
    auto b = join(b_app, "refused");
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; })));
    const auto echo = [](std::string q, const trystero::message_info&) { return q; };

    // The progress of 1 is posted before the request: its listener removes the request's.
    auto gone = b.make_request_action<std::string>("gone");
    auto answering = gone.on_request(echo);
    webcpp::test::require(BOOST_TEST(answering.has_value()));
    gone.on_receive_progress([&](double share, const trystero::progress_info&) {
        if (share == 1.0) {
            answering->unsubscribe();
        }
    });
    auto gated = b.make_request_action<std::string>("gated");
    BOOST_TEST(gated.on_request(echo).has_value());
    BOOST_TEST(gated.on_receive([](const trystero::receive_info&) { return false; }).has_value());
    static const reasons category{};
    auto cut = b.make_request_action<std::string>("cut");
    BOOST_TEST(
        cut.on_request([](const std::string& q,
                          const trystero::message_info&) -> boost::system::result<std::string> {
               return boost::system::error_code(q == "pair" ? 1 : 2, category);
           })
            .has_value());

    std::vector<boost::system::error_code> results;
    const auto ask = [&](std::string_view name, std::string q) {
        a.make_request_action<std::string>(name).async_request(
            std::move(q), trystero::request_options{.target = trystero::peer_id(b.self_id())},
            [&](boost::system::error_code ec, const std::string&) { results.push_back(ec); });
        return run_until(f, [&, n = results.size() + 1] { return results.size() == n; });
    };
    BOOST_TEST(ask("gone", "q"));
    BOOST_TEST(ask("gated", "q"));
    BOOST_TEST(ask("cut", "pair"));
    BOOST_TEST(ask("cut", "ascii"));
    for (const auto& ec : results) {
        BOOST_TEST(ec == trystero::error::request_rejected);
    }
    if (BOOST_TEST_EQ(reasons_logged.size(), 4U)) {
        BOOST_TEST_EQ(reasons_logged[0], "no active request handler");
        BOOST_TEST_EQ(reasons_logged[1], "payload rejected by receiver");
        // The high surrogate left alone is read as the replacement character.
        BOOST_TEST_EQ(reasons_logged[2], std::string(511, 'x') + "\xEF\xBF\xBD");
        BOOST_TEST_EQ(reasons_logged[3], std::string(512, 'y'));
    }
}

// A request fails with `peer_disconnected` when the peer asked leaves first, and with `room_left`
// when the requester's room is left first. The room is left at once, by its destruction: during an
// `async_leave`'s farewell the peer, told `\@_leave`, closes the room's connection to it, and a
// request still waiting then fails with `peer_disconnected`, as 0.27.0's exitPeer fails it.
void a_request_fails_when_its_peer_or_its_room_goes() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("goesAAAAAAAAAAAAAAAA"));
    auto a1 = join(a_app, "goes-peer");
    std::optional<trystero::room> a2(join(a_app, "goes-room"));
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("goesBBBBBBBBBBBBBBBB"));
    std::optional<trystero::room> b1(join(b_app, "goes-peer"));
    auto b2 = join(b_app, "goes-room");
    webcpp::test::require(BOOST_TEST(run_until(f, [&] {
        return a1.peers().size() == 1 && a2->peers().size() == 1 && b2.peers().size() == 1;
    })));
    // B makes the actions but answers nothing: the requests wait.
    const auto slow1 = b1->make_request_action<std::string>("slow");
    const auto slow2 = b2.make_request_action<std::string>("slow");
    std::optional<boost::system::error_code> peer_gone;
    a1.make_request_action<std::string>("slow").async_request(
        "q", trystero::request_options{.target = trystero::peer_id(b2.self_id())},
        [&](boost::system::error_code ec, const std::string&) { peer_gone = ec; });
    std::optional<boost::system::error_code> room_gone;
    a2->make_request_action<std::string>("slow").async_request(
        "q", trystero::request_options{.target = trystero::peer_id(b2.self_id())},
        [&](boost::system::error_code ec, const std::string&) { room_gone = ec; });
    run_until(f, [] { return false; }, 100ms);
    BOOST_TEST(!peer_gone && !room_gone);
    b1.reset();
    BOOST_TEST(run_until(f, [&] { return peer_gone.has_value(); }));
    BOOST_TEST(*peer_gone == trystero::error::peer_disconnected);
    a2.reset();
    BOOST_TEST(run_until(f, [&] { return room_gone.has_value(); }));
    BOOST_TEST(*room_gone == trystero::error::room_left);
}

// A name keeps the kind of action it was made first, as 0.27.0 refuses to redefine it ("action
// type cannot be redefined", which it throws): a handle of the other kind fails each operation with
// `action_redefined` and adds no listener; another of the first kind is the same action.
void an_action_name_keeps_its_first_kind() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("kindAAAAAAAAAAAAAAAA"));
    auto a = join(a_app, "kinds");
    std::vector<boost::system::error_code> failed;

    auto chat = a.make_action<std::string>("chat");
    BOOST_TEST(chat.on_message([](const std::string&, const trystero::message_info&) {}).active());
    auto ask_chat = a.make_request_action<std::string>("chat");
    const auto request_listener =
        ask_chat.on_request([](std::string q, const trystero::message_info&) { return q; });
    BOOST_TEST(request_listener.has_error() &&
               request_listener.error() == trystero::error::action_redefined);
    const auto request_gate =
        ask_chat.on_receive([](const trystero::receive_info&) { return true; });
    BOOST_TEST(request_gate.has_error() &&
               request_gate.error() == trystero::error::action_redefined);
    BOOST_TEST(
        !ask_chat.on_receive_progress([](double, const trystero::progress_info&) {}).active());
    ask_chat.async_request("q", trystero::request_options{.target = "anyone"},
                           [&](boost::system::error_code ec, const std::string& r) {
                               failed.push_back(ec);
                               BOOST_TEST(r.empty());
                           });

    auto ask = a.make_request_action<std::string>("ask");
    auto say_ask = a.make_action<std::string>("ask");
    BOOST_TEST(
        !say_ask.on_message([](const std::string&, const trystero::message_info&) {}).active());
    BOOST_TEST(
        !say_ask.on_receive_progress([](double, const trystero::progress_info&) {}).active());
    const auto message_gate =
        say_ask.on_receive([](const trystero::receive_info&) { return true; });
    BOOST_TEST(message_gate.has_error() &&
               message_gate.error() == trystero::error::action_redefined);
    say_ask.async_send("hello", [&](boost::system::error_code ec) { failed.push_back(ec); });
    say_ask.async_receive(
        [&](boost::system::error_code ec, const std::string& v, const trystero::message_info&) {
            failed.push_back(ec);
            BOOST_TEST(v.empty());
        });
    BOOST_TEST(run_until(f, [&] { return failed.size() == 3; }));
    for (const auto& ec : failed) {
        BOOST_TEST(ec == trystero::error::action_redefined);
    }
    // The first kind stays whole: another handle of it is the same action.
    BOOST_TEST(a.make_request_action<std::string>("ask")
                   .on_request([](std::string q, const trystero::message_info&) { return q; })
                   .has_value());
    BOOST_TEST(ask.on_request([](std::string q, const trystero::message_info&) { return q; })
                   .has_error());  // already_subscribed, through the other handle
}

// The errors' values are fixed and never reused; their messages name what failed.
/** A room error, its views copied, and when it was reported. */
struct reported_error {
    boost::system::error_code code;
    std::string message;
    std::string app_id;
    std::string room_id;
    std::string peer_id;
    std::optional<trystero::signal_stage> stage;
    bool turn_configured = false;
    std::chrono::steady_clock::time_point at;
};

/** A listener that records every report of `code` into `to`, with the fixture's time. */
auto record_errors(fixture& f, std::vector<reported_error>& to, trystero::error code) {
    return [&f, &to, code](const trystero::room_error& e) {
        if (e.code != code) {
            return;
        }
        to.push_back({
            .code = e.code,
            .message = std::string(e.message),
            .app_id = std::string(e.app_id),
            .room_id = std::string(e.room_id),
            .peer_id = std::string(e.peer_id),
            .stage = e.stage,
            .turn_configured = e.turn_configured,
            .at = f.net.steady_now(),
        });
    };
}

// Two apps whose passwords differ, on the virtual clock: A, the lower id, offers, and B cannot open
// its offers. B reports decrypt_failed once, with A's id, the stage and 0.27.0's message, though A
// sends its offer again and again within the minute; once the minute has passed, A's next offer
// is reported again. A, which receives nothing sealed from B, reports nothing, and neither joins.
void decrypt_failed_is_reported_once_a_minute() {
    fixture f{true};
    const auto labels = ordered_labels(2, "private");  // the apps, by ascending peer id
    const std::string a_id = peer_id_of(labels[0]);
    const std::string b_id = peer_id_of(labels[1]);
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(labels[0], "s3cret"));
    auto a = join(a_app, "private");
    std::vector<reported_error> a_reports;
    a.on_error([&](const trystero::room_error& e) {
        a_reports.push_back({});
        a_reports.back().code = e.code;
    });
    flooder watcher(f, "private");
    std::vector<std::chrono::steady_clock::time_point> offers;
    watcher.on_text = [&](std::string_view text) {
        const auto s = relayed_signal(text);
        if (s && s->peer == a_id && !s->offer.empty()) {
            offers.push_back(f.net.steady_now());
        }
    };
    watcher.watch(trystero::protocol::peer_topic(app_id, "private", b_id));
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[1], "guess"));
    auto b = join(b_app, "private");
    std::vector<reported_error> reports;
    b.on_error(record_errors(f, reports, trystero::error::decrypt_failed));
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return !reports.empty(); }, 30s)));
    const auto& first = reports.front();
    BOOST_TEST_EQ(trystero::room_error_code_name(first.code), "decrypt-failed");
    BOOST_TEST_EQ(first.message,
                  "failed to decrypt offer (incorrect password or corrupted signal)");
    BOOST_TEST_EQ(first.app_id, app_id);
    BOOST_TEST_EQ(first.room_id, "private");
    BOOST_TEST_EQ(first.peer_id, a_id);
    BOOST_TEST(first.stage == trystero::signal_stage::offer);
    const auto reported = first.at;

    // A minute but a millisecond after the report, A has sent its offer again, and B reported none
    // of them.
    trystero::test::advance(f, std::chrono::duration_cast<std::chrono::milliseconds>(
                                   reported + 59'999ms - f.net.steady_now()));
    BOOST_TEST_EQ(reports.size(), 1U);
    BOOST_TEST_GE(std::ranges::count_if(offers, [&](auto t) { return t > reported; }), 1);
    BOOST_TEST(run_until(f, [&] { return reports.size() == 2; }, 3min));
    if (reports.size() == 2) {
        BOOST_TEST(reports[1].at - reported >= 60s);
        BOOST_TEST_EQ(reports[1].peer_id, a_id);
        BOOST_TEST(reports[1].stage == trystero::signal_stage::offer);
    }
    BOOST_TEST(a_reports.empty());
    BOOST_TEST(a.peers().empty());
    BOOST_TEST(b.peers().empty());
}

// Offers that do not open, from fresh ids: each id is reported once, however often it sends, and
// the room remembers the last 512 ids it reported, as 0.27.0's cache does: past them, the first
// is forgotten and reported again.
void signals_that_do_not_open_are_reported_once_per_peer_and_stage() {
    constexpr std::size_t remembered = 512;
    fixture f{true};
    const auto a_label = label_beside("1", false);  // above the garbage's ids, which offer
    const std::string a_id = peer_id_of(a_label);
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(a_label));
    auto a = join(a_app, "reports");
    std::vector<reported_error> reports;
    a.on_error(record_errors(f, reports, trystero::error::decrypt_failed));
    flooder attacker(f, "reports");
    const auto a_topic = trystero::protocol::peer_topic(app_id, "reports", a_id);
    const auto offer = [&](std::size_t i) {
        const auto since_epoch = f.net.steady_now().time_since_epoch();
        attacker.publish(a_topic,
                         {
                             .peer_id = "0garbage" + std::to_string(i),
                             .offer_id = "o",
                             .offer = "not a sealed offer",
                         },
                         std::to_string(i) + "-" + std::to_string(since_epoch.count()));
    };
    for (std::size_t i = 0; i < 2 * remembered; ++i) {
        offer(i % remembered);
        if (i % 32 == 31) {
            trystero::test::advance(f, 100ms);
        }
    }
    BOOST_TEST(run_until(f, [&] { return reports.size() >= remembered; }));
    trystero::test::advance(f, 1s);
    BOOST_TEST_EQ(reports.size(), remembered);
    for (std::size_t i = 0; i < std::min(reports.size(), remembered); ++i) {
        BOOST_TEST_EQ(reports[i].peer_id, "0garbage" + std::to_string(i));
        BOOST_TEST(reports[i].stage == trystero::signal_stage::offer);
    }
    // An id seen again within the minute becomes the most recent: offered alone now, the first is
    // not reported, and the second is the least recent.
    offer(0);
    trystero::test::advance(f, 100ms);
    BOOST_TEST_EQ(reports.size(), remembered);
    // One more id makes the room forget the second, which it then reports again within the minute,
    // and not the first, which the room would forget were the ids kept in the order they came.
    offer(remembered);
    BOOST_TEST(run_until(f, [&] { return reports.size() == remembered + 1; }));
    offer(0);
    offer(1);
    BOOST_TEST(run_until(f, [&] { return reports.size() == remembered + 2; }));
    trystero::test::advance(f, 1s);
    BOOST_TEST_EQ(reports.size(), remembered + 2);
    if (reports.size() == remembered + 2) {
        BOOST_TEST_EQ(reports[remembered].peer_id, "0garbage" + std::to_string(remembered));
        BOOST_TEST_EQ(reports.back().peer_id, "0garbage1");
    }
}

// A room whose relays are all unreachable reports relay_failed once, with no peer, however often it
// tries them again; a room that reaches one of its relays reports nothing.
void relay_failed_when_every_relay_fails() {
    {
        fixture f{true};
        f.net.make_unreachable(relay_a);
        f.net.make_unreachable(relay_b);
        trystero::test::app a_app(f.io.get_executor(), f.net, app_config("relaysAAAAAAAAAAAAAA"));
        auto a = join(a_app, "unreachable");
        std::vector<reported_error> reports;
        a.on_error(record_errors(f, reports, trystero::error::relay_failed));
        webcpp::test::require(BOOST_TEST(run_until(f, [&] { return !reports.empty(); })));
        BOOST_TEST_EQ(trystero::room_error_code_name(reports[0].code), "relay-failed");
        BOOST_TEST_EQ(reports[0].message, "all signaling relays failed");
        BOOST_TEST_EQ(reports[0].app_id, app_id);
        BOOST_TEST_EQ(reports[0].room_id, "unreachable");
        BOOST_TEST(reports[0].peer_id.empty());
        BOOST_TEST(!reports[0].stage.has_value());
        trystero::test::advance(f, 3min);
        BOOST_TEST_EQ(reports.size(), 1U);
    }
    {
        fixture f{true};
        f.net.make_unreachable(relay_a);
        trystero::test::app a_app(f.io.get_executor(), f.net, app_config("relaysAAAAAAAAAAAAAA"));
        auto a = join(a_app, "half");
        std::size_t reports = 0;
        a.on_error([&](const trystero::room_error&) { ++reports; });
        trystero::test::advance(f, 3min);
        BOOST_TEST_EQ(reports, 0U);
    }
}

// Two peers whose data channels never open, on the virtual clock: once the offer is answered,
// each side reports peer_connect_failed for the other when its connection expires, with
// whether a TURN server was configured, and 0.27.0's message.
void peer_connect_failed_says_whether_turn_was_configured() {
    for (const bool turn : {false, true}) {
        fixture f{true};
        f.net.defer_opens = true;
        const std::string a_id = peer_id_of("connect-a");
        const std::string b_id = peer_id_of("connect-b");
        const auto config = [&](std::string_view label) {
            auto c = app_config(label);
            if (turn) {
                c.ice_servers.push_back({
                    .urls =
                        {
                            std::pmr::string("stun:stun.example:3478"),
                            std::pmr::string("TURNS:turn.example:5349"),
                        },
                    .username = std::pmr::string("user"),
                    .credential = std::pmr::string("secret"),
                });
            }
            return c;
        };
        trystero::test::app a_app(f.io.get_executor(), f.net, config("connect-a"));
        auto a = join(a_app, "connect");
        trystero::test::app b_app(f.io.get_executor(), f.net, config("connect-b"));
        auto b = join(b_app, "connect");
        std::vector<reported_error> a_reports;
        std::vector<reported_error> b_reports;
        a.on_error(record_errors(f, a_reports, trystero::error::peer_connect_failed));
        b.on_error(record_errors(f, b_reports, trystero::error::peer_connect_failed));
        webcpp::test::require(BOOST_TEST(
            run_until(f, [&] { return !a_reports.empty() && !b_reports.empty(); }, 2min)));
        const std::string hint = turn ? "verify TURN server URLs and credentials"
                                      : "configure TURN via app_config::ice_servers";
        for (const auto& [reports, peer] :
             {std::pair{&a_reports, b_id}, std::pair{&b_reports, a_id}}) {
            const auto& e = reports->front();
            BOOST_TEST_EQ(trystero::room_error_code_name(e.code), "peer-connect-failed");
            BOOST_TEST_EQ(e.peer_id, peer);
            BOOST_TEST_EQ(e.turn_configured, turn);
            std::string expected = "could not connect to peer ";
            expected += peer;
            expected += " after exchanging SDP; ";
            expected += hint;
            BOOST_TEST_EQ(e.message, expected);
            BOOST_TEST_EQ(e.room_id, "connect");
        }
        BOOST_TEST(a.peers().empty());
    }
}

/** The name of an action frame's action, or empty for a chunk, a control frame or no frame. */
std::string action_name(std::span<const std::uint8_t> frame) {
    const auto decoded = trystero::protocol::decode_action(frame);
    const auto* p =
        decoded.frame ? std::get_if<trystero::protocol::proposal>(&*decoded.frame) : nullptr;
    return p != nullptr ? std::string(p->name) : std::string();
}

/** Whether the memory network's connection `pc` has its channel in `memory`: in a case that gives
 * one app a resource of its own, whether `pc` is that app's end. */
bool end_in(fixture& f, std::size_t pc, const std::pmr::memory_resource* memory) {
    const auto connection = f.net.find(pc);
    const auto channel = connection ? connection->channel() : nullptr;
    return channel && channel->memory() == memory;
}

/** The ready frame of the fixture's `handshake_frames` entry `ready`, which 0.27.0 writes:
 * `@_hsready` with an empty text (crypto_layer's `handshake_frames_match` reads it from the
 * fixture). */
std::vector<std::uint8_t> recorded_ready_frame() {
    constexpr std::string_view hex =
        "0300405f6873726561647900000000000000000000000000000000000000000000000000";
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(
            static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

// A handshake that never completes fails after 0.27.0's 10 s: A's end of the connection opens and
// B's never does, so that only A runs a handshake, its frames waiting at B's end; A reports
// handshake_failed with 0.27.0's reason and B's id when its timer expires, and lets B go. B starts
// no handshake and has no timer, so which side expires first is not a question. Neither reports a
// join. All of it on the virtual clock.
void a_handshake_that_never_completes_fails_after_10_s() {
    counting_resource a_memory;  // first: A's connection ends are made in it, which names them
    fixture f{true};
    f.net.defer_opens = true;
    const auto labels = ordered_labels(2, "slow");  // the apps, by ascending peer id
    const std::string a_id = peer_id_of(labels[0]);
    const std::string b_id = peer_id_of(labels[1]);
    auto a_config = app_config(labels[0]);
    a_config.memory = &a_memory;
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(a_config));
    auto a = join(a_app, "slow");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[1]));
    auto b = join(b_app, "slow");
    std::vector<reported_error> a_reports;
    a.on_error(record_errors(f, a_reports, trystero::error::handshake_failed));
    std::vector<reported_error> b_reports;
    b.on_error(record_errors(f, b_reports, trystero::error::handshake_failed));
    std::size_t joins = 0;
    a.on_peer_join([&](const trystero::peer_event&) { ++joins; });
    b.on_peer_join([&](const trystero::peer_event&) { ++joins; });
    // The first pair negotiated, both ends made and neither open.
    std::optional<std::size_t> a_pc;
    webcpp::test::require(BOOST_TEST(run_until(f, [&] {
        for (std::size_t id = 0; id < f.net.made_connections && !a_pc; ++id) {
            if (end_in(f, id, &a_memory) && f.net.peer_of(id) != id) {
                a_pc = id;
            }
        }
        return a_pc.has_value();
    })));
    f.net.open(*a_pc);  // B's end stays closed
    trystero::test::advance(f, 9'500ms);
    BOOST_TEST(a_reports.empty());  // 9.5 s after A's handshake started
    trystero::test::advance(f, 1000ms);
    if (BOOST_TEST_EQ(a_reports.size(), 1U)) {
        const auto& e = a_reports.front();
        BOOST_TEST_EQ(trystero::room_error_code_name(e.code), "handshake-failed");
        BOOST_TEST_EQ(e.message, "handshake timed out after 10000ms");
        BOOST_TEST_EQ(e.peer_id, b_id);
        BOOST_TEST_EQ(e.room_id, "slow");
    }
    trystero::test::advance(f, 5000ms);
    BOOST_TEST_EQ(a_reports.size(), 1U);
    BOOST_TEST(b_reports.empty());
    BOOST_TEST_EQ(joins, 0U);
    BOOST_TEST(a.peers().empty());
    BOOST_TEST(b.peers().empty());
}

// The ready frame a room sends is 0.27.0's, byte for byte: `@_hsready` with an empty text, as
// 0.27.0's handshake manager sends `payload || ''`.
void ready_frames_carry_an_empty_text() {
    counting_resource a_memory;
    fixture f{true};
    std::vector<std::vector<std::uint8_t>> from_a;
    f.net.filter = [&](std::size_t from, std::size_t, std::span<const std::uint8_t> frame) {
        if (end_in(f, from, &a_memory) && action_name(frame) == "@_hsready") {
            from_a.emplace_back(frame.begin(), frame.end());
        }
        return trystero::test::memory_network::verdict::deliver;
    };
    auto a_config = app_config("readyAAAAAAAAAAAAAAA");
    a_config.memory = &a_memory;
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(a_config));
    auto a = join(a_app, "ready");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("readyBBBBBBBBBBBBBBB"));
    auto b = join(b_app, "ready");
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }));
    const auto expected = recorded_ready_frame();
    if (BOOST_TEST_EQ(from_a.size(), 1U)) {
        BOOST_TEST_ALL_EQ(from_a[0].begin(), from_a[0].end(), expected.begin(), expected.end());
    }
}

// A room sends its ready frame before the peer joins, so that what a listener of on_peer_join
// sends follows it on the wire, as 0.27.0's manager enqueues the frame before onActivate. B's end
// of the connection opens first, so that B's ready frame is already at A's end when A's handshake
// starts: A then sends its ready frame and activates the peer in one call, where the order matters.
void sends_inside_on_peer_join_follow_the_ready_frame() {
    counting_resource a_memory;
    fixture f{true};
    f.net.defer_opens = true;
    std::vector<std::string> from_a;
    f.net.filter = [&](std::size_t from, std::size_t, std::span<const std::uint8_t> frame) {
        if (end_in(f, from, &a_memory)) {
            from_a.push_back(action_name(frame));
        }
        return trystero::test::memory_network::verdict::deliver;
    };
    auto a_config = app_config("joinAAAAAAAAAAAAAAAA");
    a_config.memory = &a_memory;
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(a_config));
    auto a = join(a_app, "greet");
    auto greet = a.make_action<std::string>("greet");
    a.on_peer_join([&](const trystero::peer_event& e) {
        greet.async_send("hello", trystero::send_options{.targets = {trystero::peer_id(e.peer_id)}},
                         [](boost::system::error_code ec) { BOOST_TEST(!ec); });
    });
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("joinBBBBBBBBBBBBBBBB"));
    auto b = join(b_app, "greet");
    std::vector<std::string> heard;
    auto greeted = b.make_action<std::string>("greet");
    greeted.on_message(
        [&](const std::string& text, const trystero::message_info&) { heard.push_back(text); });
    std::optional<std::size_t> a_pc;
    webcpp::test::require(BOOST_TEST(run_until(f, [&] {
        for (std::size_t id = 0; id < f.net.made_connections && !a_pc; ++id) {
            if (end_in(f, id, &a_memory) && f.net.peer_of(id) != id) {
                a_pc = id;
            }
        }
        return a_pc.has_value();
    })));
    f.net.open(f.net.peer_of(*a_pc));
    trystero::test::advance(f, 100ms);  // B's handshake runs, its ready frame waits for A's end
    BOOST_TEST(from_a.empty());
    f.net.open(*a_pc);
    BOOST_TEST(run_until(f, [&] { return heard.size() == 1; }));
    const auto ready = std::ranges::find(from_a, "@_hsready");
    const auto hello = std::ranges::find(from_a, "greet");
    BOOST_TEST(ready != from_a.end());
    BOOST_TEST(hello != from_a.end());
    BOOST_TEST(ready < hello);
}

// A handshake message is at most 64 KiB and a ready frame's payload 128 KiB, 0.27.0's
// maxPayloadBytes of `@_hsdata` and `@_hsready`: an offer of one byte more is rejected as "payload
// exceeds receiver size limit", one at the bound accepted.
void handshake_messages_keep_0_27_0s_bounds() {
    namespace tp = trystero::protocol;
    counting_resource a_memory;
    fixture f{true};

    struct answer {
        tp::frame_kind kind;
        std::string reason;
    };

    std::map<std::uint32_t, answer> from_a;  // A's answers, by transfer id
    f.net.filter = [&](std::size_t from, std::size_t, std::span<const std::uint8_t> frame) {
        if (end_in(f, from, &a_memory)) {
            const auto decoded = tp::decode_action(frame);
            if (const auto* m =
                    decoded.frame ? std::get_if<tp::control_message>(&*decoded.frame) : nullptr) {
                from_a.insert_or_assign(m->id, answer{m->kind, std::string(m->reason)});
            }
        }
        return trystero::test::memory_network::verdict::deliver;
    };
    auto a_config = app_config("boundAAAAAAAAAAAAAAA");
    a_config.memory = &a_memory;
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(a_config));
    auto a = join(a_app, "bound");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("boundBBBBBBBBBBBBBBB"));
    auto b = join(b_app, "bound");
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; })));
    std::optional<std::size_t> a_pc;
    for (std::size_t id = 0; id < f.net.made_connections && !a_pc; ++id) {
        const auto connection = f.net.find(id);
        if (end_in(f, id, &a_memory) && connection->channel()->is_open()) {
            a_pc = id;
        }
    }
    webcpp::test::require(BOOST_TEST(a_pc.has_value()));
    const std::size_t hsdata = std::size_t{64} * 1024;
    const std::size_t hsready = std::size_t{128} * 1024;
    f.net.inject(*a_pc, tp::encode_offer("@_hsdata", tp::payload_format::json, "", 1, hsdata + 1));
    f.net.inject(*a_pc, tp::encode_offer("@_hsdata", tp::payload_format::json, "", 2, hsdata));
    f.net.inject(*a_pc,
                 tp::encode_offer("@_hsready", tp::payload_format::json, "", 3, hsready + 1));
    BOOST_TEST(run_until(f, [&] { return from_a.size() == 3; }));
    // A peer's transfers are received one at a time: the second goes, then the fourth is taken.
    f.net.inject(*a_pc, tp::encode_cancelled(2));
    f.net.inject(*a_pc, tp::encode_offer("@_hsready", tp::payload_format::json, "", 4, hsready));
    BOOST_TEST(run_until(f, [&] { return from_a.size() == 4; }));
    for (const std::uint32_t refused : {1U, 3U}) {
        if (BOOST_TEST(from_a.contains(refused))) {
            BOOST_TEST(from_a.at(refused).kind == tp::frame_kind::rejected);
            BOOST_TEST_EQ(from_a.at(refused).reason, "payload exceeds receiver size limit");
        }
    }
    for (const std::uint32_t taken : {2U, 4U}) {
        if (BOOST_TEST(from_a.contains(taken))) {
            BOOST_TEST(from_a.at(taken).kind == tp::frame_kind::accepted);
        }
    }
    BOOST_TEST_EQ(a.peers().size(), 1U);
}

/** The JSON of an inline `@_hsdata` frame, a step of the identity handshake; empty for another
 * frame. */
std::string handshake_step(std::span<const std::uint8_t> frame) {
    const auto decoded = trystero::protocol::decode_action(frame);
    const auto* p =
        decoded.frame ? std::get_if<trystero::protocol::proposal>(&*decoded.frame) : nullptr;
    if (p == nullptr || p->kind != trystero::protocol::frame_kind::inline_payload ||
        p->name != "@_hsdata") {
        return {};
    }
    return {reinterpret_cast<const char*>(p->payload.data()), p->payload.size()};
}

/** The number of a step's JSON, its `__trystero_id`, as the port writes it first; 0 for none. */
int step_number(std::string_view json) {
    constexpr std::string_view head = R"({"__trystero_id":)";
    return json.starts_with(head) && json.size() > head.size() ? json[head.size()] - '0' : 0;
}

/** The inline `@_hsdata` frame of a step's JSON, as a room sends it. */
std::vector<std::uint8_t> step_frame(std::string_view json) {
    const auto frame = trystero::protocol::encode_inline(
        "@_hsdata", trystero::protocol::payload_format::json, "",
        std::span(reinterpret_cast<const std::uint8_t*>(json.data()), json.size()));
    return {frame.begin(), frame.end()};
}

/** The DTLS fingerprints of the description the memory connection `pc` writes, as a room reads
 * them. */
std::pmr::vector<std::pmr::string> dtls_of_connection(std::size_t pc) {
    auto* const memory = std::pmr::get_default_resource();
    return trystero::protocol::dtls_fingerprints(trystero::test::memory_description(pc, memory),
                                                 memory);
}

/** `text` with its first `from` replaced by `to`; the case ends when `from` is not in it. */
std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    webcpp::test::require(BOOST_TEST(at != std::string::npos));
    return text.replace(at, from.size(), to);
}

// Two peers run 0.27.0's identity handshake, and each joins the other's room under the user it
// proved, the other app's `user_id()`. A, whose id is lower, sends steps 1 and 3, B step 2; each
// step claims the fingerprints of the description its sender wrote, which its receiver applied
// (design D20), and its sender's ids and keys. Each run draws its nonces afresh from the backend's
// randomness: a second meeting's are new, and none is the UUID of zero bytes.
void peers_verify_each_other_and_join_with_their_user_ids() {
    namespace tp = trystero::protocol;
    counting_resource a_memory;  // first: A's connection ends are made in it, which names them
    fixture f{true};

    struct sent_step {
        bool from_a;
        std::size_t from;
        std::size_t to;
        std::string json;
    };

    std::vector<sent_step> steps;
    f.net.filter = [&](std::size_t from, std::size_t to, std::span<const std::uint8_t> frame) {
        if (auto json = handshake_step(frame); !json.empty()) {
            steps.push_back({end_in(f, from, &a_memory), from, to, std::move(json)});
        }
        return trystero::test::memory_network::verdict::deliver;
    };
    const auto labels = ordered_labels(2, "verify");  // A's id below B's: A initiates
    auto a_config = app_config(labels[0]);
    a_config.memory = &a_memory;
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(a_config));
    auto a = join(a_app, "verify");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[1]));
    std::optional<trystero::room> b(join(b_app, "verify"));
    std::vector<std::string> a_joins;
    a.on_peer_join([&](const trystero::peer_event& e) {
        a_joins.push_back(std::string(e.peer_id) + " " + std::string(e.user_id));
    });
    std::vector<std::string> b_joins;
    b->on_peer_join([&](const trystero::peer_event& e) {
        b_joins.push_back(std::string(e.peer_id) + " " + std::string(e.user_id));
    });
    std::vector<std::string> failures;
    const auto record_failure = [&](const trystero::room_error& e) {
        failures.push_back(std::string(e.message));
    };
    a.on_error(record_failure);
    b->on_error(record_failure);
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a_joins.size() == 1 && b_joins.size() == 1; })));
    const std::string_view a_id = a_app.self_id();
    const std::string_view b_id = b_app.self_id();
    const std::string a_user(a_app.user_id());
    const std::string b_user(b_app.user_id());
    BOOST_TEST_NE(a_user, b_user);
    BOOST_TEST_EQ(a_joins.front(), std::string(b_id) + " " + b_user);
    BOOST_TEST_EQ(b_joins.front(), std::string(a_id) + " " + a_user);
    const auto a_peers = a.peers();
    const auto b_peers = b->peers();
    webcpp::test::require(BOOST_TEST(a_peers.contains(b_id) && b_peers.contains(a_id)));
    BOOST_TEST_EQ(std::string_view(a_peers.find(b_id)->second.user_id), b_user);
    BOOST_TEST_EQ(std::string_view(b_peers.find(a_id)->second.user_id), a_user);
    if (const auto users = a.users(); BOOST_TEST_EQ(users.size(), 1U)) {
        BOOST_TEST_EQ(std::string_view(users.begin()->first), b_user);
    }
    BOOST_TEST(failures.empty());

    // Steps 1 and 3 from A, 2 from B, each claiming its sender's keys and fingerprints.
    webcpp::test::require(BOOST_TEST_EQ(steps.size(), 3U));
    const auto& impl_a = trystero::detail::app_access::impl(a_app);
    const auto& impl_b = trystero::detail::app_access::impl(b_app);
    for (int n = 1; n <= 3; ++n) {
        const auto& step = steps.at(static_cast<std::size_t>(n - 1));
        BOOST_TEST_EQ(step_number(step.json), n);
        BOOST_TEST_EQ(step.from_a, n != 2);
        if (n == 3) {
            continue;
        }
        const auto sender = dtls_of_connection(step.from);
        const auto receiver = dtls_of_connection(step.to);
        webcpp::test::require(BOOST_TEST_EQ(sender.size(), 1U));
        BOOST_TEST_NE(sender.front(), receiver.front());
        const auto read = tp::read_handshake_step(step.json, n, sender, receiver,
                                                  std::pmr::get_default_resource());
        webcpp::test::require(BOOST_TEST(read.claims.has_value()));
        const auto& claims = *read.claims;
        BOOST_TEST_ALL_EQ(claims.dtls.begin(), claims.dtls.end(), sender.begin(), sender.end());
        const auto& by = n == 1 ? impl_a : impl_b;
        BOOST_TEST_EQ(claims.user_id, by.user_id());
        BOOST_TEST_EQ(claims.endpoint_key, by.endpoint_key());
        BOOST_TEST_EQ(claims.user_key, by.user().public_key());
    }

    // B meets A again: the second run's nonces are new.
    const auto nonce_of = [](const std::string& json) {
        const auto read = tp::read_handshake_step(json, step_number(json), {}, {},
                                                  std::pmr::get_default_resource());
        return read.claims ? std::string(read.claims->nonce) : std::string();
    };
    b.reset();
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return a.peers().empty(); })));
    b.emplace(join(b_app, "verify"));
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return a_joins.size() == 2; })));
    std::vector<std::string> nonces;
    for (const auto& step : steps) {
        if (step_number(step.json) != 3) {
            nonces.push_back(nonce_of(step.json));
        }
    }
    webcpp::test::require(BOOST_TEST_EQ(nonces.size(), 4U));
    const auto zero = tp::uuid_v4({});
    for (const auto& nonce : nonces) {
        BOOST_TEST_EQ(nonce.size(), tp::uuid_text_size);
        BOOST_TEST_NE(nonce, std::string(zero.data(), zero.size()));
    }
    std::ranges::sort(nonces);
    BOOST_TEST(std::ranges::adjacent_find(nonces) == nonces.end());
    BOOST_TEST(failures.empty());
}

/**
 * A, whose id is lower, initiates; its step 1 to B is dropped, and `forge` of it is injected at
 * B's end in its place. B reports handshake_failed once, with `reason` and A's id, and tells A to
 * leave; A reports nothing, and neither side reports a join. Once A's room is gone, B reports
 * nothing more: the failed run's timer went with it.
 */
void a_forged_step_1_fails_with(
    const std::function<std::string(const std::string& genuine, const trystero::test::app& a,
                                    const trystero::test::app& b)>& forge,
    std::string_view reason) {
    counting_resource a_memory;
    fixture f{true};
    std::optional<std::pair<std::size_t, std::string>> dropped;  // B's end, and the step
    f.net.filter = [&](std::size_t from, std::size_t to, std::span<const std::uint8_t> frame) {
        const auto json = handshake_step(frame);
        if (end_in(f, from, &a_memory) && step_number(json) == 1) {
            if (!dropped) {
                dropped.emplace(to, json);
            }
            return trystero::test::memory_network::verdict::drop;
        }
        return trystero::test::memory_network::verdict::deliver;
    };
    const auto labels = ordered_labels(2, "forge");
    auto a_config = app_config(labels[0]);
    a_config.memory = &a_memory;
    trystero::test::app a_app(f.io.get_executor(), f.net, std::move(a_config));
    std::optional<trystero::room> a(join(a_app, "forge"));
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config(labels[1]));
    auto b = join(b_app, "forge");
    std::vector<reported_error> a_reports;
    a->on_error(record_errors(f, a_reports, trystero::error::handshake_failed));
    std::vector<reported_error> b_reports;
    b.on_error(record_errors(f, b_reports, trystero::error::handshake_failed));
    std::size_t joins = 0;
    a->on_peer_join([&](const trystero::peer_event&) { ++joins; });
    b.on_peer_join([&](const trystero::peer_event&) { ++joins; });
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return dropped.has_value(); })));
    f.net.inject(dropped->first, step_frame(forge(dropped->second, a_app, b_app)));
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return !b_reports.empty(); })));
    trystero::test::advance(f, 1000ms);
    if (BOOST_TEST_EQ(b_reports.size(), 1U)) {
        const auto& e = b_reports.front();
        BOOST_TEST_EQ(trystero::room_error_code_name(e.code), "handshake-failed");
        BOOST_TEST_EQ(e.message, reason);
        BOOST_TEST_EQ(e.peer_id, a_app.self_id());
        BOOST_TEST_EQ(e.room_id, "forge");
    }
    BOOST_TEST(a_reports.empty());
    BOOST_TEST_EQ(joins, 0U);
    BOOST_TEST(a->peers().empty());
    BOOST_TEST(b.peers().empty());
    a.reset();
    trystero::test::advance(f, 15'000ms);
    BOOST_TEST_EQ(b_reports.size(), 1U);
    BOOST_TEST_EQ(joins, 0U);
}

// A step 1 that claims B's user id with A's own keys fails B's handshake as 0.27.0's
// verifyUserIdKey fails it: "peer identity verification failed".
void a_forged_step_fails_the_handshake() {
    a_forged_step_1_fails_with(
        [](const std::string& genuine, const trystero::test::app& a, const trystero::test::app& b) {
            return replaced(genuine, a.user_id(), b.user_id());
        },
        trystero::protocol::handshake_reason::identity);
}

// A step 1 that claims fingerprints other than those of the description B applied fails B's
// handshake, as a relay-side man in the middle's would: "DTLS fingerprint mismatch".
void another_fingerprint_fails_the_handshake() {
    a_forged_step_1_fails_with(
        [](const std::string& genuine, const trystero::test::app&, const trystero::test::app&) {
            const auto open = genuine.find(R"("dtls":[)");
            const auto close = genuine.find(']', open);
            webcpp::test::require(
                BOOST_TEST(open != std::string::npos && close != std::string::npos));
            std::string out = genuine.substr(0, open);
            out += R"("dtls":["sha-256 00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff:00:11:22:)"
                   R"(33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff"])";
            out += genuine.substr(close + 1);
            return out;
        },
        trystero::protocol::handshake_reason::dtls_mismatch);
}

// B leaves while the step 2 it sent is held on its way to A, its handshake waiting for step 3: B
// reports nothing after the leave, through its farewell and past the handshake's 10 s, and once
// its room is gone nothing runs on it when the held step is released toward A. No one joins.
void leaving_during_a_handshake_reports_nothing() {
    counting_resource b_memory;
    fixture f{true};
    bool holding = true;
    std::optional<std::size_t> held_at;  // A's end
    f.net.filter = [&](std::size_t from, std::size_t to, std::span<const std::uint8_t> frame) {
        if (holding && end_in(f, from, &b_memory) && step_number(handshake_step(frame)) == 2) {
            held_at = to;
            return trystero::test::memory_network::verdict::hold;
        }
        return trystero::test::memory_network::verdict::deliver;
    };
    const auto labels = ordered_labels(2, "midway");
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config(labels[0]));
    auto a = join(a_app, "midway");
    auto b_config = app_config(labels[1]);
    b_config.memory = &b_memory;
    trystero::test::app b_app(f.io.get_executor(), f.net, std::move(b_config));
    std::optional<trystero::room> b(join(b_app, "midway"));
    std::size_t joins = 0;
    a.on_peer_join([&](const trystero::peer_event&) { ++joins; });
    b->on_peer_join([&](const trystero::peer_event&) { ++joins; });
    bool left = false;
    std::size_t b_after_leave = 0;
    b->on_error([&](const trystero::room_error&) { b_after_leave += left ? 1 : 0; });
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return held_at.has_value(); })));
    std::optional<boost::system::error_code> leave;
    left = true;
    b->async_leave([&](boost::system::error_code ec) { leave = ec; });
    trystero::test::advance(f, 12'000ms);  // past the farewell and the handshake's 10 s
    BOOST_TEST(leave.has_value() && !*leave);
    b.reset();
    holding = false;
    f.net.release(*held_at);
    trystero::test::advance(f, 15'000ms);
    BOOST_TEST_EQ(b_after_leave, 0U);
    BOOST_TEST_EQ(joins, 0U);
    BOOST_TEST(a.peers().empty());
}

// The names of the codes are 0.27.0's RoomErrorCode, and every other code has none.
void error_names_match_trystero() {
    using trystero::error;
    const std::array<std::pair<error, std::string_view>, 9> named{
        {
            {error::room_full, "room-full"},
            {error::banned, "banned"},
            {error::relay_failed, "relay-failed"},
            {error::identity_failed, "identity-failed"},
            {error::plugin_failed, "plugin-failed"},
            {error::decrypt_failed, "decrypt-failed"},
            {error::peer_connect_failed, "peer-connect-failed"},
            {error::handshake_failed, "handshake-failed"},
            {error::peer_error, "peer-error"},
        },
    };
    for (const auto& [e, name] : named) {
        BOOST_TEST_EQ(trystero::room_error_code_name(e), name);
    }
    for (int value = 1; value <= static_cast<int>(error::invalid_user_identity); ++value) {
        const auto e = static_cast<error>(value);
        if (std::ranges::none_of(named, [&](const auto& n) { return n.first == e; })) {
            BOOST_TEST(trystero::room_error_code_name(e).empty());
        }
    }
    // A code of another category with a value of the same number is no room error of Trystero's.
    BOOST_TEST(
        trystero::room_error_code_name(boost::system::error_code(static_cast<int>(error::room_full),
                                                                 boost::system::generic_category()))
            .empty());
    BOOST_TEST(trystero::room_error_code_name({}).empty());
    // The messages are 0.27.0's, where it has one.
    BOOST_TEST_EQ(make_error_code(error::relay_failed).message(), "all signaling relays failed");
    BOOST_TEST_EQ(make_error_code(error::peer_connect_failed).message(),
                  "could not connect to peer after exchanging SDP");
    BOOST_TEST_EQ(make_error_code(error::handshake_failed).message(), "handshake failed");
}

// Messages that queued for async_receive when the room was left go with it, as 0.27.0 drops each
// peer's pending data at its leave: a listener added afterwards gets none of them.
void messages_waiting_at_a_leave_are_dropped() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("droppedAAAAAAAAAAAAA"));
    auto a = join(a_app, "dropped");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("droppedBBBBBBBBBBBBB"));
    auto b = join(b_app, "dropped");
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; })));
    auto send = a.make_action<std::string>("chat");
    auto receive = b.make_action<std::string>("chat");
    // A receive makes the action listen: what arrives once it completed queues for the next.
    std::vector<std::string> got;
    receive.async_receive(
        [&](boost::system::error_code ec, const std::string& text, const trystero::message_info&) {
            BOOST_TEST(!ec);
            got.push_back(text);
        });
    std::size_t sent = 0;
    for (int i = 0; i < 3; ++i) {
        send.async_send("waiting " + std::to_string(i), [&](boost::system::error_code ec) {
            BOOST_TEST(!ec);
            ++sent;
        });
    }
    BOOST_TEST(run_until(f, [&] { return sent == 3 && got.size() == 1; }));
    run_until(f, [] { return false; }, 100ms);  // the others arrive, and queue
    bool left = false;
    b.async_leave([&](boost::system::error_code) { left = true; });
    BOOST_TEST(run_until(f, [&] { return left; }));
    const auto late = receive.on_message(
        [&](const std::string& text, const trystero::message_info&) { got.push_back(text); });
    run_until(f, [] { return false; }, 100ms);
    BOOST_TEST_EQ(got.size(), 1U);
    bool received = false;
    receive.async_receive(
        [&](boost::system::error_code ec, const std::string&, const trystero::message_info&) {
            BOOST_TEST(ec == trystero::error::room_left);
            received = true;
        });
    BOOST_TEST(run_until(f, [&] { return received; }));
}

// A message posted to its listener before the room ended, whose listener is gone once the post
// runs, goes with the room as well: a progress listener removes the message listener and drops
// the room's handle, which ends it at once, before the message's post runs; a listener added
// afterwards gets nothing.
void a_message_posted_before_the_room_ended_is_dropped() {
    fixture f{true};
    trystero::test::app a_app(f.io.get_executor(), f.net, app_config("postedAAAAAAAAAAAAAA"));
    auto a = join(a_app, "posted");
    trystero::test::app b_app(f.io.get_executor(), f.net, app_config("postedBBBBBBBBBBBBBB"));
    std::optional<trystero::room> b(join(b_app, "posted"));
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b->peers().size() == 1; })));
    auto send = a.make_action<std::string>("chat");
    auto receive = b->make_action<std::string>("chat");
    std::vector<std::string> got;
    auto first = receive.on_message(
        [&](const std::string& text, const trystero::message_info&) { got.push_back(text); });
    bool ended = false;
    receive.on_receive_progress([&](double share, const trystero::progress_info&) {
        if (share == 1.0 && !ended) {
            ended = true;
            first.unsubscribe();
            b.reset();
        }
    });
    send.async_send("posted", [](boost::system::error_code) {});
    BOOST_TEST(run_until(f, [&] { return ended; }));
    run_until(f, [] { return false; }, 100ms);
    const auto late = receive.on_message(
        [&](const std::string& text, const trystero::message_info&) { got.push_back(text); });
    run_until(f, [] { return false; }, 100ms);
    BOOST_TEST(got.empty());
}

void the_errors_have_their_values() {
    // wrong_password and handshake_timed_out went with 0.26.0's password handshake: a handshake's
    // failures are handshake_failed, as 0.27.0 reports them.
    BOOST_TEST_EQ(static_cast<int>(trystero::error::handshake_failed), 9);
    BOOST_TEST_EQ(static_cast<int>(trystero::error::already_joined), 12);
    BOOST_TEST_EQ(make_error_code(trystero::error::already_joined).message(),
                  "the app has already joined a room of that id");
    BOOST_TEST_EQ(make_error_code(trystero::error::invalid_configuration).message(),
                  "the configuration is unusable");
    // After already_joined, in 0.27.0's order of room errors, with its messages: too_many_rooms,
    // which went with the shared connection, no longer comes between.
    BOOST_TEST_EQ(static_cast<int>(trystero::error::room_full), 13);
    BOOST_TEST_EQ(static_cast<int>(trystero::error::request_timed_out), 25);
    BOOST_TEST_EQ(make_error_code(trystero::error::room_full).message(), "room full");
    BOOST_TEST_EQ(make_error_code(trystero::error::banned).message(), "peer banned");
    BOOST_TEST_EQ(make_error_code(trystero::error::decrypt_failed).message(),
                  "failed to decrypt offer or answer (incorrect password or corrupted signal)");
    BOOST_TEST_EQ(make_error_code(trystero::error::action_aborted).message(), "operation aborted");
    // Appended after them: 0.27.0 throws it.
    BOOST_TEST_EQ(static_cast<int>(trystero::error::action_redefined), 26);
    BOOST_TEST_EQ(make_error_code(trystero::error::action_redefined).message(),
                  "action type cannot be redefined");
    BOOST_TEST_EQ(static_cast<int>(trystero::error::invalid_user_identity), 27);
    BOOST_TEST_EQ(make_error_code(trystero::error::invalid_user_identity).message(),
                  "invalid user identity key material; expected exported userKey or P-256 JWK");
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!BOOST_TEST_EQ(argc, 2)) {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
            << "usage: room_test <fixtures/expected/trystero-0.27.0.json>\n";
        return webcpp::test::exit_status(boost::report_errors());
    }
    {
        const std::ifstream file(argv[1]);
        std::stringstream text;
        text << file.rdbuf();
        auto loaded = webcpp::document::parse(text.str());
        if (!BOOST_TEST(loaded.has_value())) {
            return webcpp::test::exit_status(boost::report_errors());
        }
        vectors().emplace(std::move(*loaded));
    }
    // Some forty cases of two or three apps each: under Asyncify a debug build takes 54 s of the
    // processor, more than a third of node's default bound of 120 s, and 74 to 168 s on a loaded
    // machine, past it (measured on 2026-11-15 with 18 cores at load averages of 50 to 230; under
    // JSPI 34 s, and 44 to 51 s).
    webcpp::test::bound(std::chrono::seconds(300));
    // Each case in a frame of its own, which main's never holds (cases.hpp).
    using webcpp::test::run_case;
    // First, while the parser has run least: a WebAssembly engine compiles a function with its
    // baseline tier first, whose frames are the largest, and a peer's first payload meets it so.
    run_case(a_json_payload_nested_256_deep_is_delivered);
    run_case(two_peers_join_and_exchange_every_payload_format);
    run_case(large_payloads_travel_in_chunks_both_ways);
    run_case(a_wide_transfer_arrives_in_wide_chunks_in_any_order);
    run_case(a_wide_link_sends_wide_chunks);
    run_case(a_refused_chunk_ends_the_transfer_with_the_peer);
    run_case(a_narrow_link_sends_narrow_chunks);
    run_case(the_wire_keeps_0_27_0s_bounds);
    run_case(frames_of_an_action_not_made_are_dropped_or_rejected);
    run_case(an_inline_frame_above_the_receive_bound_is_dropped);
    run_case(a_control_frame_into_a_full_channel_drops_the_peer);
    run_case(ping_measures_a_round_trip);
    run_case(two_passwords_never_meet);
    run_case(a_handshake_that_never_completes_fails_after_10_s);
    run_case(ready_frames_carry_an_empty_text);
    run_case(sends_inside_on_peer_join_follow_the_ready_frame);
    run_case(handshake_messages_keep_0_27_0s_bounds);
    run_case(peers_verify_each_other_and_join_with_their_user_ids);
    run_case(a_forged_step_fails_the_handshake);
    run_case(another_fingerprint_fails_the_handshake);
    run_case(leaving_during_a_handshake_reports_nothing);
    run_case(password_room_keys_complete_after_the_join_returns);
    run_case(rooms_of_one_password_derive_once_per_room);
    run_case(a_failed_derivation_is_retried);
    run_case(a_room_left_while_its_keys_derive_announces_nothing);
    run_case(the_room_keys_cache_keeps_64_rooms);
    run_case(leaving_is_seen_by_the_others_and_a_return_reconnects);
    run_case(moving_a_room_over_a_joined_one_leaves_it);
    run_case(ice_urls_are_checked_as_webrtc_checks_them);
    run_case(unusable_ice_servers_are_dropped_and_reported);
    run_case(a_refused_peer_connection_is_reported_and_tried_again);
    run_case(a_flood_of_announcements_is_bounded);
    run_case(long_peer_ids_are_bounded);
    run_case(a_refused_peer_is_logged_once);
    run_case(a_forged_answer_changes_nothing);
    run_case(forged_offers_make_no_connection);
    run_case(a_flood_of_offers_at_one_instant_is_bounded);
    run_case(offers_in_one_name_are_answered_one_at_a_time);
    run_case(signals_that_do_not_open_are_reported_once_per_peer_and_stage);
    run_case(sealed_offers_that_fail_at_once_are_bounded);
    run_case(announcements_with_forged_answers_are_bounded);
    run_case(the_replay_cache_remembers_the_last_ones);
    run_case(recent_event_ids_drop_a_repeat_and_forget_the_oldest);
    run_case(event_ids_other_than_nip01_ones_are_never_remembered);
    run_case(the_event_id_hash_is_keyed);
    run_case(a_strategy_draws_the_key_of_its_event_ids);
    run_case(a_forged_event_under_a_genuine_id_hides_nothing);
    run_case(each_room_signs_with_its_own_key);
    run_case(a_room_subscribes_its_self_topic_before_its_root_topic);
    run_case(classes_over_a_pmr_circular_buffer_do_not_move);
    run_case(a_replayed_answer_does_not_open);
    run_case(a_replayed_offer_is_refused);
    run_case(a_reflected_offer_is_refused_as_an_answer);
    run_case(a_reflected_offer_is_refused_as_an_offer);
    run_case(a_reflected_answer_does_not_open_as_an_answer);
    run_case(a_relay_that_closes_at_once_is_tried_less_and_less_often);
    run_case(counter_announcements_are_paced);
    run_case(three_peers_form_a_mesh);
    run_case(rooms_of_one_app_have_their_own_connections);
    run_case(a_room_keeps_one_connection_per_peer);
    run_case(a_leaving_room_negotiates_nothing);
    run_case(leaving_one_room_keeps_the_other_rooms_connection);
    run_case(frames_travel_bare);
    run_case(an_app_joins_more_than_64_rooms);
    run_case(async_receive_works_in_a_coroutine);
    run_case(json_payloads_are_read_in_the_rooms_resource_up_to_its_bound);
    run_case(a_rejected_document_receive_holds_nothing);
    run_case(view_handlers_read_the_frame_without_copying);
    run_case(a_rate_limited_relay_does_not_stop_discovery);
    run_case(replay_reports_current_peers);
    run_case(second_on_request_is_refused);
    run_case(a_request_is_answered_or_refused);
    run_case(messages_wait_for_a_listener);
    run_case(a_receive_gate_decides_and_progress_is_reported);
    run_case(a_listener_may_end_its_room_while_it_is_reported);
    run_case(a_subscription_outlives_its_room);
    run_case(a_join_posted_before_the_leave_is_not_reported);
    run_case(a_gate_may_end_its_room_while_waiting_messages_are_delivered);
    run_case(leaves_are_reported_peer_then_user);
    run_case(send_to_a_user_reaches_each_of_its_peers);
    run_case(a_user_leaves_with_its_last_peer);
    run_case(a_peer_of_its_own_user_is_no_other_user);
    run_case(send_to_an_absent_target_fails);
    run_case(self_names_the_room);
    run_case(an_apps_ids_derive_from_its_keys);
    run_case(two_apps_of_one_imported_identity_share_a_user_id);
    run_case(self_reports_the_identity);
    run_case(an_endpoint_that_cannot_be_made_reports_identity_failed);
    run_case(a_user_key_that_cannot_be_made_reports_identity_failed);
    run_case(a_request_is_refused_by_a_gone_listener_or_the_gate);
    run_case(a_request_fails_when_its_peer_or_its_room_goes);
    run_case(an_action_name_keeps_its_first_kind);
    run_case(the_errors_have_their_values);
    run_case(decrypt_failed_is_reported_once_a_minute);
    run_case(relay_failed_when_every_relay_fails);
    run_case(peer_connect_failed_says_whether_turn_was_configured);
    run_case(error_names_match_trystero);
    run_case(messages_waiting_at_a_leave_are_dropped);
    run_case(a_message_posted_before_the_room_ended_is_dropped);
    run_case(offline_holds_and_online_reconnects);
    run_case(offline_holds_a_relay_waiting_to_reconnect);
    run_case(online_reconnects_a_backoff_relay_at_once);
    run_case(online_while_online_leaves_connected_relays_alone);
    run_case(automatic_is_online_natively);
    run_case(automatic_follows_the_pages_events);
    // Under Asyncify, a main that suspended loses what it returns: the status goes first.
    return webcpp::test::exit_status(boost::report_errors());
}
