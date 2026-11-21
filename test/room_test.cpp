// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests whole rooms of C++ peers in one process, over the memory backend of
// support/memory_backend.hpp. WEBCPP_TEST_TRYSTERO_VERBOSE in the environment prints each room's
// log on the standard error.

#include <webcpp/trystero/app.hpp>
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
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "cases.hpp"
#include "require.hpp"
#include "support/memory_backend.hpp"
#include "support/rooms.hpp"

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
using namespace std::chrono_literals;

namespace {

using trystero::test::advance;
using trystero::test::app_config;
using trystero::test::app_id;
using trystero::test::counting_resource;
using trystero::test::document_of;
using trystero::test::fixture;
using trystero::test::flooder;
using trystero::test::join;
using trystero::test::relay_a;
using trystero::test::relay_b;
using trystero::test::relayed_signal;
using trystero::test::run_until;

/** An app's state and a room of it, without their handles, for a case that reaches the app's
 * connections or calls the room as its app does. The case abandons the room before the fixture
 * goes. */
struct app_room {
    std::shared_ptr<trystero::detail::app_impl> app;
    std::shared_ptr<trystero::detail::room_impl> room;
};

app_room make_app_room(
    fixture& f, std::string_view self_id, std::string_view room_id,
    const std::function<void(trystero::app_config&, trystero::room_options&)>& edit = {}) {
    const std::pmr::polymorphic_allocator<> alloc(std::pmr::get_default_resource());
    trystero::app_config config{
        .app_id = app_id,
        .self_id = std::pmr::string(self_id),
        .relay_urls = {relay_a, relay_b},
    };
    trystero::room_options options;
    if (edit) {
        edit(config, options);
    }
    app_room out;
    out.app = std::allocate_shared<trystero::detail::app_impl>(alloc, f.io.get_executor(), f.net,
                                                               std::move(config));
    out.room = trystero::detail::room_impl::make(out.app, options, room_id);
    return out;
}

void two_peers_join_and_exchange_every_payload_format() {
    fixture f;
    // "A..." sorts below "z...", so A offers and z answers; the other test below swaps nothing but
    // the ids.
    trystero::app a_app(f.io.get_executor(), f.net, app_config("AAAAAAAAAAAAAAAAAAAA"));
    auto a = join(a_app, "lobby");
    trystero::app z_app(f.io.get_executor(), f.net, app_config("zzzzzzzzzzzzzzzzzzzz"));
    auto z = join(z_app, "lobby");
    std::vector<std::string> joined_a;
    std::vector<std::string> joined_z;
    a.on_peer_join([&](std::string_view p) { joined_a.emplace_back(p); });
    z.on_peer_join([&](std::string_view p) { joined_z.emplace_back(p); });

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

    BOOST_TEST(run_until(f.io, [&] { return joined_a.size() == 1 && joined_z.size() == 1; }));
    BOOST_TEST_EQ(joined_a.at(0), "zzzzzzzzzzzzzzzzzzzz");
    BOOST_TEST_EQ(joined_z.at(0), "AAAAAAAAAAAAAAAAAAAA");
    const auto peers = a.peers();
    const std::vector<trystero::peer_id> only_z{"zzzzzzzzzzzzzzzzzzzz"};
    BOOST_TEST_ALL_EQ(peers.begin(), peers.end(), only_z.begin(), only_z.end());

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

    BOOST_TEST(run_until(
        f.io, [&] { return chats_z.size() == 1 && drink.has_value() && pic.size() == 3; }));
    BOOST_TEST_EQ(sent.size(), 3U);
    for (const auto& ec : sent) {
        BOOST_TEST(!ec);
    }
    BOOST_TEST_EQ(chats_z.at(0).first, "hello");
    BOOST_TEST_EQ(chats_z.at(0).second.from, "AAAAAAAAAAAAAAAAAAAA");
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
    fixture f;
    trystero::app a_app(f.io.get_executor(), f.net, app_config("BBBBBBBBBBBBBBBBBBBB"));
    auto a = join(a_app, "files");
    trystero::app b_app(f.io.get_executor(), f.net, app_config("CCCCCCCCCCCCCCCCCCCC"));
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
    BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }));

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
    BOOST_TEST(run_until(f.io, [&] { return done == 2 && !got_a.empty() && !got_b.empty(); }));
    BOOST_TEST_ALL_EQ(got_b.begin(), got_b.end(), big.begin(), big.end());
    const trystero::bytes reversed(big.rbegin(), big.rend());
    BOOST_TEST_ALL_EQ(got_a.begin(), got_a.end(), reversed.begin(), reversed.end());
    BOOST_TEST_EQ(meta_b, R"({"name":"big.bin"})");
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
    };

    fixture f{true};
    std::shared_ptr<link> channel = std::make_shared<link>();
    std::set<std::string, std::less<>> receivers{"file"};
    std::vector<trystero::detail::incoming> delivered;
    std::vector<std::string> dropped;
    std::shared_ptr<trystero::detail::action_wire> wire;
    // The virtual clock starts with the network's first timer: one made here, so that the clock
    // the wire reads moves with advance() before the wire makes a timer of its own.
    std::shared_ptr<trystero::backend::timer> clock_start;

    wire_bench() {
        using trystero::detail::action_wire;
        const auto memory = std::pmr::get_default_resource();
        clock_start = f.net.make_timer(memory);
        wire = std::make_shared<action_wire>(
            f.net, "token", trystero::room_options{}.max_receive_bytes,
            action_wire::callbacks{
                .find_link = [this](std::string_view, bool) -> std::optional<action_wire::link> {
                    return action_wire::link{
                        std::weak_ptr<trystero::detail::channel_link>(channel)};
                },
                .peers = [memory](bool) { return std::pmr::vector<std::string_view>(memory); },
                .may_receive = [](std::string_view, std::string_view) { return true; },
                .has_receiver = [this](std::string_view name) { return receivers.contains(name); },
                .deliver =
                    [this](trystero::detail::incoming&& m) { delivered.push_back(std::move(m)); },
                .fail_peer = [this](std::string_view,
                                    std::string_view reason) { dropped.emplace_back(reason); },
            },
            memory);
    }

    /** Hands the wire an action frame of `peer`. */
    void receive(std::string_view peer, trystero::backend::bytes frame) {
        const auto view = std::span<const std::uint8_t>(frame);
        wire->handle(peer, std::move(frame), view);
    }

    /** The action frames the wire sent, out of their room envelopes. */
    [[nodiscard]] std::vector<trystero::protocol::decoded_action> sent() const {
        std::vector<trystero::protocol::decoded_action> out;
        for (const auto& frame : channel->sent) {
            const auto envelope = trystero::protocol::decode_envelope(frame);
            out.push_back(*trystero::protocol::decode_action(envelope->payload).frame);
        }
        return out;
    }
};

// A transfer offered with 0.27.0's wide flag arrives in chunks of 65,522 bytes, which may come in
// any order, each once; a chunk of another length, or one that came already, drops the peer.
void a_wide_transfer_arrives_in_wide_chunks_in_any_order() {
    namespace tp = trystero::protocol;
    trystero::bytes payload(200 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i * 31);
    }
    wire_bench b;
    b.receive("A", tp::encode_offer("file", tp::payload_format::binary, R"({"n":1})", 5,
                                    payload.size(), true));
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
    narrow.receive(
        "A", tp::encode_offer("file", tp::payload_format::binary, "", 6, payload.size(), true));
    narrow.receive("A", tp::encode_chunk(6, 0, std::span(payload).first(tp::chunk_size)));
    BOOST_TEST_EQ(narrow.dropped.size(), 1U);

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
// until the action can take them, a newer one dropping the oldest, for 10 s.
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
        for (std::size_t i = 0; i <= tp::max_pre_listener_messages; ++i) {
            b.receive("A", tp::encode_inline("later", tp::payload_format::binary, "",
                                             trystero::bytes{static_cast<std::uint8_t>(i)}));
        }
        b.receive("A",
                  tp::encode_inline("other", tp::payload_format::binary, "", trystero::bytes{99}));
        b.receivers.insert("later");
        b.wire->receiver_added();
        if (BOOST_TEST_EQ(b.delivered.size(), tp::max_pre_listener_messages)) {
            for (std::size_t i = 0; i < b.delivered.size(); ++i) {
                BOOST_TEST_EQ(b.delivered[i].payload()[0], i + 1);
            }
        }
        advance(b.f, 10'001ms);
        b.receivers.insert("other");
        b.wire->receiver_added();
        BOOST_TEST_EQ(b.delivered.size(), tp::max_pre_listener_messages);
    }
}

// A control frame goes out only while the channel buffers at most 1 MiB, as 0.27.0's
// sendControlFrame checks: past it, the peer is dropped.
void a_control_frame_into_a_full_channel_drops_the_peer() {
    namespace tp = trystero::protocol;
    wire_bench b;
    b.channel->buffered = tp::max_control_buffered_bytes + 1;
    b.receive("A", tp::encode_offer("file", tp::payload_format::binary, "", 1, 20000));
    BOOST_TEST(b.channel->sent.empty());
    if (BOOST_TEST_EQ(b.dropped.size(), 1U)) {
        BOOST_TEST_EQ(b.dropped[0], "unable to send action control");
    }
    wire_bench at_bound;
    at_bound.channel->buffered = tp::max_control_buffered_bytes;
    at_bound.receive("A", tp::encode_offer("file", tp::payload_format::binary, "", 1, 20000));
    BOOST_TEST_EQ(at_bound.channel->sent.size(), 1U);
    BOOST_TEST(at_bound.dropped.empty());
}

void ping_measures_a_round_trip() {
    fixture f;
    trystero::app a_app(f.io.get_executor(), f.net, app_config("DDDDDDDDDDDDDDDDDDDD"));
    auto a = join(a_app, "ping");
    trystero::app b_app(f.io.get_executor(), f.net, app_config("EEEEEEEEEEEEEEEEEEEE"));
    const auto b = join(b_app, "ping");
    BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1; }));
    std::optional<boost::system::error_code> result;
    a.async_ping(b.self_id(), [&](boost::system::error_code ec, std::chrono::milliseconds rtt) {
        result = ec;
        BOOST_TEST_GE(rtt.count(), 0);
    });
    BOOST_TEST(run_until(f.io, [&] { return result.has_value(); }));
    BOOST_TEST(!*result);
    std::optional<boost::system::error_code> missing;
    a.async_ping("nobody",
                 [&](boost::system::error_code ec, std::chrono::milliseconds) { missing = ec; });
    BOOST_TEST(run_until(f.io, [&] { return missing.has_value(); }));
    BOOST_TEST(*missing == trystero::error::no_such_peer);
}

void the_same_password_joins_and_another_does_not() {
    fixture f{true};
    trystero::app a_app(f.io.get_executor(), f.net, app_config("FFFFFFFFFFFFFFFFFFFF", "s3cret"));
    auto a = join(a_app, "private");
    trystero::app b_app(f.io.get_executor(), f.net, app_config("GGGGGGGGGGGGGGGGGGGG", "s3cret"));
    auto b = join(b_app, "private");
    BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }));

    // A and B, whose ids are lower, offer to the intruder, which cannot open their offers with its
    // room key, so it never connects. It reports both: the first at once, with its peer, and the
    // other with their number when the interval of its reports ends, 10 s later.
    std::vector<boost::system::error_code> errors;
    std::size_t refused = 0;
    trystero::app intruder_app(f.io.get_executor(), f.net,
                               app_config("HHHHHHHHHHHHHHHHHHHH", "guess"));
    auto intruder = join(intruder_app, "private");
    intruder.on_error(
        [&](std::string_view peer, boost::system::error_code ec, std::string_view what) {
            errors.push_back(ec);
            if (ec == trystero::error::wrong_password) {
                refused += peer.empty() ? std::stoul(std::string(what)) : 1;
            }
        });
    a.on_error([&](std::string_view, boost::system::error_code ec, std::string_view) {
        errors.push_back(ec);
    });
    BOOST_TEST(run_until(f, [&] { return refused == 2; }, 20'000ms));
    BOOST_TEST(intruder.peers().empty());
    BOOST_TEST_EQ(a.peers().size(), 1U);
    BOOST_TEST(!errors.empty());
    for (const auto& ec : errors) {
        BOOST_TEST(ec == trystero::error::wrong_password);
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
    const std::string a_id = "0aaaaaaaaaaaaaaaaaaa";
    const std::string b_id = "0bbbbbbbbbbbbbbbbbbb";
    announcement_watcher watcher(f, "keyed", {a_id, b_id});
    trystero::app a_app(f.io.get_executor(), f.net, app_config(a_id, "s3cret"));
    auto a = join(a_app, "keyed");
    BOOST_TEST_EQ(f.net.derivations, 1U);
    BOOST_TEST_EQ(f.net.held_derivations(), 1U);
    trystero::app b_app(f.io.get_executor(), f.net, app_config(b_id, "s3cret"));
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
    trystero::app a_app(f.io.get_executor(), f.net, app_config("0aaaaaaaaaaaaaaaaaaa", "s3cret"));
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

    trystero::app open_app(f.io.get_executor(), f.net, app_config("0ccccccccccccccccccc"));
    const auto open = join(open_app, "first");
    webcpp::test::require(BOOST_TEST(f.net.poll_ready()));
    BOOST_TEST_EQ(f.net.derivations, 2U);
}

// A derivation that fails, as WebCrypto's may: the room reports error::backend_failure through
// its error handler and announces nothing; joined again, the room derives again, the failure
// forgotten, and announces.
void a_failed_derivation_is_retried() {
    fixture f{true};
    const std::string a_id = "0aaaaaaaaaaaaaaaaaaa";
    announcement_watcher watcher(f, "failing", {a_id});
    f.net.fail_derivations = 1;
    trystero::app a_app(f.io.get_executor(), f.net, app_config(a_id, "s3cret"));
    std::vector<boost::system::error_code> errors;
    {
        auto a = join(a_app, "failing");
        a.on_error([&](std::string_view peer, boost::system::error_code ec, std::string_view) {
            BOOST_TEST(peer.empty());
            errors.push_back(ec);
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
// handler of the room runs, and nothing it allocated is touched (the hardened run of shared_test
// and the allocation test cover its memory; here the room's own resource is checked empty). A
// room of the same id joined again while the derivation is still held completes from it.
void a_room_left_while_its_keys_derive_announces_nothing() {
    counting_resource memory;  // first: the io_context's pending operations free into it
    fixture f{true};
    f.net.derive_delay = true;
    const std::string a_id = "0aaaaaaaaaaaaaaaaaaa";
    announcement_watcher watcher(f, "leaving", {a_id});
    std::size_t errors = 0;
    {
        auto c = app_config(a_id, "s3cret");
        c.memory = &memory;
        trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
        {
            auto a = join(a_app, "leaving");
            a.on_error([&errors](std::string_view, boost::system::error_code, std::string_view) {
                ++errors;
            });
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
        trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
        auto a = join(a_app, "leaving");
        a.on_error(
            [&errors](std::string_view, boost::system::error_code, std::string_view) { ++errors; });
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
        cache->get(room, [&keys, room](boost::system::error_code ec,
                                       const trystero::protocol::room_keys& k) {
            BOOST_TEST(!ec);
            std::array<std::uint8_t, 32> aes{};
            std::ranges::copy(k.aes.bytes(), aes.begin());
            keys[room].push_back(aes);
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
    fixture f;
    trystero::app a_app(f.io.get_executor(), f.net, app_config("IIIIIIIIIIIIIIIIIIII"));
    auto a = join(a_app, "leave");
    std::vector<std::string> left;
    std::vector<std::string> joined;
    a.on_peer_leave([&](std::string_view p) { left.emplace_back(p); });
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    {
        // B's app goes with its room, so that its return negotiates again: a living app would keep
        // the connection, idle, and bind the returning room over it.
        trystero::app b_app(f.io.get_executor(), f.net, app_config("JJJJJJJJJJJJJJJJJJJJ"));
        auto b = join(b_app, "leave");
        BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1; }));
        bool done = false;
        b.async_leave([&](boost::system::error_code ec) { done = !ec; });
        BOOST_TEST(run_until(f.io, [&] { return done && left.size() == 1; }));
        BOOST_TEST(a.peers().empty());
    }
    trystero::app b2_app(f.io.get_executor(), f.net, app_config("JJJJJJJJJJJJJJJJJJJJ"));
    const auto b2 = join(b2_app, "leave");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 2; }));
}

// A room moved over a joined one leaves it as destroying it does: the other peer sees it go, and
// its pending operations complete with room_left, while an action of it still holds its state.
void moving_a_room_over_a_joined_one_leaves_it() {
    fixture f;
    trystero::app a_app(f.io.get_executor(), f.net, app_config("KKKKKKKKKKKKKKKKKKKK"));
    auto a = join(a_app, "move");
    std::vector<std::string> left;
    std::vector<std::string> joined;
    a.on_peer_leave([&](std::string_view p) { left.emplace_back(p); });
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    trystero::app b_app(f.io.get_executor(), f.net, app_config("LLLLLLLLLLLLLLLLLLLL"));
    auto b = join(b_app, "move");
    auto pending = b.make_action<std::string>("pending");
    std::optional<boost::system::error_code> received;
    pending.async_receive([&](boost::system::error_code ec, const std::string& /*text*/,
                              const trystero::message_info& /*info*/) { received = ec; });
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1; }));

    b = join(b_app, "elsewhere");
    BOOST_TEST(run_until(f.io, [&] { return left.size() == 1 && received.has_value(); }));
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
    fixture f;
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
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
    auto a = join(a_app, "ice");
    std::vector<std::pair<std::string, boost::system::error_code>> errors;
    a_app.on_error([&](std::string_view peer, boost::system::error_code ec,
                       std::string_view /*what*/) { errors.emplace_back(peer, ec); });
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    trystero::app b_app(f.io.get_executor(), f.net, app_config("OOOOOOOOOOOOOOOOOOOO"));
    const auto b = join(b_app, "ice");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1 && errors.size() == 2; }));
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
    fixture f;
    f.net.refuse_peer_connections = 1;
    trystero::app a_app(f.io.get_executor(), f.net, app_config("PPPPPPPPPPPPPPPPPPPP"));
    auto a = join(a_app, "refused");
    trystero::app b_app(f.io.get_executor(), f.net, app_config("QQQQQQQQQQQQQQQQQQQQ"));
    auto b = join(b_app, "refused");
    std::vector<boost::system::error_code> errors;
    const auto on_error = [&](std::string_view /*peer*/, boost::system::error_code ec,
                              std::string_view /*what*/) { errors.push_back(ec); };
    a.on_error(on_error);
    b.on_error(on_error);
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1; }, 30s));
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
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
    auto a = join(a_app, "flood");
    trystero::app b_app(f.io.get_executor(), f.net, app_config("BBBBBBBBBBBBBBBBBBBB"));
    auto b = join(b_app, "flood");
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
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
                advance(f, minute);
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
    trystero::app late_app(f.io.get_executor(), f.net, app_config("CCCCCCCCCCCCCCCCCCCC"));
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
// B join, and A reports the failed open at most once for B.
void a_forged_answer_changes_nothing() {
    fixture f;
    const std::string_view a_id = "AAAAAAAAAAAAAAAAAAAA";
    const std::string_view b_id = "BBBBBBBBBBBBBBBBBBBB";
    trystero::app a_app(f.io.get_executor(), f.net, app_config(a_id));
    auto a = join(a_app, "forged");
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    std::vector<boost::system::error_code> errors;
    a.on_error([&](std::string_view /*peer*/, boost::system::error_code ec,
                   std::string_view /*what*/) { errors.push_back(ec); });
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
    trystero::app b_app(f.io.get_executor(), f.net, app_config(b_id));
    const auto b = join(b_app, "forged");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1; }));
    BOOST_TEST_GE(forged, 1U);
    BOOST_TEST_LE(errors.size(), 1U);
}

// Offers from fresh ids that are not sealed with the room key make no connection: a negotiation
// changes only once its offer opened. The case counts the offers the room could not open, each
// logged once, and looks at the connections once it has every one.
void forged_offers_make_no_connection() {
    constexpr std::size_t flood = 200;
    fixture f{true};
    auto c = app_config("zzzzzzzzzzzzzzzzzzzz");
    std::size_t refused = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refused += text.starts_with("could not open the offer from 0offer") ? 1 : 0;
    };
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
    const auto a = join(a_app, "offers");
    flooder attacker(f, "offers");
    const auto a_topic = trystero::protocol::peer_topic(app_id, "offers", "zzzzzzzzzzzzzzzzzzzz");
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
    const std::string_view z_id = "zzzzzzzzzzzzzzzzzzzz";
    auto c = app_config(z_id);
    std::size_t not_opened = 0;
    std::size_t refused = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        not_opened += text.starts_with("could not open the offer from 0offer") ? 1 : 0;
        refused += text.starts_with("refusing 0offer") ? 1 : 0;
    };
    trystero::app z_app(f.io.get_executor(), f.net, std::move(c));
    auto z = join(z_app, "instant");
    std::vector<std::string> joined;
    z.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
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

    trystero::app a_app(f.io.get_executor(), f.net, app_config("0aaaaaaaaaaaaaaaaaaa"));
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
    const std::string_view z_id = "zzzzzzzzzzzzzzzzzzzz";
    auto c = app_config(z_id);
    c.memory = &memory;
    trystero::app z_app(f.io.get_executor(), f.net, std::move(c));
    const auto z = join(z_app, "one");
    const auto z_topic = trystero::protocol::peer_topic(app_id, "one", z_id);
    // Two sockets in the name of 0name, one on each relay, which see the room's answers to it.
    std::set<std::string> answered_through;
    flooder on_a(f, "one");
    flooder on_b(f, "one", relay_b);
    for (auto [socket, relay] : {std::pair{&on_a, relay_a}, std::pair{&on_b, relay_b}}) {
        socket->on_text = [&answered_through, relay, z_id](std::string_view text) {
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

// Unsealed offers from 2000 fresh ids, which anyone who reads the relays can send: the room reports
// the first at once, with its peer, and the rest together, at most once per interval, with no peer
// and their number, so that the application is not handed one report per id. On the virtual
// clock, the flood spans two and a half intervals.
void signals_that_do_not_open_are_reported_once_per_interval() {
    constexpr std::size_t flood = 2000;
    fixture f{true};
    const std::string a_id = "zzzzzzzzzzzzzzzzzzzz";
    trystero::app a_app(f.io.get_executor(), f.net, app_config(a_id));
    auto a = join(a_app, "reports");

    struct report {
        std::string peer;
        std::string what;
    };

    std::vector<report> reports;
    a.on_error([&](std::string_view peer, boost::system::error_code ec, std::string_view what) {
        if (ec == trystero::error::wrong_password) {
            reports.push_back({.peer = std::string(peer), .what = std::string(what)});
        }
    });
    flooder attacker(f, "reports");
    const auto a_topic = trystero::protocol::peer_topic(app_id, "reports", a_id);
    const auto started = f.net.steady_now();
    const auto before = f.net.relay(relay_a).events_seen;
    // Paced, twenty at a time: every twenty, everything ready runs, the relay and the room, and
    // then the clock moves 250 ms, 25 s for the whole flood.
    for (std::size_t i = 0; i < flood; ++i) {
        attacker.publish(a_topic, {
                                      .peer_id = "0garbage" + std::to_string(i),
                                      .offer_id = "o",
                                      .offer = "not a sealed offer",
                                  });
        if (i % 20 == 19) {
            advance(f, 250ms);
        }
    }
    BOOST_TEST(run_until(f, [&] { return f.net.relay(relay_a).events_seen >= before + flood; }));
    // Every failed open is counted once its interval ends.
    const auto counted = [&] {
        std::size_t sum = 0;
        for (const auto& r : reports) {
            sum += r.peer.empty() ? std::stoul(r.what) : 1;
        }
        return sum;
    };
    BOOST_TEST(run_until(f, [&] { return counted() == flood; }));
    const auto elapsed = f.net.steady_now() - started;
    BOOST_TEST_LE(reports.size(), 2 + static_cast<std::size_t>(elapsed / 10s));
    if (BOOST_TEST(!reports.empty())) {
        BOOST_TEST_EQ(reports.front().peer, "0garbage0");
    }
}

// A room member, who holds the key, sends offers from fresh ids whose SDP every connection refuses:
// each ends its negotiation at once and frees its place, but the room makes no more than
// max_connections_per_window connections in the window, and refuses the rest.
void sealed_offers_that_fail_at_once_are_bounded() {
    constexpr std::size_t flood = 500;
    constexpr std::size_t bound = trystero::detail::signaling::max_connections_per_window;
    fixture f;
    auto c = app_config("zzzzzzzzzzzzzzzzzzzz");
    std::size_t refusals = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refusals +=
            text.find(" connections were made in the last ") != std::string_view::npos ? 1 : 0;
    };
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
    const auto a = join(a_app, "churn");
    flooder member(f, "churn");
    const auto a_topic = trystero::protocol::peer_topic(app_id, "churn", "zzzzzzzzzzzzzzzzzzzz");
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
        if (!BOOST_TEST(run_until(f.io, [&] { return f.net.made_connections + refusals > i; }))) {
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
    fixture f;
    const std::string_view a_id = "00000000000000000000";
    auto c = app_config(a_id);
    std::size_t refusals = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refusals += text.starts_with("refusing ") ? 1 : 0;
    };
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
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
    BOOST_TEST(run_until(f.io, [&] { return forged >= bound && refusals >= flood - bound; }));
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
    trystero::app a_app(f.io.get_executor(), f.net, app_config("SSSSSSSSSSSSSSSSSSSS"));
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
        auto two = join(a_app, "two");
        webcpp::test::require(BOOST_TEST(run_until(f, announced_twice(one_topic))));
        webcpp::test::require(BOOST_TEST(run_until(f, announced_twice(two_topic))));
        for (const auto& topic : {one_topic, two_topic}) {
            std::set<std::string> room_keys;
            for (const auto url : {relay_a, relay_b}) {
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
    BOOST_TEST(keys.find(signers.front()) == keys.end());
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
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
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
    BOOST_TEST(!std::is_move_constructible_v<trystero::detail::recent_challenges>);
    BOOST_TEST(!std::is_move_assignable_v<trystero::detail::recent_challenges>);
    BOOST_TEST(!std::is_move_constructible_v<trystero::detail::replay_cache>);
    BOOST_TEST(!std::is_move_assignable_v<trystero::detail::replay_cache>);
}

/** Joins B to A, records what `record` keeps of the relays meanwhile, and has B leave; B's app goes
 * with it, so that B's return negotiates again. */
void join_and_leave(fixture& f, std::string_view b_id, std::string_view room_id,
                    const std::vector<std::string>& joined, const std::vector<std::string>& left,
                    const std::function<bool()>& recorded) {
    trystero::app b_app(f.io.get_executor(), f.net, app_config(b_id));
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
    const std::string a_id = "00000000000000000000";
    const std::string b_id = "BBBBBBBBBBBBBBBBBBBB";
    trystero::app a_app(f.io.get_executor(), f.net, app_config(a_id));
    auto a = join(a_app, "replay");
    std::vector<std::string> joined;
    std::vector<std::string> left;
    std::vector<boost::system::error_code> errors;
    bool armed = false;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    a.on_peer_leave([&](std::string_view p) {
        left.emplace_back(p);
        armed = true;
    });
    a.on_error([&](std::string_view /*peer*/, boost::system::error_code ec,
                   std::string_view /*what*/) { errors.push_back(ec); });
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
    join_and_leave(f, b_id, "replay", joined, left, [&] { return !recorded.empty(); });
    trystero::app b_app(f.io.get_executor(), f.net, app_config(b_id));
    const auto b = join(b_app, "replay");
    BOOST_TEST(run_until(f, [&] { return joined.size() == 2; }));
    BOOST_TEST_GE(replays, 1U);
    BOOST_TEST(std::ranges::any_of(errors, [](boost::system::error_code ec) {
        return ec == trystero::error::wrong_password;
    }));
}

// The same for an offer: B is lower and offers. The watcher records B's sealed offer to A and its
// offer id, and once B is back, sends both in B's name every 0.5 s of the virtual clock. Its AAD
// is B's and its id's, so it opens with the room key, but A answered that offer before: A refuses
// each replay, and B's own offer joins them.
void a_replayed_offer_is_refused() {
    fixture f{true};
    const std::string a_id = "zzzzzzzzzzzzzzzzzzzz";
    const std::string b_id = "BBBBBBBBBBBBBBBBBBBB";
    auto c = app_config(a_id);
    std::size_t refused = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refused += text.starts_with("refused the offer from " + b_id) ? 1 : 0;
    };
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
    auto a = join(a_app, "replayo");
    std::vector<std::string> joined;
    std::vector<std::string> left;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    a.on_peer_leave([&](std::string_view p) { left.emplace_back(p); });
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
    join_and_leave(f, b_id, "replayo", joined, left, [&] { return !recorded.empty(); });
    trystero::app b_app(f.io.get_executor(), f.net, app_config(b_id));
    const auto b = join(b_app, "replayo");
    std::size_t replays = 0;
    const auto end = f.net.steady_now() + 10s;
    while (joined.size() < 2 && f.net.steady_now() < end) {
        watcher.publish(a_topic, {.peer_id = b_id, .offer_id = recorded_id, .offer = recorded},
                        std::to_string(replays++));
        advance(f, 500ms);
    }
    BOOST_TEST_EQ(joined.size(), 2U);
    BOOST_TEST_GE(replays, 1U);
    BOOST_TEST_GE(refused, 1U);
}

// A watcher without the room key sends A's own sealed offer to B back to A, at once, as B's answer
// to it. Sealed as A's offer, which its AAD binds, it does not open as B's answer: A changes
// nothing, and B's own answer joins them.
void a_reflected_offer_is_refused_as_an_answer() {
    fixture f;
    const std::string a_id = "00000000000000000000";
    const std::string b_id = "BBBBBBBBBBBBBBBBBBBB";
    trystero::app a_app(f.io.get_executor(), f.net, app_config(a_id));
    auto a = join(a_app, "reflect");
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
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
    trystero::app b_app(f.io.get_executor(), f.net, app_config(b_id));
    const auto b = join(b_app, "reflect");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1; }));
    BOOST_TEST_GE(reflected, 1U);
}

// A watcher without the room key announces a decoy id above A, records A's sealed offer to it, and
// sends that offer to A in B's name whenever B announces itself, before B's own offer. Sealed as
// A's, which its AAD binds, it does not open as B's: A never answers one in B's name but B's own,
// and B's own offer joins them.
void a_reflected_offer_is_refused_as_an_offer() {
    fixture f{true};
    const std::string a_id = "yyyyyyyyyyyyyyyyyyyy";
    const std::string b_id = "BBBBBBBBBBBBBBBBBBBB";
    const std::string decoy = "zzzzzzzzzzzzzzzzzzzz";
    auto c = app_config(a_id);
    std::size_t answered_reflections = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        answered_reflections += text.starts_with("offer reflected") ? 1 : 0;
    };
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
    auto a = join(a_app, "reflecto");
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
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
    trystero::app b_app(f.io.get_executor(), f.net, app_config(b_id));
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
    fixture f;
    const std::string a_id = "MMMMMMMMMMMMMMMMMMMM";
    const std::string b_id = "BBBBBBBBBBBBBBBBBBBB";
    const std::string c_id = "ZZZZZZZZZZZZZZZZZZZZ";
    auto c = app_config(a_id);
    bool refused = false;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refused = refused || text.starts_with("could not open the answer from " + c_id);
    };
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
    auto a = join(a_app, "reflans");
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
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
    trystero::app b_app(f.io.get_executor(), f.net, app_config(b_id));
    const auto b = join(b_app, "reflans");
    webcpp::test::require(
        BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1 && !a_answer.empty(); })));
    trystero::app peer_c_app(f.io.get_executor(), f.net, app_config(c_id));
    const auto peer_c = join(peer_c_app, "reflans");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 2; }));
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
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
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
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
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
    advance(f, 5332ms);
    announced(1, "0early");
    BOOST_TEST_EQ(asked, 1U);
    advance(f, 1ms);
    announced(1, "0due");
    BOOST_TEST_EQ(asked, 2U);
}

// Beyond the bound, a new peer is refused, and one refused again and again is logged once: the
// offers to the first ids hold their places (the clock runs at its own pace here).
void a_refused_peer_is_logged_once() {
    fixture f;
    auto c = app_config("00000000000000000000");
    std::size_t refusals_of_one = 0;
    std::size_t refusals = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refusals += text.starts_with("refusing ") ? 1 : 0;
        refusals_of_one += text.starts_with("refusing 1same") ? 1 : 0;
    };
    trystero::app a_app(f.io.get_executor(), f.net, std::move(c));
    const auto a = join(a_app, "refusal");
    flooder attacker(f, "refusal");
    const auto before = f.net.relay(relay_a).events_seen;
    const std::size_t bound = trystero::detail::signaling::max_unconnected_peers;
    attacker.announce(bound + 8, "1fill");
    attacker.announce_as("1same", 5);
    BOOST_TEST(run_until(
        f.io, [&] { return f.net.relay(relay_a).events_seen >= before + bound + 8 + 5; }));
    BOOST_TEST(run_until(f.io, [&] { return refusals == 9; }));
    BOOST_TEST_EQ(refusals_of_one, 1U);
    BOOST_TEST_LE(f.net.live_connections(), 2 * bound);
}

// A peer id longer than protocol::max_peer_id_bytes is ignored in a signal; an app configured with
// one uses a random id instead, in its rooms too, and reports it.
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

    fixture f;
    trystero::app a_app(f.io.get_executor(), f.net, app_config(too_long));
    const auto a = join(a_app, "long");
    std::vector<boost::system::error_code> errors;
    a_app.on_error([&](std::string_view /*peer*/, boost::system::error_code ec,
                       std::string_view /*what*/) { errors.push_back(ec); });
    BOOST_TEST_EQ(a.self_id().size(), trystero::protocol::peer_id_size);
    BOOST_TEST(run_until(f.io, [&] { return errors.size() == 1; }));
    if (!errors.empty()) {
        BOOST_TEST(errors.front() == trystero::error::invalid_configuration);
    }
}

void three_peers_form_a_mesh() {
    fixture f;
    std::vector<trystero::app> apps;
    std::vector<trystero::room> rooms;
    for (const char* id :
         {"KKKKKKKKKKKKKKKKKKKK", "LLLLLLLLLLLLLLLLLLLL", "MMMMMMMMMMMMMMMMMMMM"}) {
        apps.emplace_back(f.io.get_executor(), f.net, app_config(id));
        rooms.push_back(join(apps.back(), "mesh"));
    }
    BOOST_TEST(run_until(f.io, [&] {
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
    BOOST_TEST(run_until(f.io, [&] { return heard == 2; }));
}

void async_receive_works_in_a_coroutine() {
    fixture f;
    trystero::app a_app(f.io.get_executor(), f.net, app_config("NNNNNNNNNNNNNNNNNNNN"));
    auto a = join(a_app, "coro");
    trystero::app b_app(f.io.get_executor(), f.net, app_config("OOOOOOOOOOOOOOOOOOOO"));
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
    BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1; }));
    a.make_action<std::string>("any").async_send("text", asio::detached);
    a.make_action<webcpp::document::document>("any").async_send(document_of("[1,2]"),
                                                                asio::detached);
    BOOST_TEST(run_until(f.io, [&] { return seen.size() == 2; }));
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
    fixture f;
    trystero::app a_app(f.io.get_executor(), f.net, app_config("TTTTTTTTTTTTTTTTTTTT"));
    auto a = join(a_app, "json");
    trystero::app z_app(f.io.get_executor(), f.net, app_config("UUUUUUUUUUUUUUUUUUUU"));
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
    BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1; }));
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
    BOOST_TEST(run_until(f.io, [&] { return in_resource.size() >= 3 && in_heap.size() >= 3; }));
    const std::vector<std::size_t> delivered{1000, at_bound, 3};
    BOOST_TEST_ALL_EQ(in_resource.begin(), in_resource.end(), delivered.begin(), delivered.end());
    BOOST_TEST_ALL_EQ(in_heap.begin(), in_heap.end(), delivered.begin(), delivered.end());
    BOOST_TEST_EQ(elsewhere, 0U);
}

// A JSON payload nested as deep as webcpp::document's default, 256, is delivered, to either
// document type: its parse recurses once a level, on the room's executor, on every target and in
// both suspension variants, within the engine's default stack (doc: #trystero-protocol-json).
void a_json_payload_nested_256_deep_is_delivered() {
    fixture f;
    trystero::app a_app(f.io.get_executor(), f.net, app_config("XXXXXXXXXXXXXXXXXXXX"));
    auto a = join(a_app, "deep");
    trystero::app z_app(f.io.get_executor(), f.net, app_config("YYYYYYYYYYYYYYYYYYYY"));
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
    BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1; }));
    const std::string deepest = std::string(256, '[') + std::string(256, ']');
    for (const char* name : {"pmr", "std"}) {
        a.make_action<webcpp::document::document>(name).async_send(document_of(deepest),
                                                                   asio::detached);
    }
    BOOST_TEST(run_until(f.io, [&] { return delivered == 2; }));
}

// A message of another format than a document action's completes its receive with
// error::transfer_rejected and a document that holds nothing, which allocates nothing: its root
// reads undefined and it has no image, in the room's resource for a pmr::document.
void a_rejected_document_receive_holds_nothing() {
    std::pmr::unsynchronized_pool_resource z_memory;  // outlives the io_context
    fixture f;
    trystero::app a_app(f.io.get_executor(), f.net, app_config("VVVVVVVVVVVVVVVVVVVV"));
    auto a = join(a_app, "rejected");
    trystero::app z_app(f.io.get_executor(), f.net, app_config("WWWWWWWWWWWWWWWWWWWW"));
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
            BOOST_TEST_EQ(info.from, "VVVVVVVVVVVVVVVVVVVV");
        });
    z.make_action<webcpp::document::document>("std").async_receive(
        [&](boost::system::error_code ec, webcpp::document::document v,
            const trystero::message_info& /*info*/) {
            ++completed;
            BOOST_TEST(ec == trystero::error::transfer_rejected);
            BOOST_TEST(v.root().is_undefined());
            BOOST_TEST(v.image().empty());
        });
    BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1; }));
    a.make_action<std::string>("pmr").async_send("not JSON", asio::detached);
    a.make_action<std::string>("std").async_send("not JSON", asio::detached);
    BOOST_TEST(run_until(f.io, [&] { return completed == 2; }));
}

void view_handlers_read_the_frame_without_copying() {
    fixture f;
    trystero::app a_app(f.io.get_executor(), f.net, app_config("RRRRRRRRRRRRRRRRRRRR"));
    auto a = join(a_app, "views");
    trystero::app b_app(f.io.get_executor(), f.net, app_config("SSSSSSSSSSSSSSSSSSSS"));
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
    BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1; }));
    a.make_action<std::string_view>("note").async_send(
        std::string_view("a view, copied once at the call"), asio::detached);
    a.make_action<trystero::bytes>("blob").async_send(
        trystero::bytes(50'000, 7), trystero::send_options{.targets = {}, .metadata = R"({"k":1})"},
        asio::detached);
    BOOST_TEST(run_until(f.io, [&] { return !text_seen.empty() && bytes_seen != 0; }));
    BOOST_TEST_EQ(text_seen, "a view, copied once at the call");
    BOOST_TEST_EQ(bytes_seen, 50'000U);
    BOOST_TEST_EQ(metadata_seen, R"({"k":1})");
}

void a_rate_limited_relay_does_not_stop_discovery() {
    fixture f;
    f.net.relay(relay_a).rate_limit_next = 1000;  // relay A refuses everything; B carries the room
    trystero::app a_app(f.io.get_executor(), f.net, app_config("PPPPPPPPPPPPPPPPPPPP"));
    auto a = join(a_app, "limits");
    trystero::app b_app(f.io.get_executor(), f.net, app_config("QQQQQQQQQQQQQQQQQQQQ"));
    auto b = join(b_app, "limits");
    BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }));
    // Backoff: after the first refusal each peer stops publishing on relay A, so it sees only a
    // handful of attempts.
    BOOST_TEST_LT(1000 - f.net.relay(relay_a).rate_limit_next, 12U);
}

// A connection's end is every room's (one room here): a forced failure, a frame that does not
// decode, and a 65th advertised token each end it; the peers then meet again over a new one.
void a_connections_end_is_seen_and_the_peers_meet_again() {
    for (std::size_t way = 0; way < 3; ++way) {
        fixture f{true};
        trystero::app a_app(f.io.get_executor(), f.net, app_config("NNNNNNNNNNNNNNNNNNNN"));
        auto a = join(a_app, "ends");
        std::vector<std::size_t> a_pcs;
        f.net.on_made = [&](std::size_t pc, std::span<const trystero::backend::ice_server>) {
            a_pcs.push_back(pc);
        };
        trystero::app b_app(f.io.get_executor(), f.net, app_config("OOOOOOOOOOOOOOOOOOOO"));
        const auto b = join(b_app, "ends");
        std::size_t joins = 0;
        std::size_t leaves = 0;
        a.on_peer_join([&](std::string_view) { ++joins; });
        a.on_peer_leave([&](std::string_view) { ++leaves; });
        webcpp::test::require(BOOST_TEST(run_until(f, [&] { return joins == 1; })));
        const auto open_pc = f.net.live_connections();  // the pair, both ends
        BOOST_TEST_EQ(open_pc, 2U);
        // The first connection made once A's room exists: A's offer in practice, A's id being the
        // lower; an end of either side ends the pair, so every way holds for either.
        const std::size_t a_end = a_pcs.front();
        if (way == 0) {
            f.net.fail(a_end);
        } else if (way == 1) {
            const std::array<std::uint8_t, 4> bad{3, 0, 1, 'x'};
            f.net.inject(a_end, bad);
        } else {
            for (std::size_t i = 0; i < 65; ++i) {
                f.net.inject(a_end,
                             trystero::protocol::presence_frame("X" + std::to_string(i), true));
            }
        }
        BOOST_TEST(run_until(f, [&] { return leaves == 1; }));
        BOOST_TEST(run_until(f, [&] { return joins == 2; }, 20s));
    }
}

// A frame naming a token no room of the app has is dropped, the connection stays, and the app's
// resource keeps none of them (64 kept frames of 4,000 bytes would hold over 256 KB).
void a_frame_of_an_unknown_token_is_dropped() {
    counting_resource memory;
    fixture f{true};
    auto a_config = app_config("PPPPPPPPPPPPPPPPPPPP");
    a_config.memory = &memory;
    trystero::app a_app(f.io.get_executor(), f.net, std::move(a_config));
    auto a = join(a_app, "unknown");
    std::vector<std::size_t> pcs;
    f.net.on_made = [&](std::size_t pc, std::span<const trystero::backend::ice_server>) {
        pcs.push_back(pc);
    };
    trystero::app b_app(f.io.get_executor(), f.net, app_config("QQQQQQQQQQQQQQQQQQQQ"));
    const auto b = join(b_app, "unknown");
    std::size_t leaves = 0;
    a.on_peer_leave([&](std::string_view) { ++leaves; });
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; })));
    const std::vector<std::uint8_t> action(4'000, 2);
    advance(f, 300ms);
    const auto before = memory.in_use;
    for (std::size_t i = 0; i < 1000; ++i) {
        f.net.inject(pcs.front(), trystero::protocol::room_frame("unknown-token", action));
    }
    advance(f, 300ms);
    BOOST_TEST_EQ(leaves, 0U);
    BOOST_TEST_EQ(a.peers().size(), 1U);
    BOOST_TEST_LT(memory.in_use, before + (std::size_t{64} * 1024));
}

// Malformed envelopes end the connection; a frame of exactly 16,515 bytes does not.
void malformed_envelopes_end_the_connection() {
    const auto zero_token = std::vector<std::uint8_t>{2, 1, 0, 0};
    std::vector<std::uint8_t> long_token{2, 1, 0, 129};
    long_token.resize(4 + 129, 'x');
    const auto kind_3 = std::vector<std::uint8_t>{3, 1, 0, 1, 'x'};
    std::vector<std::uint8_t> too_big{1, 0, 1, 'x'};
    too_big.resize(16'516, 0);
    for (const auto& bad : {zero_token, long_token, kind_3, too_big}) {
        fixture f{true};
        trystero::app a_app(f.io.get_executor(), f.net, app_config("RRRRRRRRRRRRRRRRRRRR"));
        auto a = join(a_app, "bad");
        std::vector<std::size_t> pcs;
        f.net.on_made = [&](std::size_t pc, std::span<const trystero::backend::ice_server>) {
            pcs.push_back(pc);
        };
        trystero::app b_app(f.io.get_executor(), f.net, app_config("SSSSSSSSSSSSSSSSSSSS"));
        const auto b = join(b_app, "bad");
        std::size_t leaves = 0;
        a.on_peer_leave([&](std::string_view) { ++leaves; });
        webcpp::test::require(BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; })));
        f.net.inject(pcs.front(), bad);
        BOOST_TEST(run_until(f, [&] { return leaves == 1; }));
    }
    fixture f{true};
    trystero::app a_app(f.io.get_executor(), f.net, app_config("RRRRRRRRRRRRRRRRRRRR"));
    auto a = join(a_app, "bad");
    std::vector<std::size_t> pcs;
    f.net.on_made = [&](std::size_t pc, std::span<const trystero::backend::ice_server>) {
        pcs.push_back(pc);
    };
    trystero::app b_app(f.io.get_executor(), f.net, app_config("SSSSSSSSSSSSSSSSSSSS"));
    const auto b = join(b_app, "bad");
    std::size_t leaves = 0;
    a.on_peer_leave([&](std::string_view) { ++leaves; });
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return a.peers().size() == 1; })));
    std::vector<std::uint8_t> largest{1, 0, 1, 'x'};
    largest.resize(16'515, 0);
    f.net.inject(pcs.front(), largest);
    advance(f, 300ms);
    BOOST_TEST_EQ(leaves, 0U);
}

// A room acts on the end of the connection its peer is bound to, and on no other: a late end of
// the peer's first connection keeps the peer, bound to the second; the second's end removes it.
void a_late_end_of_another_connection_keeps_the_peer() {
    fixture f{true};
    const std::string b_id = "UUUUUUUUUUUUUUUUUUUU";
    const auto a = make_app_room(f, "TTTTTTTTTTTTTTTTTTTT", "late");
    std::vector<std::size_t> pcs;
    f.net.on_made = [&](std::size_t pc, std::span<const trystero::backend::ice_server>) {
        pcs.push_back(pc);
    };
    trystero::app b_app(f.io.get_executor(), f.net, app_config(b_id));
    auto b = join(b_app, "late");
    std::size_t b_joins = 0;
    std::size_t b_leaves = 0;
    b.on_peer_join([&](std::string_view) { ++b_joins; });
    b.on_peer_leave([&](std::string_view) { ++b_leaves; });
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return a.room->active_peers().size() == 1 && b_joins == 1; })));
    const auto first = a.app->find(b_id);
    webcpp::test::require(BOOST_TEST(first != nullptr));
    f.net.fail(pcs.front());
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return a.room->active_peers().size() == 1 && b_joins == 2; }, 30s)));
    const auto second = a.app->find(b_id);
    webcpp::test::require(BOOST_TEST(second != nullptr && second != first));
    a.room->connection_ended(first, b_id, "a late end of the first connection");
    advance(f, 5000ms);
    BOOST_TEST_EQ(a.room->active_peers().size(), 1U);
    BOOST_TEST_EQ(b_leaves, 1U);
    second->end("test");
    BOOST_TEST(a.room->active_peers().empty());
    a.room->abandon();
}

// A remote's presence(0) for the room is a departure: the peer leaves, and the connection, which
// another room of the app could use, idles instead of ending.
void a_withdrawn_presence_is_a_departure() {
    fixture f{true};
    const std::string b_id = "WWWWWWWWWWWWWWWWWWWW";
    const auto a = make_app_room(f, "VVVVVVVVVVVVVVVVVVVV", "withdraw");
    std::size_t leaves = 0;
    a.room->on_peer_leave(
        a.room->bind<void(std::string_view)>([&](std::string_view) { ++leaves; }));
    std::vector<std::size_t> pcs;
    f.net.on_made = [&](std::size_t pc, std::span<const trystero::backend::ice_server>) {
        pcs.push_back(pc);
    };
    trystero::app b_app(f.io.get_executor(), f.net, app_config(b_id));
    const auto b = join(b_app, "withdraw");
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return a.room->active_peers().size() == 1; })));
    const auto c = a.app->find(b_id);
    webcpp::test::require(BOOST_TEST(c != nullptr));
    f.net.inject(pcs.front(), trystero::protocol::presence_frame(
                                  trystero::protocol::room_token(app_id, "withdraw"), false));
    BOOST_TEST(run_until(f, [&] { return leaves == 1; }));
    BOOST_TEST(a.room->active_peers().empty());
    BOOST_TEST(c->live());
    BOOST_TEST(a.app->find(b_id) == c);
    a.room->abandon();
}

// A kick by the connection's last room ends the connection at once, rather than idling it: the
// handshake times out, since every room frame to A is dropped.
void a_kick_by_the_last_room_ends_the_connection() {
    fixture f{true};
    const std::string b_id = "YYYYYYYYYYYYYYYYYYYY";
    std::vector<std::string> log;
    const auto a = make_app_room(
        f, "XXXXXXXXXXXXXXXXXXXX", "kick", [&](trystero::app_config& c, trystero::room_options&) {
            c.log = [&log](std::string_view category, std::string_view text) {
                log.push_back(std::string(category) + ": " + std::string(text));
            };
        });
    std::vector<std::size_t> pcs;
    f.net.on_made = [&](std::size_t pc, std::span<const trystero::backend::ice_server>) {
        pcs.push_back(pc);
    };
    f.net.filter = [&](std::size_t, std::size_t to, std::span<const std::uint8_t> frame) {
        const bool room_frame_to_a = !pcs.empty() && to == pcs.front() && frame[0] == 1;
        return room_frame_to_a ? trystero::test::memory_network::verdict::drop
                               : trystero::test::memory_network::verdict::deliver;
    };
    trystero::app b_app(f.io.get_executor(), f.net, app_config(b_id));
    const auto b = join(b_app, "kick");
    const std::string kicked =
        "webrtc: connection with " + b_id + " ended: its last room kicked the peer";
    BOOST_TEST(run_until(f, [&] { return std::ranges::count(log, kicked) > 0; }, 20s));
    a.room->abandon();
}

// A room left with no idle time still says so on the connection, before its departure ends it:
// Trystero's idle close is always a later timer.
void a_room_left_with_no_idle_time_sends_its_presence() {
    fixture f{true};
    const auto a = make_app_room(f, "ZZZZZZZZZZZZZZZZZZZZ", "no-idle",
                                 [](trystero::app_config& c, trystero::room_options&) {
                                     c.idle_timeout = std::chrono::milliseconds::zero();
                                 });
    trystero::app b_app(f.io.get_executor(), f.net, app_config("zzzzzzzzzzzzzzzzzzzz"));
    const auto b = join(b_app, "no-idle");
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return a.room->active_peers().size() == 1 && b.peers().size() == 1; })));
    const auto absent = trystero::protocol::presence_frame(
        trystero::protocol::room_token(app_id, "no-idle"), false);
    std::size_t seen = 0;
    f.net.filter = [&](std::size_t, std::size_t, std::span<const std::uint8_t> frame) {
        if (std::ranges::equal(frame, absent)) {
            ++seen;
        }
        return trystero::test::memory_network::verdict::deliver;
    };
    a.room->abandon();
    advance(f, 1000ms);
    BOOST_TEST_EQ(seen, 1U);
}

// The errors' values are fixed and never reused; their messages name what failed.
void the_errors_have_their_values() {
    BOOST_TEST_EQ(static_cast<int>(trystero::error::already_joined), 14);
    BOOST_TEST_EQ(static_cast<int>(trystero::error::too_many_rooms), 15);
    BOOST_TEST_EQ(make_error_code(trystero::error::already_joined).message(),
                  "the app has already joined a room of that id");
    BOOST_TEST_EQ(make_error_code(trystero::error::too_many_rooms).message(),
                  "the app holds as many rooms as a peer accepts, 64");
    BOOST_TEST_EQ(make_error_code(trystero::error::invalid_configuration).message(),
                  "the configuration is unusable");
    // Appended after too_many_rooms, in 0.27.0's order of room errors, with its messages.
    BOOST_TEST_EQ(static_cast<int>(trystero::error::room_full), 16);
    BOOST_TEST_EQ(static_cast<int>(trystero::error::request_timed_out), 28);
    BOOST_TEST_EQ(make_error_code(trystero::error::room_full).message(), "room full");
    BOOST_TEST_EQ(make_error_code(trystero::error::banned).message(), "peer banned");
    BOOST_TEST_EQ(make_error_code(trystero::error::decrypt_failed).message(),
                  "failed to decrypt a signal (incorrect password or corrupted signal)");
    BOOST_TEST_EQ(make_error_code(trystero::error::action_aborted).message(), "operation aborted");
}

}  // namespace

int main() {
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
    run_case(the_wire_keeps_0_27_0s_bounds);
    run_case(a_control_frame_into_a_full_channel_drops_the_peer);
    run_case(ping_measures_a_round_trip);
    run_case(the_same_password_joins_and_another_does_not);
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
    run_case(signals_that_do_not_open_are_reported_once_per_interval);
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
    run_case(async_receive_works_in_a_coroutine);
    run_case(json_payloads_are_read_in_the_rooms_resource_up_to_its_bound);
    run_case(a_rejected_document_receive_holds_nothing);
    run_case(view_handlers_read_the_frame_without_copying);
    run_case(a_rate_limited_relay_does_not_stop_discovery);
    run_case(a_connections_end_is_seen_and_the_peers_meet_again);
    run_case(a_frame_of_an_unknown_token_is_dropped);
    run_case(malformed_envelopes_end_the_connection);
    run_case(a_late_end_of_another_connection_keeps_the_peer);
    run_case(a_withdrawn_presence_is_a_departure);
    run_case(a_kick_by_the_last_room_ends_the_connection);
    run_case(a_room_left_with_no_idle_time_sends_its_presence);
    run_case(the_errors_have_their_values);
    // Under Asyncify, a main that suspended loses what it returns: the status goes first.
    return webcpp::test::exit_status(boost::report_errors());
}
