// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Rooms of one app sharing one connection per remote peer, over the memory backend, on its virtual
// clock (doc: #trystero-design-sharing).

#include <limits>
#include <webcpp/trystero.hpp>
#include <webcpp/trystero/protocol/action_wire.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include "support/memory_backend.hpp"
#include "support/rooms.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "require.hpp"

namespace trystero = webcpp::trystero;
using namespace webcpp::trystero::test;
using namespace std::chrono_literals;

namespace {

// Every room reports the app's id; an over-long one is replaced, and it and a negative idle time
// are reported to the app's handler, set after the app is made, once each.
void every_room_has_the_apps_id() {
    fixture f{true};
    trystero::app a(f.io.get_executor(), f.net, app_config("AAAAAAAAAAAAAAAAAAAA"));
    const auto r1 = join(a, "r1");
    const auto r2 = join(a, "r2");
    BOOST_TEST_EQ(r1.self_id(), "AAAAAAAAAAAAAAAAAAAA");
    BOOST_TEST_EQ(r2.self_id(), a.self_id());
    auto long_id = app_config(std::string(65, 'x'));
    long_id.idle_timeout = -1ms;
    std::vector<std::pair<boost::system::error_code, std::string>> reports;
    trystero::app b(f.io.get_executor(), f.net, std::move(long_id));
    b.on_error([&](std::string_view peer, boost::system::error_code ec, std::string_view what) {
        BOOST_TEST(peer.empty());
        reports.emplace_back(ec, what);
    });
    BOOST_TEST_EQ(b.self_id().size(), trystero::protocol::peer_id_size);
    BOOST_TEST(run_until(f, [&] { return reports.size() == 2; }));
    webcpp::trystero::test::advance(f, 100ms);
    webcpp::test::require(BOOST_TEST_EQ(reports.size(), 2U));
    for (const auto& [ec, what] : reports) {
        BOOST_TEST(ec == trystero::error::invalid_configuration);
    }
    BOOST_TEST(reports[0].second.starts_with("self_id "));
    BOOST_TEST(reports[1].second.starts_with("idle_timeout "));
}

// A report reaches only the handler set when it is delivered, and each is delivered once: a handler
// replaced before the delivery gets none, and one set after it gets none again.
void each_setup_report_reaches_the_current_handler_once() {
    fixture f{true};
    auto long_id = app_config(std::string(65, 'x'));
    long_id.idle_timeout = -1ms;
    trystero::app b(f.io.get_executor(), f.net, std::move(long_id));
    std::array<std::size_t, 3> calls{};
    b.on_error([&](std::string_view, boost::system::error_code, std::string_view) { ++calls[0]; });
    b.on_error([&](std::string_view, boost::system::error_code, std::string_view) { ++calls[1]; });
    webcpp::trystero::test::advance(f, 100ms);
    b.on_error([&](std::string_view, boost::system::error_code, std::string_view) { ++calls[2]; });
    webcpp::trystero::test::advance(f, 100ms);
    BOOST_TEST_EQ(calls[0], 0U);
    BOOST_TEST_EQ(calls[1], 2U);
    BOOST_TEST_EQ(calls[2], 0U);
}

// A room joined twice is refused while it is joined, and so is a 65th room.
void joining_is_refused_twice_and_beyond_64() {
    fixture f{true};
    trystero::app a(f.io.get_executor(), f.net, app_config("BBBBBBBBBBBBBBBBBBBB"));
    const auto r = join(a, "r");
    const auto again = a.join("r");
    BOOST_TEST(!again && again.error() == trystero::error::already_joined);
    std::vector<trystero::room> rooms;
    for (int i = 1; i < 64; ++i) {
        rooms.push_back(join(a, "x" + std::to_string(i)));
    }
    const auto too_many = a.join("x64");
    BOOST_TEST(!too_many && too_many.error() == trystero::error::too_many_rooms);
}

// Two apps of one app id with the same self id never meet, and nothing loops.
void two_apps_with_one_id_never_meet() {
    fixture f{true};
    trystero::app a(f.io.get_executor(), f.net, app_config("CCCCCCCCCCCCCCCCCCCC"));
    trystero::app b(f.io.get_executor(), f.net, app_config("CCCCCCCCCCCCCCCCCCCC"));
    const auto ra = join(a, "same");
    const auto rb = join(b, "same");
    webcpp::trystero::test::advance(f, 2000ms);
    BOOST_TEST(ra.peers().empty() && rb.peers().empty());
    BOOST_TEST_EQ(f.net.made_connections, 0U);
}

/** Two apps, A and B, on the virtual clock, and their rooms. */
struct two_apps {
    fixture f{true};
    trystero::app a;
    trystero::app b;
    std::vector<trystero::room> a_rooms;
    std::vector<trystero::room> b_rooms;

    two_apps(std::string_view a_id, std::string_view b_id, std::string_view password = {})
        : a(f.io.get_executor(), f.net, app_config(a_id, password)),
          b(f.io.get_executor(), f.net, app_config(b_id, password)) {}

    ~two_apps() {
        // Observers often capture locals declared after this fixture; room teardown still sends.
        f.net.filter = {};
        f.net.on_made = {};
    }

    [[nodiscard]] bool all_joined() const {
        const auto one = [](const trystero::room& r) { return r.peers().size() == 1; };
        return std::ranges::all_of(a_rooms, one) && std::ranges::all_of(b_rooms, one);
    }
};

/** Joins A and B to each room given, in that order, and waits until every room of both has the
 * other. */
void joined_in(two_apps& t, std::initializer_list<const char*> rooms) {
    for (const auto* id : rooms) {
        t.a_rooms.push_back(join(t.a, id));
        t.b_rooms.push_back(join(t.b, id));
    }
    webcpp::test::require(BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); })));
}

// Two rooms, one connection; each room's action of one name reaches only its room, both ways.
void two_rooms_share_one_connection() {
    two_apps t("DDDDDDDDDDDDDDDDDDDD", "EEEEEEEEEEEEEEEEEEEE");
    joined_in(t, {"r1", "r2"});
    BOOST_TEST(run_until(t.f, [&] { return t.f.net.live_connections() == 2U; }));  // one pair
    std::array<std::vector<std::string>, 2> at_b;
    std::array<std::vector<std::string>, 2> at_a;
    std::vector<trystero::action<std::string>> actions;
    for (std::size_t i = 0; i < 2; ++i) {
        actions.push_back(t.b_rooms[i].make_action<std::string>("chat"));
        actions.back().on_message([&at_b, i](std::string s, const trystero::message_info&) {
            at_b[i].push_back(std::move(s));
        });
        actions.push_back(t.a_rooms[i].make_action<std::string>("chat"));
        actions.back().on_message([&at_a, i](std::string s, const trystero::message_info&) {
            at_a[i].push_back(std::move(s));
        });
    }
    t.a_rooms[0].make_action<std::string>("chat").async_send("a1",
                                                             [](boost::system::error_code) {});
    t.a_rooms[1].make_action<std::string>("chat").async_send("a2",
                                                             [](boost::system::error_code) {});
    t.b_rooms[0].make_action<std::string>("chat").async_send("b1",
                                                             [](boost::system::error_code) {});
    t.b_rooms[1].make_action<std::string>("chat").async_send("b2",
                                                             [](boost::system::error_code) {});
    BOOST_TEST(run_until(t.f, [&] {
        return at_b[0].size() == 1 && at_b[1].size() == 1 && at_a[0].size() == 1 &&
               at_a[1].size() == 1;
    }));
    BOOST_TEST(at_b[0].at(0) == "a1" && at_b[1].at(0) == "a2");
    BOOST_TEST(at_a[0].at(0) == "b1" && at_a[1].at(0) == "b2");
}

// Three rooms, joined in another order on each side, one connection.
void three_rooms_share_one_connection() {
    two_apps t("FFFFFFFFFFFFFFFFFFFF", "GGGGGGGGGGGGGGGGGGGG");
    for (const auto* id : {"r1", "r2", "r3"}) {
        t.a_rooms.push_back(join(t.a, id));
    }
    for (const auto* id : {"r3", "r1", "r2"}) {
        t.b_rooms.push_back(join(t.b, id));
    }
    BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }));
    BOOST_TEST(run_until(t.f, [&] { return t.f.net.live_connections() == 2U; }));
}

// Both rooms negotiate at once; one connection stays, both rooms join over it.
void a_second_connection_is_dropped() {
    two_apps t("JJJJJJJJJJJJJJJJJJJJ", "KKKKKKKKKKKKKKKKKKKK");
    joined_in(t, {"r1", "r2"});
    BOOST_TEST(run_until(t.f, [&] { return t.f.net.live_connections() == 2U; }));
}

// An app moved from is only destroyed: its state, moved, still counts its handle once, so the room
// keeps its peer and its connection when the moved-from handle and the new one are both gone, which
// the other side sees as no departure and no new connection.
void a_moved_from_app_releases_nothing() {
    fixture f{true};
    std::optional<trystero::app> a(std::in_place, f.io.get_executor(), f.net,
                                   app_config("LLLLLLLLLLLLLLLLLLLL"));
    trystero::app b(f.io.get_executor(), f.net, app_config("MMMMMMMMMMMMMMMMMMMM"));
    auto ra = join(*a, "r");
    auto rb = join(b, "r");
    int leaves = 0;
    rb.on_peer_leave([&](std::string_view) { ++leaves; });
    webcpp::test::require(
        BOOST_TEST(run_until(f, [&] { return ra.peers().size() == 1 && rb.peers().size() == 1; })));
    const auto made = f.net.made_connections;
    {
        const trystero::app moved(std::move(*a));
        // A moved-from app takes another's state by assignment, and releases only that one.
        *a = trystero::app(f.io.get_executor(), f.net, app_config("UUUUUUUUUUUUUUUUUUUU"));
        BOOST_TEST_EQ(a->self_id(), "UUUUUUUUUUUUUUUUUUUU");
        a.reset();
        BOOST_TEST_EQ(moved.self_id(), "LLLLLLLLLLLLLLLLLLLL");
    }
    webcpp::trystero::test::advance(f, 1000ms);
    BOOST_TEST_EQ(leaves, 0);
    BOOST_TEST_EQ(f.net.made_connections, made);
    BOOST_TEST_EQ(ra.peers().size(), 1U);
}

// An app destroyed while its room remains changes nothing for the room; the room's departure, the
// last user of the state, then ends the connection at once rather than idling it.
void an_app_destroyed_leaves_its_rooms_going() {
    fixture f{true};
    std::optional<trystero::app> a(std::in_place, f.io.get_executor(), f.net,
                                   app_config("SSSSSSSSSSSSSSSSSSSS"));
    trystero::app b(f.io.get_executor(), f.net, app_config("TTTTTTTTTTTTTTTTTTTT"));
    std::optional<trystero::room> ra(join(*a, "r"));
    auto rb = join(b, "r");
    int leaves = 0;
    rb.on_peer_leave([&](std::string_view) { ++leaves; });
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return ra->peers().size() == 1 && rb.peers().size() == 1; })));
    const auto made = f.net.made_connections;
    a.reset();
    webcpp::trystero::test::advance(f, 1000ms);
    BOOST_TEST_EQ(leaves, 0);
    BOOST_TEST_EQ(f.net.made_connections, made);
    BOOST_TEST_EQ(ra->peers().size(), 1U);
    ra.reset();
    BOOST_TEST(run_until(f, [&] { return f.net.live_connections() == 0U; }, 1000ms));
}

// An app assigned over releases the state it held: that state's rooms go on, and when the last of
// them goes, its connections end at once rather than idling.
void an_app_assigned_over_releases_its_state() {
    fixture f{true};
    trystero::app a(f.io.get_executor(), f.net, app_config("NNNNNNNNNNNNNNNNNNNN"));
    trystero::app b(f.io.get_executor(), f.net, app_config("OOOOOOOOOOOOOOOOOOOO"));
    std::optional<trystero::room> ra(join(a, "r"));
    auto rb = join(b, "r");
    int leaves = 0;
    rb.on_peer_leave([&](std::string_view) { ++leaves; });
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return ra->peers().size() == 1 && rb.peers().size() == 1; })));
    const auto made = f.net.made_connections;
    a = trystero::app(f.io.get_executor(), f.net, app_config("PPPPPPPPPPPPPPPPPPPP"));
    BOOST_TEST_EQ(a.self_id(), "PPPPPPPPPPPPPPPPPPPP");
    webcpp::trystero::test::advance(f, 1000ms);
    BOOST_TEST_EQ(leaves, 0);
    BOOST_TEST_EQ(f.net.made_connections, made);
    BOOST_TEST_EQ(ra->peers().size(), 1U);
    ra.reset();
    BOOST_TEST(run_until(f, [&] { return f.net.live_connections() == 0U; }, 1000ms));
}

// An app assigned to itself keeps its state and its count: its room's departure idles the
// connection, which the app's handle still holds.
void an_app_assigned_to_itself_keeps_its_state() {
    fixture f{true};
    trystero::app a(f.io.get_executor(), f.net, app_config("QQQQQQQQQQQQQQQQQQQQ"));
    trystero::app b(f.io.get_executor(), f.net, app_config("RRRRRRRRRRRRRRRRRRRR"));
    std::optional<trystero::room> ra(join(a, "r"));
    auto rb = join(b, "r");
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return ra->peers().size() == 1 && rb.peers().size() == 1; })));
    auto& same = a;  // through a reference: the compiler warns of a move to itself written out
    a = std::move(same);
    BOOST_TEST_EQ(a.self_id(), "QQQQQQQQQQQQQQQQQQQQ");
    ra.reset();
    webcpp::trystero::test::advance(f, 1000ms);
    BOOST_TEST_EQ(f.net.live_connections(), 2U);
}

/** A's end of the first pair made, recorded by on_made: A, the lower id, offers first. */
struct ends {
    std::vector<std::size_t> made;

    explicit ends(two_apps& t) {
        t.f.net.on_made = [this](std::size_t pc, std::span<const trystero::backend::ice_server>) {
            made.push_back(pc);
        };
    }

    [[nodiscard]] std::size_t a() const { return made.at(0); }
};

/** The action an inline room frame of `room` carries (`\@_hsdata`, `\@_leave`, `chat`, ...), or
 * empty. */
std::string internal_name(std::span<const std::uint8_t> frame, std::string_view room) {
    const auto env = trystero::protocol::decode_envelope(frame);
    if (!env || env->type != trystero::protocol::envelope::kind::room ||
        env->token != trystero::protocol::room_token(app_id, room)) {
        return {};
    }
    const auto d = trystero::protocol::decode_action(env->payload);
    const auto* p = d.frame ? std::get_if<trystero::protocol::proposal>(&*d.frame) : nullptr;
    return p != nullptr ? std::string(p->name) : std::string();
}

/** What a frame of `room` is, for a record of what one end sent: its inline action's name,
 * `presence(1)` or `presence(0)`, or empty for a frame of another room or not inline. */
std::string what_of(std::span<const std::uint8_t> frame, std::string_view room) {
    const auto env = trystero::protocol::decode_envelope(frame);
    if (env && env->type == trystero::protocol::envelope::kind::presence &&
        env->token == trystero::protocol::room_token(app_id, room)) {
        return env->present ? "presence(1)" : "presence(0)";
    }
    return internal_name(frame, room);
}

/** A room frame of `room`'s token holding the inline action `name`, with `json` as its payload
 * (none when empty), as a peer's room sends an internal action. */
trystero::protocol::bytes internal_frame(std::string_view room, std::string_view name,
                                         std::string_view json = {}) {
    const auto token = trystero::protocol::room_token(app_id, room);
    const std::span<const std::uint8_t> payload(reinterpret_cast<const std::uint8_t*>(json.data()),
                                                json.size());
    auto frame = trystero::protocol::begin_room_frame(
        token, trystero::protocol::inline_header_size + payload.size());
    trystero::protocol::append_inline(frame, name,
                                      json.empty() ? trystero::protocol::payload_format::text
                                                   : trystero::protocol::payload_format::json,
                                      {}, payload);
    return frame;
}

/** Replaces r2's proof responses with an incorrect proof, leaving its challenges and every
 * other room unchanged. The peers have already authenticated the shared app password. */
void corrupt_r2_responses(two_apps& t) {
    t.f.net.filter = [&t](std::size_t, std::size_t to, std::span<const std::uint8_t> frame) {
        const std::string_view text(reinterpret_cast<const char*>(frame.data()), frame.size());
        if (internal_name(frame, "r2") == "@_hsdata" &&
            text.find(R"("__trystero_pw":"response")") != std::string_view::npos) {
            t.f.net.inject(to, internal_frame("r2", "@_hsdata",
                                              R"({"__trystero_pw":"response","h":"incorrect"})"));
            return trystero::test::memory_network::verdict::drop;
        }
        return trystero::test::memory_network::verdict::deliver;
    };
}

// Frames of r2 that reach A before B's presence, as a Trystero JS peer sends its handshake before
// its presence: A keeps them, and once B's presence binds r2 it delivers them after making B's
// state, so the handshake completes. B's own `@_hsready` of r2 never reaches A, so only the kept
// one can complete it. B, whose join finds A's presence for r2, binds during the join, as Trystero
// JS does: its handshake goes out first, then its presence. The 64-frame bound and the 10 s expiry
// are the connection's, tested in connection_test.cpp.
void frames_before_presence_are_kept() {
    two_apps t("HHHHHHHHHHHHHHHHHHHH", "IIIIIIIIIIIIIIIIIIII");
    const ends e(t);
    joined_in(t, {"r1"});
    t.a_rooms.push_back(join(t.a, "r2"));
    webcpp::trystero::test::advance(t.f, 200ms);
    t.f.net.inject(e.a(), internal_frame("r2", "@_hsready"));
    webcpp::trystero::test::advance(t.f, 50ms);
    BOOST_TEST(t.a_rooms[1].peers().empty());  // kept, not bound: B never advertised r2
    std::vector<std::string> b_sent;
    t.f.net.filter = [&](std::size_t, std::size_t to, std::span<const std::uint8_t> frame) {
        if (to != e.a()) {
            return trystero::test::memory_network::verdict::deliver;
        }
        if (auto what = what_of(frame, "r2"); !what.empty()) {
            b_sent.push_back(std::move(what));
        }
        return internal_name(frame, "r2") == "@_hsready"
                   ? trystero::test::memory_network::verdict::drop
                   : trystero::test::memory_network::verdict::deliver;
    };
    t.b_rooms.push_back(join(t.b, "r2"));
    BOOST_TEST((b_sent == std::vector<std::string>{"@_hsready", "presence(1)"}));
    BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }));
    t.f.net.filter = nullptr;  // the rooms' farewells go through the network after `e` is gone
}

// A kept frame that makes A kick B while A binds (an undecodable action frame of r2, then B's
// presence for r2): A kicks B from r2 only, nothing else happens, and nothing touches the peer's
// removed state (shared_hardened and the sanitizers see it).
void a_kept_frame_that_kicks_during_a_binding() {
    two_apps t("HHHHHHHHHHHHHHHHHHH1", "IIIIIIIIIIIIIIIIIII2");
    const ends e(t);
    joined_in(t, {"r1"});
    t.a_rooms.push_back(join(t.a, "r2"));
    webcpp::trystero::test::advance(t.f, 200ms);
    std::size_t leaves_sent = 0;
    std::vector<std::string> a_sent;
    t.f.net.filter = [&](std::size_t from, std::size_t, std::span<const std::uint8_t> frame) {
        leaves_sent += from == e.a() && internal_name(frame, "r2") == "@_leave" ? 1 : 0;
        if (auto what = what_of(frame, "r2"); from == e.a() && !what.empty()) {
            a_sent.push_back(std::move(what));
        }
        return trystero::test::memory_network::verdict::deliver;
    };
    const auto token = trystero::protocol::room_token(app_id, "r2");
    const std::array<std::uint8_t, 3> garbage{9, 9, 9};
    t.f.net.inject(e.a(), trystero::protocol::room_frame(token, garbage));
    t.f.net.inject(e.a(), trystero::protocol::presence_frame(token, true));
    BOOST_TEST(run_until(t.f, [&] { return leaves_sent == 1; }));
    webcpp::trystero::test::advance(t.f, 1000ms);
    // The kick came before the handshake's first message, which the removed peer never got.
    BOOST_TEST((a_sent == std::vector<std::string>{"@_leave"}));
    BOOST_TEST(t.a_rooms[1].peers().empty());
    BOOST_TEST_EQ(t.a_rooms[0].peers().size(), 1U);
    BOOST_TEST_EQ(t.f.net.live_connections(), 2U);
    // The rooms' farewells go through the network after `e` and `leaves_sent` are gone.
    t.f.net.filter = nullptr;
}

// A frame of a room from a peer that advertised it binds the room, as its presence does: after B's
// `@_leave` of r2, which leaves the connection and B's presence for r2 as they were, B's next frame
// of r2 binds A's r2 to it again.
void a_kept_frame_binds_its_room() {
    two_apps t("LLLLLLLLLLLLLLLLLLL1", "MMMMMMMMMMMMMMMMMMM2");
    const ends e(t);
    joined_in(t, {"r1", "r2"});
    t.f.net.inject(e.a(), internal_frame("r2", "@_leave"));
    BOOST_TEST(run_until(t.f, [&] { return t.a_rooms[1].peers().empty(); }));
    t.f.net.inject(e.a(), internal_frame("r2", "@_hsready"));
    BOOST_TEST(run_until(t.f, [&] { return t.a_rooms[1].peers().size() == 1; }, 12s));
    BOOST_TEST_EQ(t.a_rooms[0].peers().size(), 1U);
    BOOST_TEST_EQ(t.f.net.live_connections(), 2U);
}

// A room that is leaving binds no one: B, which joins r2 during A's farewell from it, finds A's
// presence and binds A at once, but A, which hears B's presence and B's handshake then, binds
// neither, so B never sees A join; A's presence(0) then ends B's binding.
void a_leaving_room_binds_no_one() {
    two_apps t("NNNNNNNNNNNNNNNNNNN1", "OOOOOOOOOOOOOOOOOOO2");
    joined_in(t, {"r1"});
    t.a_rooms.push_back(join(t.a, "r2"));
    webcpp::trystero::test::advance(t.f, 200ms);
    bool left = false;
    t.a_rooms[1].async_leave([&left](boost::system::error_code) { left = true; });
    t.b_rooms.push_back(join(t.b, "r2"));
    std::size_t joins = 0;
    t.b_rooms[1].on_peer_join([&joins](std::string_view) { ++joins; });
    BOOST_TEST(run_until(t.f, [&] { return left; }));
    webcpp::trystero::test::advance(t.f, 1000ms);
    BOOST_TEST_EQ(joins, 0U);
    BOOST_TEST(t.b_rooms[1].peers().empty());
    BOOST_TEST_EQ(t.b_rooms[0].peers().size(), 1U);
}

// A forced failure, a frame that does not decode, and a 65th advertised token each end the
// connection for both rooms, which then meet again over one new connection.
void a_connections_end_is_every_rooms() {
    for (int way = 0; way < 3; ++way) {
        two_apps t("JJJJJJJJJJJJJJJJJJJ1", "KKKKKKKKKKKKKKKKKKK2");
        const ends e(t);
        joined_in(t, {"r1", "r2"});
        int leaves = 0;
        for (auto& r : t.a_rooms) {
            r.on_peer_leave([&leaves](std::string_view) { ++leaves; });
        }
        if (way == 0) {
            t.f.net.fail(e.a());
        } else if (way == 1) {
            const std::array<std::uint8_t, 4> bad{3, 0, 1, 'x'};
            t.f.net.inject(e.a(), bad);
        } else {
            for (int i = 0; i < 65; ++i) {
                t.f.net.inject(e.a(),
                               trystero::protocol::presence_frame("X" + std::to_string(i), true));
            }
        }
        BOOST_TEST(run_until(t.f, [&] { return leaves == 2; }));
        BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }, 30s));
        BOOST_TEST(run_until(t.f, [&] { return t.f.net.live_connections() == 2U; }));
    }
}

/** Every relay text of `topics` the watcher reads: whether an offer or an answer was published. */
struct offers_on {
    std::vector<relayed> seen;

    [[nodiscard]] bool offer_or_answer() const {
        return std::ranges::any_of(
            seen, [](const relayed& r) { return !r.offer.empty() || !r.answer.empty(); });
    }
};

// a late join, both directions: r2 joins over the existing connection, no connection is
// made, and no offer and no answer is published on r2's topics for the whole case.
void a_late_join_binds_over_the_connection() {
    for (const bool a_first : {true, false}) {
        two_apps t("LLLLLLLLLLLLLLLLLLLL", "MMMMMMMMMMMMMMMMMMMM");
        t.a_rooms.push_back(join(t.a, "r1"));
        t.b_rooms.push_back(join(t.b, "r1"));
        webcpp::test::require(BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); })));
        flooder watcher(t.f, "r2");
        offers_on r2;
        watcher.on_text = [&](std::string_view text) {
            if (auto s = relayed_signal(text)) {
                r2.seen.push_back(std::move(*s));
            }
        };
        watcher.watch(trystero::protocol::root_topic(app_id, "r2"));
        watcher.watch(trystero::protocol::peer_topic(app_id, "r2", t.a.self_id()));
        watcher.watch(trystero::protocol::peer_topic(app_id, "r2", t.b.self_id()));
        const auto made = t.f.net.made_connections;
        if (a_first) {
            t.a_rooms.push_back(join(t.a, "r2"));
            webcpp::trystero::test::advance(t.f, 500ms);
            t.b_rooms.push_back(join(t.b, "r2"));
        } else {
            t.b_rooms.push_back(join(t.b, "r2"));
            webcpp::trystero::test::advance(t.f, 500ms);
            t.a_rooms.push_back(join(t.a, "r2"));
        }
        BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }));
        webcpp::trystero::test::advance(
            t.f,
            70s);  // past r2's regular announcements, at once, 233, 533, 1333, 5333 ms, 60 s
        BOOST_TEST_EQ(t.f.net.made_connections, made);
        BOOST_TEST(!r2.offer_or_answer());
    }
}

// an announcement forged in X's name in r2, before X joins r2, binds nothing, reports
// nothing and publishes no offer; X then joins within a round trip (no timer runs meanwhile).
void a_forged_announcement_binds_nothing() {
    two_apps t("PPPPPPPPPPPPPPPPPPPP", "QQQQQQQQQQQQQQQQQQQQ");
    joined_in(t, {"r1"});
    t.a_rooms.push_back(join(t.a, "r2"));
    std::vector<boost::system::error_code> errors;
    t.a_rooms[1].on_error([&](std::string_view, boost::system::error_code ec, std::string_view) {
        errors.push_back(ec);
    });
    const auto made = t.f.net.made_connections;
    flooder forger(t.f, "r2");
    forger.announce_as(t.b.self_id(), 3);
    webcpp::trystero::test::advance(t.f, 15s);
    BOOST_TEST(t.a_rooms[1].peers().empty());
    BOOST_TEST(errors.empty());
    BOOST_TEST_EQ(t.f.net.made_connections, made);
    t.b_rooms.push_back(join(t.b, "r2"));
    BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }, 1ms));
}

/** Runs one ready handler at a time until `done` holds, for at most `limit` handlers, moving no
 * clock. Tip: a buffering channel's drain posts itself again, so io_context::poll would drain it
 * whole; this one stops while the channel is still full. */
bool poll_until(fixture& f, const std::function<bool()>& done, std::size_t limit = 1'000'000) {
    for (std::size_t i = 0; i < limit && !done(); ++i) {
        f.io.restart();
        if (f.io.poll_one() == 0) {
            break;
        }
    }
    return done();
}

/** Whether `frame` is a presence frame of `room` saying `present`. */
bool is_presence(std::span<const std::uint8_t> frame, std::string_view room, bool present) {
    const auto env = trystero::protocol::decode_envelope(frame);
    return env && env->type == trystero::protocol::envelope::kind::presence &&
           env->token == trystero::protocol::room_token(app_id, room) && env->present == present;
}

// one room leaves (by async_leave, by its destruction, by a room moved over it): only that
// room sees the peer go, presence(0, r1) arrives, r2 keeps exchanging, the connection stays, and a
// send in the room left completes with room_left.
void one_room_leaves() {
    for (int way = 0; way < 3; ++way) {
        two_apps t("RRRRRRRRRRRRRRRRRRRR", "SSSSSSSSSSSSSSSSSSSS");
        const ends e(t);
        joined_in(t, {"r1", "r2"});
        int b_r1_leaves = 0;
        int b_r2_leaves = 0;
        t.b_rooms[0].on_peer_leave([&](std::string_view) { ++b_r1_leaves; });
        t.b_rooms[1].on_peer_leave([&](std::string_view) { ++b_r2_leaves; });
        bool withdrawn = false;
        t.f.net.filter = [&](std::size_t from, std::size_t, std::span<const std::uint8_t> frame) {
            withdrawn = withdrawn || (from == e.a() && is_presence(frame, "r1", false));
            return trystero::test::memory_network::verdict::deliver;
        };
        auto r1_chat = t.a_rooms[0].make_action<std::string>("chat");
        if (way == 0) {
            bool done = false;
            t.a_rooms[0].async_leave([&](boost::system::error_code) { done = true; });
            BOOST_TEST(run_until(t.f, [&] { return done; }));
        } else if (way == 1) {
            t.a_rooms.erase(t.a_rooms.begin());
        } else {
            t.a_rooms[0] = join(t.a, "elsewhere");
        }
        BOOST_TEST(run_until(t.f, [&] { return b_r1_leaves == 1 && withdrawn; }));
        webcpp::trystero::test::advance(t.f, 1s);
        BOOST_TEST_EQ(b_r2_leaves, 0);
        BOOST_TEST_EQ(t.f.net.live_connections(), 2U);
        std::vector<std::string> got;
        auto in_r2 = t.b_rooms[1].make_action<std::string>("chat");
        in_r2.on_message(
            [&](const std::string& s, const trystero::message_info&) { got.push_back(s); });
        auto& a_r2 = way == 1 ? t.a_rooms[0] : t.a_rooms[1];  // the erase moved r2 to the front
        a_r2.make_action<std::string>("chat").async_send("still", [](boost::system::error_code) {});
        BOOST_TEST(run_until(t.f, [&] { return got.size() == 1; }));
        if (way == 0) {
            std::optional<boost::system::error_code> sent;
            r1_chat.async_send("late", [&](boost::system::error_code ec) { sent = ec; });
            BOOST_TEST(run_until(t.f, [&] { return sent.has_value(); }));
            BOOST_TEST(*sent == trystero::error::room_left);
        }
    }
}

// the peer leaves every room: the connection closes after idle_timeout (200 ms), not
// before, on the virtual clock; a room joined again within it reuses it; with 0 it closes at once.
void a_peer_leaving_every_room_idles_the_connection() {
    for (const auto idle : {200ms, 0ms}) {
        fixture f{true};
        auto a_config = app_config("TTTTTTTTTTTTTTTTTTTT");
        a_config.idle_timeout = idle;
        trystero::app a(f.io.get_executor(), f.net, std::move(a_config));
        trystero::app b(f.io.get_executor(), f.net, app_config("UUUUUUUUUUUUUUUUUUUU"));
        auto a1 = join(a, "r1");
        auto a2 = join(a, "r2");
        std::optional<trystero::room> b1 = join(b, "r1");
        std::optional<trystero::room> b2 = join(b, "r2");
        webcpp::test::require(BOOST_TEST(
            run_until(f, [&] { return a1.peers().size() == 1 && a2.peers().size() == 1; })));
        b1.reset();
        b2.reset();
        BOOST_TEST(run_until(f, [&] { return a1.peers().empty() && a2.peers().empty(); }, 1ms));
        if (idle == 0ms) {
            BOOST_TEST(run_until(f, [&] { return f.net.live_connections() == 0; }, 1ms));
            continue;
        }
        webcpp::trystero::test::advance(f, 100ms);
        BOOST_TEST_EQ(f.net.live_connections(), 2U);
        const auto made = f.net.made_connections;
        b1 = join(b, "r1");  // within the idle time: the connection is reused
        BOOST_TEST(run_until(f, [&] { return a1.peers().size() == 1; }, 1ms));
        BOOST_TEST_EQ(f.net.made_connections, made);
        b1.reset();
        BOOST_TEST(run_until(f, [&] { return a1.peers().empty(); }, 1ms));
        webcpp::trystero::test::advance(f, 199ms);
        BOOST_TEST_EQ(f.net.live_connections(), 2U);  // not before
        webcpp::trystero::test::advance(f, 1ms);
        BOOST_TEST(run_until(f, [&] { return f.net.live_connections() == 0; }, 1ms));
    }
}

/** Drops the first \@_hsdata of r2 on any end, B's challenge when B is the lower id. */
void drop_first_challenge(two_apps& t) {
    const auto dropped = std::make_shared<bool>(false);
    t.f.net.filter = [dropped](std::size_t, std::size_t, std::span<const std::uint8_t> frame) {
        if (*dropped || internal_name(frame, "r2") != "@_hsdata") {
            return trystero::test::memory_network::verdict::deliver;
        }
        *dropped = true;
        return trystero::test::memory_network::verdict::drop;
    };
}

// an action-level kick is per room: an undecodable action frame of r2 kicks B from r2
// only; r1 keeps exchanging.
void an_action_level_kick_is_per_room() {
    two_apps t("CCCCCCCCCCCCCCCCCCC1", "DDDDDDDDDDDDDDDDDDD2");
    const ends e(t);
    joined_in(t, {"r1", "r2"});
    int r2_leaves = 0;
    t.a_rooms[1].on_peer_leave([&](std::string_view) { ++r2_leaves; });
    const std::array<std::uint8_t, 3> garbage{9, 9, 9};
    t.f.net.inject(e.a(), trystero::protocol::room_frame(
                              trystero::protocol::room_token(app_id, "r2"), garbage));
    BOOST_TEST(run_until(t.f, [&] { return r2_leaves == 1; }));
    std::vector<std::string> got;
    auto in_r1 = t.b_rooms[0].make_action<std::string>("chat");
    in_r1.on_message(
        [&](const std::string& s, const trystero::message_info&) { got.push_back(s); });
    t.a_rooms[0].make_action<std::string>("chat").async_send("still",
                                                             [](boost::system::error_code) {});
    BOOST_TEST(run_until(t.f, [&] { return got.size() == 1; }));
    BOOST_TEST_EQ(t.f.net.live_connections(), 2U);
}

// the connection fails while a transfer of r2 waits on the full channel. Its
// end fails the drain waiter, whose completion fails the transfer and may send on the ended
// connection: the transfer completes once with an error, both rooms see B leave, and they meet
// again. Run under shared_hardened and the sanitizers too.
void a_connection_failing_under_a_waiting_transfer() {
    two_apps t("EEEEEEEEEEEEEEEEEEE1", "FFFFFFFFFFFFFFFFFFF2");
    t.f.net.channel_capacity = 1;
    const ends e(t);
    joined_in(t, {"r1", "r2"});
    auto receiver = t.b_rooms[1].make_action<trystero::bytes>("big");
    receiver.on_message([](const trystero::bytes&, const trystero::message_info&) {});
    int completions = 0;
    std::optional<boost::system::error_code> sent;
    t.a_rooms[1].make_action<trystero::bytes>("big").async_send(
        trystero::bytes(1'000'000, std::uint8_t{4}), [&](boost::system::error_code ec) {
            ++completions;
            sent = ec;
        });
    webcpp::test::require(BOOST_TEST(poll_until(t.f, [&] {
        return t.f.net.buffered(e.a()) > std::size_t{64} * 1024;  // a chunk waits on the drain
    })));
    int leaves = 0;
    for (auto& r : t.a_rooms) {
        r.on_peer_leave([&leaves](std::string_view) { ++leaves; });
    }
    t.f.net.fail(e.a());
    BOOST_TEST(run_until(t.f, [&] { return leaves == 2 && sent.has_value(); }));
    BOOST_TEST(*sent == trystero::error::peer_disconnected);
    BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }, 30s));
    BOOST_TEST_EQ(completions, 1);
}

// A kick is per room. B sends an incorrect r2 proof and A is the lower id:
// A reports wrong_password in r2 only and r1 stays joined over the same connection. Then a
// handshake that times out: B the lower id, its challenge dropped once by the filter; B reports
// handshake_timed_out in r2 only, 10 s after binding, and r1 stays. (The rest of case 10, the kick
// that closes the connection and the renegotiation after it use the rebinding times.)
void a_kick_is_per_room() {
    {
        two_apps t("AAAAAAAAAAAAAAAAAAA1", "BBBBBBBBBBBBBBBBBBB2", "pw");
        joined_in(t, {"r1"});
        corrupt_r2_responses(t);
        std::vector<boost::system::error_code> r2_errors;
        t.a_rooms.push_back(join(t.a, "r2"));
        t.a_rooms[1].on_error([&](std::string_view, boost::system::error_code ec,
                                  std::string_view) { r2_errors.push_back(ec); });
        const auto made = t.f.net.made_connections;
        t.b_rooms.push_back(join(t.b, "r2"));
        BOOST_TEST(run_until(t.f, [&] { return !r2_errors.empty(); }));
        BOOST_TEST(r2_errors.at(0) == trystero::error::wrong_password);
        BOOST_TEST_EQ(t.a_rooms[0].peers().size(), 1U);
        BOOST_TEST_EQ(t.b_rooms[0].peers().size(), 1U);
        BOOST_TEST_EQ(t.f.net.made_connections, made);
        BOOST_TEST_EQ(t.f.net.live_connections(), 2U);
    }
    {
        two_apps t("BBBBBBBBBBBBBBBBBBB1", "AAAAAAAAAAAAAAAAAAA2", "pw");  // B the lower id
        joined_in(t, {"r1"});
        drop_first_challenge(t);
        std::vector<boost::system::error_code> b_errors;
        t.a_rooms.push_back(join(t.a, "r2", {.handshake_timeout = 30s}));
        const double joined = t.f.net.now_ms();
        t.b_rooms.push_back(join(t.b, "r2"));
        t.b_rooms[1].on_error([&](std::string_view, boost::system::error_code ec,
                                  std::string_view) { b_errors.push_back(ec); });
        BOOST_TEST(run_until(t.f, [&] { return !b_errors.empty(); }, 11s));
        BOOST_TEST(b_errors.at(0) == trystero::error::handshake_timed_out);
        BOOST_TEST_EQ(t.f.net.now_ms() - joined, 10'000.0);
        BOOST_TEST_EQ(t.a_rooms[0].peers().size(), 1U);
        BOOST_TEST_EQ(t.f.net.live_connections(), 2U);
    }
}

// join of a room id succeeds once async_leave has completed.
void a_room_left_can_be_joined_again() {
    fixture f{true};
    trystero::app a(f.io.get_executor(), f.net, app_config("GGGGGGGGGGGGGGGGGGG1"));
    auto r = join(a, "r");
    bool done = false;
    r.async_leave([&](boost::system::error_code) { done = true; });
    BOOST_TEST(run_until(f, [&] { return done; }));
    BOOST_TEST(a.join("r").has_value());
}

// Review Focus 4 and case 15's farewell half: join, async_leave twice, join during the farewell
// (which finishes it: both leaves complete, once each), the room destroyed during its farewell,
// join again. presence(0) and presence(1) go out in that order, and max_rooms counts a leaving
// room until its presence(0).
void join_and_leave_sequences_on_one_id() {
    two_apps t("IIIIIIIIIIIIIIIIIII1", "JJJJJJJJJJJJJJJJJJJ2");
    const ends e(t);
    joined_in(t, {"lobby"});
    std::vector<bool> presences;  // A's presence frames of r, in the order sent
    t.f.net.filter = [&](std::size_t from, std::size_t, std::span<const std::uint8_t> frame) {
        if (from == e.a() && is_presence(frame, "r", true)) {
            presences.push_back(true);
        } else if (from == e.a() && is_presence(frame, "r", false)) {
            presences.push_back(false);
        }
        return trystero::test::memory_network::verdict::deliver;
    };
    std::vector<trystero::room> others;
    others.reserve(62);
    for (int i = 0; i < 62; ++i) {
        others.push_back(join(t.a, "x" + std::to_string(i)));  // with lobby and r: 64 rooms
    }
    std::optional<trystero::room> r(join(t.a, "r"));
    t.b_rooms.push_back(join(t.b, "r"));
    webcpp::test::require(BOOST_TEST(run_until(t.f, [&] { return r->peers().size() == 1; })));
    int first = 0;
    int second = 0;
    r->async_leave([&](boost::system::error_code ec) { first += ec ? 100 : 1; });
    r->async_leave([&](boost::system::error_code ec) { second += ec ? 100 : 1; });
    const auto refused = t.a.join("x62");
    BOOST_TEST(!refused && refused.error() == trystero::error::too_many_rooms);  // r still counts
    webcpp::trystero::test::advance(t.f, 50ms);
    BOOST_TEST_EQ(first + second, 0);  // the second leave waits for the farewell with the first
    BOOST_TEST((presences == std::vector<bool>{true}));
    auto again = t.a.join("r");  // finishes r's farewell at once
    webcpp::test::require(BOOST_TEST(again.has_value()));
    BOOST_TEST((presences == std::vector<bool>{true, false, true}));
    BOOST_TEST(run_until(t.f, [&] { return first == 1 && second == 1; }, 1ms));
    r.reset();  // the old handle, its room ended: nothing more
    std::optional<trystero::room> r2(std::move(*again));
    BOOST_TEST(run_until(t.f, [&] { return r2->peers().size() == 1; }));
    int third = 0;
    r2->async_leave([&](boost::system::error_code ec) { third += ec ? 100 : 1; });
    r2.reset();  // destroyed during its farewell: the farewell ends at once
    BOOST_TEST((presences == std::vector<bool>{true, false, true, false}));
    const auto last_join = t.a.join("r");
    BOOST_TEST(last_join.has_value());
    BOOST_TEST((presences == std::vector<bool>{true, false, true, false, true}));
    webcpp::trystero::test::advance(t.f, 1s);
    BOOST_TEST_EQ(first, 1);
    BOOST_TEST_EQ(second, 1);
    BOOST_TEST_EQ(third, 1);
}

// during a room's 99 ms farewell, B's presence and B's frames for it bind nothing
// (B binds A on A's earlier presence and is dropped when A's presence(0) goes out); and a
// connection the leaving room's own negotiation opens is closed, not adopted.
void the_farewell_binds_nothing() {
    {
        two_apps t("KKKKKKKKKKKKKKKKKKK1", "LLLLLLLLLLLLLLLLLLL2");
        joined_in(t, {"r1"});
        t.a_rooms.push_back(join(t.a, "r2"));
        bool left = false;
        t.a_rooms[1].async_leave([&](boost::system::error_code) { left = true; });
        int b_joins = 0;
        t.b_rooms.push_back(join(t.b, "r2"));  // B's presence, announcement and handshake now
        t.b_rooms[1].on_peer_join([&](std::string_view) { ++b_joins; });
        BOOST_TEST(run_until(t.f, [&] { return left; }));
        webcpp::trystero::test::advance(t.f, 1s);
        BOOST_TEST(t.a_rooms[1].peers().empty());
        BOOST_TEST(t.b_rooms[1].peers().empty());
        BOOST_TEST_EQ(b_joins, 0);
        BOOST_TEST_EQ(t.f.net.live_connections(), 2U);
    }
    {
        fixture f{true};
        std::vector<std::string> a_log;
        auto a_config = app_config("MMMMMMMMMMMMMMMMMMM1");
        a_config.log = [&a_log](std::string_view, std::string_view text) {
            a_log.emplace_back(text);
        };
        trystero::app a(f.io.get_executor(), f.net, std::move(a_config));
        trystero::app b(f.io.get_executor(), f.net, app_config("NNNNNNNNNNNNNNNNNNN2"));
        std::vector<std::size_t> made;
        f.net.on_made = [&](std::size_t pc, std::span<const trystero::backend::ice_server>) {
            made.push_back(pc);
        };
        f.net.defer_opens = true;
        auto a2 = join(a, "r2");
        const auto b2 = join(b, "r2");
        webcpp::test::require(BOOST_TEST(
            run_until(f, [&] { return made.size() == 2 && f.net.peer_of(made[0]) == made[1]; })));
        bool left = false;
        a2.async_leave([&](boost::system::error_code) { left = true; });
        f.net.open(made[0]);
        f.net.open(made[1]);
        BOOST_TEST(run_until(f, [&] { return left; }));
        webcpp::trystero::test::advance(f, 1s);
        BOOST_TEST(std::ranges::none_of(a_log, [](const std::string& line) {
            return line.find(" adopted") != std::string::npos;
        }));
        BOOST_TEST(b2.peers().empty());
        BOOST_TEST(run_until(f, [&] { return f.net.live_connections() == 0; }, 1s));
    }
}

// the app handle destroyed before its rooms: they go on; the last room destroyed: every
// connection closes at that call.
void the_last_user_closes_every_connection() {
    fixture f{true};
    std::vector<std::string> a_log;
    auto a_config = app_config("OOOOOOOOOOOOOOOOOOO1");
    a_config.log = [&a_log](std::string_view, std::string_view text) { a_log.emplace_back(text); };
    std::optional<trystero::app> a(std::in_place, f.io.get_executor(), f.net, std::move(a_config));
    trystero::app b(f.io.get_executor(), f.net, app_config("PPPPPPPPPPPPPPPPPPP2"));
    std::optional<trystero::room> a1(join(*a, "r1"));
    std::optional<trystero::room> a2(join(*a, "r2"));
    const auto b1 = join(b, "r1");
    const auto b2 = join(b, "r2");
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return a1->peers().size() == 1 && a2->peers().size() == 1; })));
    const auto ended = [&a_log] {
        return std::ranges::count_if(a_log, [](const std::string& line) {
            return line.find(" ended: ") != std::string::npos;
        });
    };
    a.reset();
    a1.reset();
    webcpp::trystero::test::advance(f, 1s);
    BOOST_TEST_EQ(a2->peers().size(), 1U);
    BOOST_TEST_EQ(ended(), 0);
    a2.reset();                 // the last user
    BOOST_TEST_EQ(ended(), 1);  // at that call, before the io_context runs
    BOOST_TEST(run_until(f, [&] { return f.net.live_connections() == 0; }, 1ms));
}

// a room handle destroyed during its
// farewell, the handles gone, then the io_context destroyed with the rooms' operations pending:
// nothing is closed, logged or posted from a destructor, and the app's resource reaches zero.
void the_io_context_destroyed_with_operations_pending() {
    counting_resource memory;
    std::vector<std::string> late;
    bool destroying = false;
    {
        std::optional<fixture> f(std::in_place, true);
        auto a_config = app_config("QQQQQQQQQQQQQQQQQQQ1");
        a_config.memory = &memory;
        a_config.log = [&](std::string_view, std::string_view text) {
            if (destroying) {
                late.emplace_back(text);
            }
        };
        std::optional<trystero::app> a(std::in_place, f->io.get_executor(), f->net,
                                       std::move(a_config));
        std::optional<trystero::app> b(std::in_place, f->io.get_executor(), f->net,
                                       app_config("RRRRRRRRRRRRRRRRRRR2"));
        std::optional<trystero::room> a1(join(*a, "r1"));
        std::optional<trystero::room> a2(join(*a, "r2"));
        std::optional<trystero::room> b1(join(*b, "r1"));
        webcpp::test::require(BOOST_TEST(run_until(*f, [&] { return a1->peers().size() == 1; })));
        a1->async_leave([](boost::system::error_code) {});
        a1.reset();  // during its farewell: ended at once, its completion posted
        a2->async_ping(b->self_id(), [](boost::system::error_code, std::chrono::milliseconds) {});
        a2.reset();
        b1.reset();
        a.reset();
        b.reset();
        destroying = true;
        f.reset();  // the network, then the io_context and the operations still pending in it
    }
    BOOST_TEST(late.empty());
    BOOST_TEST_EQ(memory.in_use, 0U);
}

void the_rebinding_step() {
    using trystero::detail::rebinding_step;
    BOOST_TEST(rebinding_step(10s, 1) == 11s);
    BOOST_TEST(rebinding_step(10s, 2) == 22s);
    BOOST_TEST(rebinding_step(10s, 3) == 44s);
    BOOST_TEST(rebinding_step(10s, 5) == 176s);
    BOOST_TEST(rebinding_step(10s, 6) == 300s);
    BOOST_TEST(rebinding_step(10s, 40) == 300s);
    BOOST_TEST(rebinding_step(2s, 1) == 11s);  // never below Trystero's 10 s keep time
    BOOST_TEST(rebinding_step(30s, 1) == 31s);
    BOOST_TEST(rebinding_step(std::chrono::milliseconds::max(),
                              std::numeric_limits<std::uint32_t>::max()) == 300s);
}

// the lower id reports wrong_password at 0, 11, 33 and 77 s after the first, exactly,
// and the fifth at 165 s, not before; the higher id reports nothing; r2's topics carry the regular
// announcements only, no offer and no answer.
void check_incorrect_proof_rebinding(bool a_lower) {
    two_apps t(a_lower ? "AAAAAAAAAAAAAAAAAAA1" : "ZZZZZZZZZZZZZZZZZZZ1",
               a_lower ? "ZZZZZZZZZZZZZZZZZZZ2" : "AAAAAAAAAAAAAAAAAAA2", "pw");
    joined_in(t, {"r1"});
    corrupt_r2_responses(t);
    flooder watcher(t.f, "r2");
    std::size_t r2_announcements = 0;
    bool offer_or_answer = false;
    watcher.on_text = [&](std::string_view text) {
        if (auto s = relayed_signal(text)) {
            r2_announcements += s->announcement ? 1 : 0;
            offer_or_answer = offer_or_answer || !s->offer.empty() || !s->answer.empty();
        }
    };
    watcher.watch(trystero::protocol::root_topic(app_id, "r2"));
    watcher.watch(trystero::protocol::peer_topic(app_id, "r2", t.a.self_id()));
    watcher.watch(trystero::protocol::peer_topic(app_id, "r2", t.b.self_id()));
    std::vector<double> a_reports;
    std::vector<double> b_reports;
    const auto recorder = [&t](std::vector<double>& to) {
        return [&t, &to](std::string_view, boost::system::error_code ec, std::string_view) {
            if (ec == trystero::error::wrong_password) {
                to.push_back(t.f.net.now_ms());
            }
        };
    };
    t.a_rooms.push_back(join(t.a, "r2"));
    t.b_rooms.push_back(join(t.b, "r2"));
    t.a_rooms[1].on_error(recorder(a_reports));
    t.b_rooms[1].on_error(recorder(b_reports));
    const auto& lower = a_lower ? a_reports : b_reports;
    const auto& higher = a_lower ? b_reports : a_reports;
    webcpp::test::require(BOOST_TEST(run_until(t.f, [&] { return !lower.empty(); }, 1s)));
    const double first = lower.front();
    webcpp::trystero::test::advance(
        t.f,
        std::chrono::milliseconds(static_cast<std::int64_t>(first + 100'000 - t.f.net.now_ms())));
    const std::size_t announced_in_100_s = r2_announcements;
    webcpp::trystero::test::advance(t.f, 64'999ms);  // to 164.999 s after the first
    BOOST_TEST_EQ(lower.size(), 4U);
    const std::array<double, 4> expected{0, 11'000, 33'000, 77'000};
    for (std::size_t i = 0; i < std::min<std::size_t>(lower.size(), 4); ++i) {
        BOOST_TEST_EQ(lower[i] - first, expected[i]);
    }
    BOOST_TEST(run_until(t.f, [&] { return lower.size() == 5; }, 1ms));
    BOOST_TEST_EQ(lower.back() - first, 165'000.0);
    BOOST_TEST(higher.empty());
    BOOST_TEST(!offer_or_answer);
    // Each room's regular cadence on the relay the watcher reads: at once, after 233, 533,
    // 1333 and 5333 ms, then every 60 s: at most 6 each in 100 s, and no restart after a kick.
    BOOST_TEST_LE(announced_in_100_s, 12U);
}

void an_incorrect_proof_rebinds_at_the_steps() {
    for (const bool a_lower : {true, false}) {
        check_incorrect_proof_rebinding(a_lower);
    }
}

// A binds r2 again at its rebinding time, 11 s after its first kick, kicks B, and with
// no room left in common that kick closes the connection at once; the peers then renegotiate
// through r2 with their common app password, and its incorrect proofs still prevent joining. (b)
// Only r2 in common, B the lower id, its challenge dropped once: B's timeout kick closes the
// connection, the rebinding counts go with it, and B renegotiates through r2's signaling: the new
// connection is adopted and bound with a count of zero, so the rooms join within 5 s of the kick,
// before the 11 s a kept count would impose.
void a_kick_that_closes_the_connection() {
    {
        std::optional<double> kicked_connection_at;
        two_apps t("AAAAAAAAAAAAAAAAAAK1", "BBBBBBBBBBBBBBBBBBK2", "pw");
        auto a_config = app_config("AAAAAAAAAAAAAAAAAAK1", "pw");
        a_config.log = [&](std::string_view category, std::string_view text) {
            if (!kicked_connection_at && category == "webrtc" &&
                text.ends_with("ended: its last room kicked the peer")) {
                kicked_connection_at = t.f.net.now_ms();
            }
        };
        t.a = trystero::app(t.f.io.get_executor(), t.f.net, std::move(a_config));
        // B's connections carry B's own ICE server, so the test can tell them apart.
        auto b_config = app_config("BBBBBBBBBBBBBBBBBBK2", "pw");
        b_config.ice_servers = {{.urls = {"stun:b.test"}, .username = {}, .credential = {}}};
        t.b = trystero::app(t.f.io.get_executor(), t.f.net, std::move(b_config));
        std::size_t b_made = 0;
        t.f.net.on_made = [&](std::size_t, std::span<const trystero::backend::ice_server> servers) {
            b_made += !servers.empty() && servers[0].urls.at(0) == "stun:b.test" ? 1 : 0;
        };
        joined_in(t, {"r1"});
        corrupt_r2_responses(t);
        std::vector<double> kicks;
        t.a_rooms.push_back(join(t.a, "r2"));
        t.a_rooms[1].on_error(
            [&](std::string_view, boost::system::error_code ec, std::string_view) {
                if (ec == trystero::error::wrong_password) {
                    kicks.push_back(t.f.net.now_ms());
                }
            });
        t.b_rooms.push_back(join(t.b, "r2"));
        webcpp::test::require(BOOST_TEST(run_until(t.f, [&] { return kicks.size() == 1; }, 1s)));
        t.b_rooms.erase(t.b_rooms.begin());  // r1 left on both sides: r2 is all they share
        t.a_rooms.erase(t.a_rooms.begin());
        const auto b_made_before = b_made;
        BOOST_TEST(run_until(t.f, [&] { return kicked_connection_at.has_value(); }, 12s));
        webcpp::test::require(BOOST_TEST_GE(kicks.size(), 2U));
        BOOST_TEST_EQ(kicks[1] - kicks.front(), 11'000.0);
        BOOST_TEST_EQ(kicked_connection_at.value_or(-1) - kicks.front(), 11'000.0);
        webcpp::trystero::test::advance(t.f, 60s);
        BOOST_TEST_GT(b_made, b_made_before);
        BOOST_TEST(t.a_rooms[0].peers().empty());
    }
    {
        two_apps t("BBBBBBBBBBBBBBBBBBL1", "AAAAAAAAAAAAAAAAAAL2", "pw");  // B the lower id
        drop_first_challenge(t);
        std::vector<double> b_timeouts;
        t.a_rooms.push_back(join(t.a, "r2", {.handshake_timeout = 30s}));
        t.b_rooms.push_back(join(t.b, "r2"));
        t.b_rooms[0].on_error(
            [&](std::string_view, boost::system::error_code ec, std::string_view) {
                if (ec == trystero::error::handshake_timed_out) {
                    b_timeouts.push_back(t.f.net.now_ms());
                }
            });
        webcpp::test::require(
            BOOST_TEST(run_until(t.f, [&] { return t.f.net.live_connections() == 2; }, 1s)));
        const auto made = t.f.net.made_connections;
        webcpp::test::require(BOOST_TEST(run_until(t.f, [&] { return !b_timeouts.empty(); }, 30s)));
        BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }, 5s));
        BOOST_TEST_LT(t.f.net.now_ms() - b_timeouts.front(), 5'000.0);
        BOOST_TEST_GE(t.f.net.made_connections, made + 2);  // a new pair, adopted
        BOOST_TEST(run_until(t.f, [&] { return t.f.net.live_connections() == 2U; }, 1ms));
        BOOST_TEST_EQ(b_timeouts.size(), 1U);
    }
}

/** Holds, at the receiving end, the first frame from `from` that carries the internal action
 * `message` of r2, and, as the channel delivers in order, every later frame of that end .
 * Counts, in `challenges`, every \@_hsdata of r2 that `counted` sends. */
void hold_first(two_apps& t, std::size_t from, std::string message, std::size_t counted = 0,
                std::size_t* challenges = nullptr) {
    const auto held = std::make_shared<bool>(false);
    t.f.net.filter = [from, message = std::move(message), held, counted, challenges](
                         std::size_t sender, std::size_t, std::span<const std::uint8_t> frame) {
        const auto name = internal_name(frame, "r2");
        if (challenges != nullptr && sender == counted && name == "@_hsdata") {
            ++*challenges;
        }
        if (*held || sender != from || name != message) {
            return trystero::test::memory_network::verdict::deliver;
        }
        *held = true;
        return trystero::test::memory_network::verdict::hold;
    };
}

// binding needs the peer's own presence. The filter drops B's presence(1, r2) while B
// joins r2, A having joined it first: B binds at its join, on A's presence, and its handshake
// frames reach A, which keeps them and does not bind; B's announcements in r2 bind nothing and
// start no negotiation. B's handshake times out 10 s after its join and B, the higher id, kicks;
// at its rebinding time, 21 s after its join, B binds again and sends its presence, which goes
// through now: A binds at that instant.
void binding_needs_the_peers_own_presence() {
    two_apps t("NNNNNNNNNNNNNNNNNNNN", "OOOOOOOOOOOOOOOOOOOO");
    const ends e(t);
    joined_in(t, {"r1"});
    t.a_rooms.push_back(join(t.a, "r2"));
    webcpp::trystero::test::advance(t.f, 300ms);
    bool drop = true;
    t.f.net.filter = [&](std::size_t from, std::size_t, std::span<const std::uint8_t> frame) {
        return drop && from != e.a() && is_presence(frame, "r2", true)
                   ? trystero::test::memory_network::verdict::drop
                   : trystero::test::memory_network::verdict::deliver;
    };
    flooder watcher(t.f, "r2");
    bool offer_or_answer = false;
    watcher.on_text = [&](std::string_view text) {
        if (auto s = relayed_signal(text)) {
            offer_or_answer = offer_or_answer || !s->offer.empty() || !s->answer.empty();
        }
    };
    watcher.watch(trystero::protocol::peer_topic(app_id, "r2", t.a.self_id()));
    watcher.watch(trystero::protocol::peer_topic(app_id, "r2", t.b.self_id()));
    std::vector<boost::system::error_code> b_errors;
    const double joined = t.f.net.now_ms();
    t.b_rooms.push_back(join(t.b, "r2"));
    t.b_rooms[1].on_error([&](std::string_view, boost::system::error_code ec, std::string_view) {
        b_errors.push_back(ec);
    });
    webcpp::trystero::test::advance(t.f, 10'999ms);
    BOOST_TEST(t.a_rooms[1].peers().empty());
    BOOST_TEST((b_errors == std::vector<boost::system::error_code>{
                                make_error_code(trystero::error::handshake_timed_out)}));
    drop = false;
    BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }, 11s));
    BOOST_TEST_EQ(t.f.net.now_ms() - joined, 21'000.0);
    BOOST_TEST(!offer_or_answer);
}

void check_transient_failure(bool password, bool lower_times_out, bool release_after_rebinding) {
    two_apps t("AAAAAAAAAAAAAAAAAAD1", "ZZZZZZZZZZZZZZZZZZD2",
               password ? "p" : "");  // A the lower id
    const ends e(t);
    joined_in(t, {"r1"});
    const auto a_end = e.a();
    const auto b_end = t.f.net.peer_of(a_end);
    // A times out on B's answer (without a password, B's @_hsready); B on A's
    // challenge (or A's @_hsready).
    hold_first(t, lower_times_out ? b_end : a_end, password ? "@_hsdata" : "@_hsready");
    const trystero::room_options patient{
        .handshake_timeout = 30s,
    };
    const trystero::room_options normal{};
    std::vector<boost::system::error_code> a_errors;
    std::vector<boost::system::error_code> b_errors;
    t.a_rooms.push_back(join(t.a, "r2", lower_times_out ? normal : patient));
    t.b_rooms.push_back(join(t.b, "r2", lower_times_out ? patient : normal));
    t.a_rooms[1].on_error([&](std::string_view, boost::system::error_code ec, std::string_view) {
        a_errors.push_back(ec);
    });
    t.b_rooms[1].on_error([&](std::string_view, boost::system::error_code ec, std::string_view) {
        b_errors.push_back(ec);
    });
    const auto& timed_out = lower_times_out ? a_errors : b_errors;
    webcpp::test::require(BOOST_TEST(run_until(t.f, [&] { return !timed_out.empty(); }, 11s)));
    const double kick = t.f.net.now_ms();
    const std::chrono::milliseconds release_after = release_after_rebinding ? 13s : 2s;
    webcpp::trystero::test::advance(t.f, release_after);
    t.f.net.filter = {};
    t.f.net.release(lower_times_out ? a_end : b_end);  // held at the receiving end
    BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }, 12s));
    BOOST_TEST_EQ(t.f.net.now_ms() - kick,
                  static_cast<double>(std::max<std::int64_t>(11'000, release_after.count())));
    BOOST_TEST_EQ(timed_out.size(), 1U);
    BOOST_TEST(timed_out.at(0) == trystero::error::handshake_timed_out);
    BOOST_TEST_EQ(a_errors.size() + b_errors.size(), 1U);  // no other report
}

void a_transient_failure_converges() {
    for (const bool password : {true, false}) {
        for (const bool lower_times_out : {true, false}) {
            for (const bool release_after_rebinding : {false, true}) {
                check_transient_failure(password, lower_times_out, release_after_rebinding);
            }
        }
    }
}

// an answer to a challenge two bindings old passes a bound
// initiator; one to a challenge nine bindings old, no longer among the last 8, is wrong_password.
void an_answer_to_an_old_challenge() {
    for (const std::size_t binding : {3U, 9U}) {
        two_apps t("AAAAAAAAAAAAAAAAAAJ1", "ZZZZZZZZZZZZZZZZZZJ2", "pw");
        const ends e(t);
        joined_in(t, {"r1"});
        const auto a_end = e.a();
        const auto b_end = t.f.net.peer_of(a_end);
        std::size_t a_challenges = 0;
        hold_first(t, b_end, "@_hsdata", a_end, &a_challenges);  // B's first answer and after
        std::vector<boost::system::error_code> a_errors;
        t.a_rooms.push_back(join(t.a, "r2"));
        t.b_rooms.push_back(join(t.b, "r2", {.handshake_timeout = 30s}));
        t.a_rooms[1].on_error([&](std::string_view, boost::system::error_code ec,
                                  std::string_view) { a_errors.push_back(ec); });
        // A's bindings start at 0, 21, 53, 107, 205, 391, 701, 1011 and 1321 s.
        webcpp::test::require(
            BOOST_TEST(run_until(t.f, [&] { return a_challenges == binding; }, 1400s)));
        BOOST_TEST_EQ(std::ranges::count(a_errors, trystero::error::handshake_timed_out),
                      static_cast<std::ptrdiff_t>(binding - 1));
        t.f.net.filter = {};
        t.f.net.release(a_end);
        const auto wrong = [&] {
            return std::ranges::count(a_errors, trystero::error::wrong_password);
        };
        if (binding == 3) {
            BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }, 1ms));
            BOOST_TEST_EQ(wrong(), 0);
        } else {
            BOOST_TEST(run_until(t.f, [&] { return wrong() == 1; }, 1ms));
            BOOST_TEST(t.a_rooms[1].peers().empty());
        }
    }
}

// A is the lower id and r2 responses are corrupted: after A's third kick at 33 s, B leaves r2
// and rejoins with intact responses at 35 s; B joins at 77 s, A's rebinding time, exactly, having
// reported at most two handshake_timed_out. With B the lower id rejecting corrupted responses, B's
// new join joins at once: its rebinding count was its old room's.
void a_corrected_response_rejoins() {
    {
        two_apps t("AAAAAAAAAAAAAAAAAAB1", "ZZZZZZZZZZZZZZZZZZB2", "pw");
        joined_in(t, {"r1"});
        corrupt_r2_responses(t);
        std::vector<double> a_kicks;
        t.a_rooms.push_back(join(t.a, "r2"));
        t.a_rooms[1].on_error(
            [&](std::string_view, boost::system::error_code ec, std::string_view) {
                if (ec == trystero::error::wrong_password) {
                    a_kicks.push_back(t.f.net.now_ms());
                }
            });
        std::optional<trystero::room> b2(join(t.b, "r2"));
        webcpp::test::require(BOOST_TEST(run_until(t.f, [&] { return a_kicks.size() == 3; }, 40s)));
        const double first = a_kicks.front();
        BOOST_TEST_EQ(a_kicks.back() - first, 33'000.0);
        webcpp::trystero::test::advance(t.f, 2s);
        b2.reset();
        t.f.net.filter = {};
        b2.emplace(join(t.b, "r2"));
        std::vector<boost::system::error_code> b_errors;
        b2->on_error([&](std::string_view, boost::system::error_code ec, std::string_view) {
            b_errors.push_back(ec);
        });
        BOOST_TEST(run_until(t.f, [&] { return b2->peers().size() == 1; }, 45s));
        BOOST_TEST_EQ(t.f.net.now_ms() - first, 77'000.0);
        BOOST_TEST_LE(std::ranges::count(b_errors, trystero::error::handshake_timed_out), 2);
        BOOST_TEST_EQ(a_kicks.size(), 3U);
    }
    {
        two_apps t("ZZZZZZZZZZZZZZZZZZC1", "AAAAAAAAAAAAAAAAAAC2", "pw");  // B the lower id
        joined_in(t, {"r1"});
        corrupt_r2_responses(t);
        std::vector<boost::system::error_code> b_errors;
        t.a_rooms.push_back(join(t.a, "r2"));
        std::optional<trystero::room> b2(join(t.b, "r2"));
        b2->on_error([&](std::string_view, boost::system::error_code ec, std::string_view) {
            b_errors.push_back(ec);
        });
        webcpp::test::require(BOOST_TEST(run_until(t.f, [&] { return !b_errors.empty(); }, 1s)));
        webcpp::trystero::test::advance(t.f, 3s);
        b2.reset();
        t.f.net.filter = {};
        b2.emplace(join(t.b, "r2"));
        BOOST_TEST(run_until(t.f, [&] { return b2->peers().size() == 1; }, 1ms));
    }
}

// before A's r2 binds B, inject delivers on A's end B's @_hsready and @_leave of r2; then
// B's presence is let through: A binds, discards both, and the handshake completes on B's next
// frames, with no leave: A's log names no peer gone.
void a_kept_leave_discards_what_came_before() {
    std::vector<std::string> a_log;
    auto a_config = app_config("AAAAAAAAAAAAAAAAAAE1");
    a_config.log = [&a_log](std::string_view, std::string_view text) { a_log.emplace_back(text); };
    two_apps t("AAAAAAAAAAAAAAAAAAE1", "ZZZZZZZZZZZZZZZZZZE2");
    t.a = trystero::app(t.f.io.get_executor(), t.f.net, std::move(a_config));
    const ends e(t);
    joined_in(t, {"r1"});
    bool hold_presence = true;
    t.f.net.filter = [&](std::size_t, std::size_t, std::span<const std::uint8_t> frame) {
        return hold_presence && is_presence(frame, "r2", true)
                   ? trystero::test::memory_network::verdict::hold
                   : trystero::test::memory_network::verdict::deliver;
    };
    t.a_rooms.push_back(join(t.a, "r2"));
    t.b_rooms.push_back(join(t.b, "r2"));
    for (const auto* name : {"@_hsready", "@_leave"}) {
        t.f.net.inject(e.a(), internal_frame("r2", name));
    }
    webcpp::trystero::test::advance(t.f, 200ms);
    BOOST_TEST(t.a_rooms[1].peers().empty());
    hold_presence = false;
    t.f.net.release(e.a());
    t.f.net.release(t.f.net.peer_of(e.a()));
    BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }, 1ms));
    BOOST_TEST(std::ranges::none_of(
        a_log, [](const std::string& line) { return line.find(" gone: ") != std::string::npos; }));
}

// crossed connections recover: A's end of r1's pair and B's end of r2's open first; both
// sides lose both, then both rooms join again within 15 s of virtual time, over one connection.
void crossed_connections_recover() {
    fixture f{true};
    auto a_config = app_config("AAAAAAAAAAAAAAAAAAF1");
    a_config.ice_servers = {{.urls = {"stun:a.test"}, .username = {}, .credential = {}}};
    auto b_config = app_config("ZZZZZZZZZZZZZZZZZZF2");
    b_config.ice_servers = {{.urls = {"stun:b.test"}, .username = {}, .credential = {}}};
    trystero::app a(f.io.get_executor(), f.net, std::move(a_config));
    trystero::app b(f.io.get_executor(), f.net, std::move(b_config));
    std::vector<std::size_t> a_made;
    f.net.on_made = [&](std::size_t pc, std::span<const trystero::backend::ice_server> servers) {
        if (!servers.empty() && servers[0].urls.at(0) == "stun:a.test") {
            a_made.push_back(pc);
        }
    };
    f.net.defer_opens = true;
    auto a1 = join(a, "r1");
    auto b1 = join(b, "r1");
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return !a_made.empty() && f.net.peer_of(a_made[0]) != a_made[0]; })));
    const auto a_r1 = a_made[0];
    const auto b_r1 = f.net.peer_of(a_r1);
    auto a2 = join(a, "r2");
    auto b2 = join(b, "r2");
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return a_made.size() >= 2 && f.net.peer_of(a_made[1]) != a_made[1]; })));
    const auto a_r2 = a_made[1];
    const auto b_r2 = f.net.peer_of(a_r2);
    f.net.open(a_r1);
    f.net.open(b_r2);
    webcpp::trystero::test::advance(f, 100ms);
    f.net.open(b_r1);
    f.net.open(a_r2);
    f.net.defer_opens = false;
    const double opened = f.net.now_ms();
    BOOST_TEST(run_until(
        f,
        [&] {
            return a1.peers().size() == 1 && a2.peers().size() == 1 && b1.peers().size() == 1 &&
                   b2.peers().size() == 1;
        },
        15s));
    BOOST_TEST_LE(f.net.now_ms() - opened, 15'000.0);
    BOOST_TEST(run_until(f, [&] { return f.net.live_connections() == 2U; }));
}

// flood isolation. B floods r1 with 10,000 inline messages to an action without a handler
// (A's queue keeps 1024) while r2 pings and chats: every r2 message arrives and every ping
// completes without error while r1's flood is queued. The virtual clock stays fixed while posts
// run; the interop test measures wall-clock latency.
// Then, with a buffering channel, B sends a 5 MB transfer in r1 and, once
// the channel holds more than 64 KiB, 20 chats in r2, which must wait for the drain :
// none enters the channel at once, and every chat completes before the transfer does.
void a_flood_in_one_room_does_not_starve_another() {
    two_apps t("AAAAAAAAAAAAAAAAAAG1", "ZZZZZZZZZZZZZZZZZZG2");
    const ends e(t);
    joined_in(t, {"r1", "r2"});
    const auto b_end = t.f.net.peer_of(e.a());
    static_cast<void>(t.a_rooms[0].make_action<std::string>("unread"));  // made, no handler: queued
    auto flood = t.b_rooms[0].make_action<std::string>("unread");
    for (int i = 0; i < 10'000; ++i) {
        flood.async_send("x", [](boost::system::error_code) {});
    }
    std::vector<std::string> r2_got;
    auto r2_in = t.a_rooms[1].make_action<std::string>("chat");
    r2_in.on_message(
        [&](const std::string& s, const trystero::message_info&) { r2_got.push_back(s); });
    auto r2_out = t.b_rooms[1].make_action<std::string>("chat");
    for (int i = 0; i < 100; ++i) {
        r2_out.async_send(std::to_string(i), [](boost::system::error_code) {});
    }
    std::vector<std::chrono::milliseconds> pings;
    for (int i = 0; i < 5; ++i) {
        t.b_rooms[1].async_ping(t.a.self_id(),
                                [&](boost::system::error_code ec, std::chrono::milliseconds rtt) {
                                    if (!ec) {
                                        pings.push_back(rtt);
                                    }
                                });
    }
    // A ping is recorded only without an error: five of them, and every r2 message, arrive while
    // r1's 10,000 are queued at A.
    BOOST_TEST(run_until(t.f, [&] { return pings.size() == 5 && r2_got.size() == 100; }, 10s));
    t.f.net.channel_capacity = 1;
    bool transfer_done = false;
    std::size_t chats_done = 0;
    bool chats_first = false;
    auto big_in = t.a_rooms[0].make_action<trystero::bytes>("big");
    big_in.on_message([](const trystero::bytes&, const trystero::message_info&) {});
    t.b_rooms[0].make_action<trystero::bytes>("big").async_send(
        trystero::bytes(5'000'000, std::uint8_t{1}),
        [&](boost::system::error_code) { transfer_done = true; });
    webcpp::test::require(BOOST_TEST(
        poll_until(t.f, [&] { return t.f.net.buffered(b_end) > std::size_t{64} * 1024; })));
    r2_got.clear();
    const auto full = t.f.net.buffered(b_end);
    for (int i = 0; i < 20; ++i) {
        r2_out.async_send("c", [&](boost::system::error_code) {
            if (++chats_done == 20) {
                chats_first = !transfer_done;
            }
        });
    }
    BOOST_TEST_EQ(t.f.net.buffered(b_end), full);  // the chats wait for the drain, not inline
    BOOST_TEST(run_until(t.f, [&] { return transfer_done && chats_done == 20; }, 60s));
    BOOST_TEST(chats_first);
    BOOST_TEST_EQ(r2_got.size(), 20U);
}

// A public app's rooms share one admission domain: B, already in lobby, can announce secret
// and complete its passwordless handshake, so secret's broadcasts reach that shared peer.
void a_passwordless_room_is_open_to_the_apps_other_rooms() {
    two_apps t("AAAAAAAAAAAAAAAAAAH1", "ZZZZZZZZZZZZZZZZZZH2");
    const ends e(t);
    t.a_rooms.push_back(join(t.a, "lobby"));
    t.a_rooms.push_back(join(t.a, "secret"));
    t.b_rooms.push_back(join(t.b, "lobby"));
    webcpp::test::require(BOOST_TEST(run_until(t.f, [&] {
        return t.a_rooms[0].peers().size() == 1 && t.b_rooms[0].peers().size() == 1;
    })));
    std::size_t secret_broadcasts = 0;
    t.f.net.filter = [&](std::size_t from, std::size_t, std::span<const std::uint8_t> frame) {
        secret_broadcasts += from == e.a() && internal_name(frame, "secret") == "chat" ? 1 : 0;
        return trystero::test::memory_network::verdict::deliver;
    };
    int secret_joins = 0;
    t.a_rooms[1].on_peer_join([&](std::string_view) { ++secret_joins; });
    t.f.net.inject(e.a(), trystero::protocol::presence_frame(
                              trystero::protocol::room_token(app_id, "secret"), true));
    t.f.net.inject(e.a(), internal_frame("secret", "@_hsready"));
    BOOST_TEST(run_until(t.f, [&] { return secret_joins == 1; }, 1ms));
    t.a_rooms[1].make_action<std::string>("chat").async_send("to all",
                                                             [](boost::system::error_code) {});
    BOOST_TEST(run_until(t.f, [&] { return secret_broadcasts == 1; }, 1ms));
}

// A peer authenticated on r1 can still send a missing or incorrect room proof on r2. Those
// failures keep their precise timeout and error behavior without mixing password domains.
void invalid_room_proofs_fail_within_one_password_domain() {
    for (const bool wrong_response : {false, true}) {
        two_apps t("AAAAAAAAAAAAAAAAAAH3", "ZZZZZZZZZZZZZZZZZZH4", "pw");
        joined_in(t, {"r1"});
        if (wrong_response) {
            corrupt_r2_responses(t);
        } else {
            drop_first_challenge(t);
        }
        t.a_rooms.push_back(join(t.a, "r2"));
        std::vector<boost::system::error_code> errors;
        t.a_rooms[1].on_error([&](std::string_view, boost::system::error_code ec,
                                  std::string_view) { errors.push_back(ec); });
        const double joined = t.f.net.now_ms();
        t.b_rooms.push_back(join(t.b, "r2", {.handshake_timeout = 30s}));
        webcpp::test::require(BOOST_TEST(run_until(t.f, [&] { return !errors.empty(); }, 11s)));
        BOOST_TEST(errors.front() == (wrong_response ? trystero::error::wrong_password
                                                     : trystero::error::handshake_timed_out));
        BOOST_TEST_EQ(t.f.net.now_ms() - joined, wrong_response ? 0.0 : 10'000.0);
        BOOST_TEST(t.a_rooms[1].peers().empty());
        BOOST_TEST_EQ(t.a_rooms[0].peers().size(), 1U);
    }
}

// A's r2, in a resource of its own, sends 1 MB that waits on the full channel; A destroys
// r2 while r1 goes on: once the posts have run, r2's resource has no bytes out, and r1 still
// exchanges.
void a_rooms_own_resource_returns_to_zero() {
    counting_resource r2_memory;
    two_apps t("AAAAAAAAAAAAAAAAAAI1", "ZZZZZZZZZZZZZZZZZZI2");
    const ends e(t);
    t.a_rooms.push_back(join(t.a, "r1"));
    t.b_rooms.push_back(join(t.b, "r1"));
    std::optional<trystero::room> r2(join(t.a, "r2", {.memory = &r2_memory}));
    t.b_rooms.push_back(join(t.b, "r2"));
    webcpp::test::require(
        BOOST_TEST(run_until(t.f, [&] { return t.all_joined() && r2->peers().size() == 1; })));
    auto big_in = t.b_rooms[1].make_action<trystero::bytes>("big");
    big_in.on_message([](const trystero::bytes&, const trystero::message_info&) {});
    t.f.net.channel_capacity = 1;
    r2->make_action<trystero::bytes>("big").async_send(trystero::bytes(1'000'000, std::uint8_t{3}),
                                                       [](boost::system::error_code) {});
    webcpp::test::require(BOOST_TEST(
        poll_until(t.f, [&] { return t.f.net.buffered(e.a()) > std::size_t{64} * 1024; })));
    r2.reset();
    webcpp::trystero::test::advance(t.f, 1s);
    BOOST_TEST_EQ(r2_memory.in_use, 0U);
    std::vector<std::string> got;
    auto chat = t.b_rooms[0].make_action<std::string>("chat");
    chat.on_message([&](const std::string& s, const trystero::message_info&) { got.push_back(s); });
    t.a_rooms[0].make_action<std::string>("chat").async_send("after",
                                                             [](boost::system::error_code) {});
    BOOST_TEST(run_until(t.f, [&] { return got.size() == 1; }));
}

// Construction reserves the room before invoking a user log sink, including a reentrant join.
void a_joining_log_cannot_join_the_same_room_twice() {
    fixture f{true};
    bool tried = false;
    std::optional<boost::system::error_code> refused;
    std::optional<trystero::room> duplicate;
    std::optional<trystero::app> app;
    auto config = app_config("AAAAAAAAAAAAAAAAAAT1");
    config.log = [&](std::string_view, std::string_view text) {
        if (!tried && app && text.starts_with("joining room r ")) {
            tried = true;
            auto again = app->join("r");
            if (again) {
                duplicate.emplace(std::move(*again));
            } else {
                refused = again.error();
            }
        }
    };
    app.emplace(f.io.get_executor(), f.net, std::move(config));
    const auto room = join(*app, "r");
    BOOST_TEST(tried);
    BOOST_TEST(refused.has_value());
    if (refused) {
        BOOST_TEST(*refused == trystero::error::already_joined);
    }
    BOOST_TEST(!duplicate);
}

// A rejoin from the farewell log completes the old farewell; it must not gain a new timer when
// that log returns to the original leave call.
void a_leaving_log_can_finish_the_farewell_by_rejoining() {
    fixture f{true};
    bool acted = false;
    std::size_t timers_after_rejoin = 0;
    std::optional<trystero::room> replacement;
    std::optional<trystero::app> app;
    auto config = app_config("AAAAAAAAAAAAAAAAAAU1");
    config.log = [&](std::string_view, std::string_view text) {
        if (!acted && text == "leaving room r") {
            acted = true;
            replacement.emplace(join(*app, "r"));
            timers_after_rejoin = f.net.scheduled();
        }
    };
    app.emplace(f.io.get_executor(), f.net, std::move(config));
    auto room = join(*app, "r");
    int completions = 0;
    room.async_leave([&](boost::system::error_code ec) {
        BOOST_TEST(!ec);
        ++completions;
    });
    BOOST_TEST(acted);
    BOOST_TEST_EQ(f.net.scheduled(), timers_after_rejoin);
    webcpp::trystero::test::advance(f, 200ms);
    BOOST_TEST_EQ(completions, 1);
    BOOST_TEST(replacement.has_value());
}

// A join log runs user code before the join notification is posted. Leaving there must prevent
// that notification, and destroying the room must not invalidate the peer identifier in use.
void check_leaving_from_join_log(bool destroy) {
    fixture f{true};
    bool acted = false;
    int joins = 0;
    std::optional<trystero::room> room;
    auto config = app_config("AAAAAAAAAAAAAAAAAAQ1");
    config.log = [&](std::string_view, std::string_view text) {
        if (!acted && room && text.starts_with("peer joined: ")) {
            acted = true;
            if (destroy) {
                room.reset();
            } else {
                room->async_leave([](boost::system::error_code) {});
            }
        }
    };
    trystero::app a(f.io.get_executor(), f.net, std::move(config));
    trystero::app b(f.io.get_executor(), f.net, app_config("ZZZZZZZZZZZZZZZZZZQ2"));
    room = join(a, "r");
    room->on_peer_join([&](std::string_view) { ++joins; });
    const auto remote = join(b, "r");
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return acted; })));
    webcpp::trystero::test::advance(f, 200ms);
    BOOST_TEST_EQ(joins, 0);
    BOOST_TEST(remote.peers().empty());
}

void a_join_log_can_leave_or_destroy_its_room() {
    for (const bool destroy : {true, false}) {
        check_leaving_from_join_log(destroy);
    }
}

// The error callback selected after logging receives owned diagnostic text even if the sink
// destroys the offending binding. A replacement callback installed by the sink takes effect.
void check_leaving_from_error_log(bool destroy) {
    bool acted = false;
    int old_errors = 0;
    int new_errors = 0;
    std::optional<trystero::room> room;
    two_apps t("AAAAAAAAAAAAAAAAAAR1", "ZZZZZZZZZZZZZZZZZZR2", "pw");
    auto config = app_config("AAAAAAAAAAAAAAAAAAR1", "pw");
    config.log = [&](std::string_view, std::string_view text) {
        if (!acted && room && text.ends_with("incorrect password for overlapping room")) {
            acted = true;
            room->on_error(
                [&](std::string_view peer, boost::system::error_code ec, std::string_view what) {
                    ++new_errors;
                    BOOST_TEST_EQ(peer, "ZZZZZZZZZZZZZZZZZZR2");
                    BOOST_TEST(ec == trystero::error::wrong_password);
                    BOOST_TEST_EQ(what, "incorrect password for overlapping room");
                });
            if (destroy) {
                room.reset();
            } else {
                room->async_leave([](boost::system::error_code) {});
            }
        }
    };
    t.a = trystero::app(t.f.io.get_executor(), t.f.net, std::move(config));
    joined_in(t, {"r1"});
    corrupt_r2_responses(t);
    room = join(t.a, "r2");
    room->on_error(
        [&](std::string_view, boost::system::error_code, std::string_view) { ++old_errors; });
    t.b_rooms.push_back(join(t.b, "r2"));
    webcpp::test::require(BOOST_TEST(run_until(t.f, [&] { return acted; })));
    webcpp::trystero::test::advance(t.f, 200ms);
    BOOST_TEST_EQ(old_errors, 0);
    BOOST_TEST_EQ(new_errors, 1);
    BOOST_TEST_EQ(t.a_rooms[0].peers().size(), 1U);
}

void an_error_log_can_leave_or_destroy_its_room() {
    for (const bool destroy : {true, false}) {
        check_leaving_from_error_log(destroy);
    }
}

// Installing a handler from the queue-limit log drains the existing queue synchronously. The
// in-flight message must use that handler, or be discarded when the sink also leaves the room.
void check_draining_from_queue_log(bool leave) {
    fixture f{true};
    bool acted = false;
    std::size_t received = 0;
    std::optional<trystero::room> room;
    std::optional<trystero::action<std::string>> inbox;
    auto config = app_config("AAAAAAAAAAAAAAAAAAS1");
    config.log = [&](std::string_view, std::string_view text) {
        if (!acted && text.starts_with("dropping the oldest unread message of ")) {
            acted = true;
            inbox->on_message(
                [&](const std::string&, const trystero::message_info&) { ++received; });
            if (leave) {
                room.reset();
            }
        }
    };
    trystero::app a(f.io.get_executor(), f.net, std::move(config));
    trystero::app b(f.io.get_executor(), f.net, app_config("ZZZZZZZZZZZZZZZZZZS2"));
    room = join(a, "r");
    auto remote = join(b, "r");
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return room->peers().size() == 1 && remote.peers().size() == 1; })));
    inbox.emplace(room->make_action<std::string>("unread"));
    auto out = remote.make_action<std::string>("unread");
    for (int i = 0; i < 1025; ++i) {
        out.async_send("x", [](boost::system::error_code) {});
    }
    webcpp::test::require(BOOST_TEST(run_until(f, [&] { return acted; })));
    webcpp::trystero::test::advance(f, 200ms);
    BOOST_TEST_EQ(received, leave ? 1024U : 1025U);
}

void a_queue_log_can_drain_or_destroy_its_room() {
    for (const bool leave : {false, true}) {
        check_draining_from_queue_log(leave);
    }
}

// A synchronous log sink may end the room whose connection is being adopted. Neither a
// destroyed room nor one in its farewell may regain a binding after the sink returns.
void a_log_sink_can_leave_during_adoption() {
    for (const bool destroy : {false, true}) {
        fixture f{true};
        std::optional<trystero::room> leaving;
        bool acted = false;
        auto config = app_config("AAAAAAAAAAAAAAAAAAL1");
        config.idle_timeout = 200ms;
        config.log = [&](std::string_view category, std::string_view text) {
            if (!acted && leaving && category == "webrtc" && text.ends_with(" adopted")) {
                acted = true;
                if (destroy) {
                    leaving.reset();
                } else {
                    leaving->async_leave([](boost::system::error_code) {});
                }
            }
        };
        trystero::app a(f.io.get_executor(), f.net, std::move(config));
        trystero::app b(f.io.get_executor(), f.net, app_config("ZZZZZZZZZZZZZZZZZZL2"));
        const auto survivor = join(a, "survivor");
        leaving = join(a, "departing");
        auto remote = join(b, "departing");
        int remote_joins = 0;
        remote.on_peer_join([&](std::string_view) { ++remote_joins; });
        webcpp::test::require(BOOST_TEST(run_until(f, [&] { return acted; }, 1s)));
        webcpp::trystero::test::advance(f, 1ms);
        if (leaving) {
            BOOST_TEST(leaving->peers().empty());
        }
        BOOST_TEST_EQ(remote_joins, 0);
        webcpp::trystero::test::advance(f, 200ms);
        BOOST_TEST_EQ(remote_joins, 0);
        webcpp::trystero::test::advance(f, 200ms);
        BOOST_TEST_EQ(f.net.live_connections(), 0U);
        auto other = join(b, "survivor");
        BOOST_TEST(run_until(
            f, [&] { return survivor.peers().size() == 1 && other.peers().size() == 1; }));
    }
}

// Destroying another room from the adoption log must not leave a dangling token in the
// advertised snapshot, or advertise that departed room after its withdrawal.
void a_log_sink_can_destroy_another_room_during_adoption() {
    fixture f{true};
    std::optional<trystero::room> retired;
    bool acted = false;
    auto config = app_config("AAAAAAAAAAAAAAAAAAM1");
    config.idle_timeout = 0ms;
    config.log = [&](std::string_view category, std::string_view text) {
        if (!acted && retired && category == "webrtc" && text.ends_with(" adopted")) {
            acted = true;
            retired.reset();
        }
    };
    trystero::app a(f.io.get_executor(), f.net, std::move(config));
    trystero::app b(f.io.get_executor(), f.net, app_config("ZZZZZZZZZZZZZZZZZZM2"));
    retired = join(a, "retired");
    auto lobby = join(a, "lobby");
    auto remote = join(b, "lobby");
    std::size_t invalid_frames = 0;
    std::size_t retired_after_leave = 0;
    const auto retired_token = trystero::protocol::room_token(app_id, "retired");
    f.net.filter = [&](std::size_t, std::size_t, std::span<const std::uint8_t> frame) {
        const auto env = trystero::protocol::decode_envelope(frame);
        invalid_frames += !env ? 1 : 0;
        if (acted && env && env->type == trystero::protocol::envelope::kind::presence &&
            env->token == retired_token && env->present) {
            ++retired_after_leave;
        }
        return trystero::test::memory_network::verdict::deliver;
    };
    BOOST_TEST(
        run_until(f, [&] { return lobby.peers().size() == 1 && remote.peers().size() == 1; }));
    BOOST_TEST(acted);
    BOOST_TEST_EQ(invalid_frames, 0U);
    BOOST_TEST_EQ(retired_after_leave, 0U);
    f.net.filter = {};
}

// A password challenge has a string nonce, as Trystero's handshake requires. A number or
// null must not be silently converted into an empty challenge by the responder.
void a_password_challenge_requires_a_string() {
    for (const auto* const malformed :
         {R"({"__trystero_pw":"challenge","c":null})", R"({"__trystero_pw":"challenge","c":42})"}) {
        two_apps t("ZZZZZZZZZZZZZZZZZZN1", "AAAAAAAAAAAAAAAAAAN2", "pw");
        const ends e(t);
        joined_in(t, {"r1"});
        t.f.net.filter = [](std::size_t, std::size_t, std::span<const std::uint8_t> frame) {
            return internal_name(frame, "r2") == "@_hsdata"
                       ? trystero::test::memory_network::verdict::drop
                       : trystero::test::memory_network::verdict::deliver;
        };
        t.a_rooms.push_back(join(t.a, "r2"));
        std::vector<boost::system::error_code> errors;
        t.a_rooms[1].on_error([&](std::string_view, boost::system::error_code ec,
                                  std::string_view) { errors.push_back(ec); });
        t.b_rooms.push_back(join(t.b, "r2"));
        webcpp::trystero::test::advance(t.f, 1ms);
        t.f.net.filter = {};
        t.f.net.inject(t.f.net.peer_of(e.a()), internal_frame("r2", "@_hsdata", malformed));
        webcpp::trystero::test::advance(t.f, 1ms);
        BOOST_TEST_EQ(errors.size(), 1U);
        if (!errors.empty()) {
            BOOST_TEST(errors.front() == trystero::error::wrong_password);
        }
        BOOST_TEST(t.a_rooms[1].peers().empty());
        BOOST_TEST_EQ(t.a_rooms[0].peers().size(), 1U);
    }
}

// Connections and room tokens from a passwordless app never reach a protected app's registry,
// in either peer-id order. Different password domains have distinct app states and peer ids.
void check_public_connection_isolation(bool protected_lower) {
    two_apps t("MMMMMMMMMMMMMMMMMMM1", "NNNNNNNNNNNNNNNNNNN2");
    const ends e(t);
    t.a_rooms.push_back(join(t.a, "lobby"));
    t.a_rooms.push_back(join(t.a, "secret"));
    t.b_rooms.push_back(join(t.b, "lobby"));
    webcpp::test::require(BOOST_TEST(run_until(t.f, [&] {
        return t.a_rooms[0].peers().size() == 1 && t.b_rooms[0].peers().size() == 1;
    })));
    trystero::app protected_app(
        t.f.io.get_executor(), t.f.net,
        app_config(protected_lower ? "AAAAAAAAAAAAAAAAAAA3" : "ZZZZZZZZZZZZZZZZZZZ3", "pw"));
    auto vault = join(protected_app, "vault");
    const auto protected_lobby = join(protected_app, "lobby");
    BOOST_TEST(protected_app.self_id() != t.a.self_id());
    BOOST_TEST(protected_app.self_id() != t.b.self_id());
    std::size_t secret_frames = 0;
    std::size_t vault_frames = 0;
    t.f.net.filter = [&](std::size_t, std::size_t, std::span<const std::uint8_t> frame) {
        secret_frames += internal_name(frame, "secret") == "chat" ? 1 : 0;
        vault_frames += internal_name(frame, "vault") == "private" ? 1 : 0;
        return trystero::test::memory_network::verdict::deliver;
    };
    for (const auto* const room : {"secret", "vault"}) {
        t.f.net.inject(e.a(), trystero::protocol::presence_frame(
                                  trystero::protocol::room_token(app_id, room), true));
        t.f.net.inject(e.a(), internal_frame(room, "@_hsdata",
                                             R"({"__trystero_pw":"challenge","c":"unverified"})"));
        t.f.net.inject(e.a(), internal_frame(room, "@_hsready"));
    }
    webcpp::trystero::test::advance(t.f, 1ms);
    BOOST_TEST_EQ(t.a_rooms[1].peers().size(), 1U);
    BOOST_TEST(vault.peers().empty());
    BOOST_TEST(protected_lobby.peers().empty());
    t.a_rooms[1].make_action<std::string>("chat").async_send("public",
                                                             [](boost::system::error_code) {});
    vault.make_action<std::string>("private").async_send("protected",
                                                         [](boost::system::error_code) {});
    webcpp::trystero::test::advance(t.f, 12s);
    BOOST_TEST_EQ(secret_frames, 1U);
    BOOST_TEST_EQ(vault_frames, 0U);
    BOOST_TEST(vault.peers().empty());
    BOOST_TEST(protected_lobby.peers().empty());
    BOOST_TEST_EQ(t.a_rooms[0].peers().size(), 1U);
    t.f.net.filter = {};
}

void a_public_connection_never_reaches_a_protected_app() {
    for (const bool protected_lower : {true, false}) {
        check_public_connection_isolation(protected_lower);
    }
}

}  // namespace

int main(int argc, const char* const argv[]) {
    const auto cases = std::to_array<std::pair<std::string_view, void (*)()>>({
        {
            "a_joining_log_cannot_join_the_same_room_twice",
            a_joining_log_cannot_join_the_same_room_twice,
        },
        {
            "a_leaving_log_can_finish_the_farewell_by_rejoining",
            a_leaving_log_can_finish_the_farewell_by_rejoining,
        },
        {"a_join_log_can_leave_or_destroy_its_room", a_join_log_can_leave_or_destroy_its_room},
        {"an_error_log_can_leave_or_destroy_its_room", an_error_log_can_leave_or_destroy_its_room},
        {"a_queue_log_can_drain_or_destroy_its_room", a_queue_log_can_drain_or_destroy_its_room},
        {"a_password_challenge_requires_a_string", a_password_challenge_requires_a_string},
        {
            "invalid_room_proofs_fail_within_one_password_domain",
            invalid_room_proofs_fail_within_one_password_domain,
        },
        {"a_log_sink_can_leave_during_adoption", a_log_sink_can_leave_during_adoption},
        {
            "a_log_sink_can_destroy_another_room_during_adoption",
            a_log_sink_can_destroy_another_room_during_adoption,
        },
        {
            "a_public_connection_never_reaches_a_protected_app",
            a_public_connection_never_reaches_a_protected_app,
        },
        {"a_rooms_own_resource_returns_to_zero", a_rooms_own_resource_returns_to_zero},
        {
            "a_passwordless_room_is_open_to_the_apps_other_rooms",
            a_passwordless_room_is_open_to_the_apps_other_rooms,
        },
        {"crossed_connections_recover", crossed_connections_recover},
        {
            "a_flood_in_one_room_does_not_starve_another",
            a_flood_in_one_room_does_not_starve_another,
        },
        {"every_room_has_the_apps_id", every_room_has_the_apps_id},
        {
            "each_setup_report_reaches_the_current_handler_once",
            each_setup_report_reaches_the_current_handler_once,
        },
        {"joining_is_refused_twice_and_beyond_64", joining_is_refused_twice_and_beyond_64},
        {"two_apps_with_one_id_never_meet", two_apps_with_one_id_never_meet},
        {"two_rooms_share_one_connection", two_rooms_share_one_connection},
        {"three_rooms_share_one_connection", three_rooms_share_one_connection},
        {"a_second_connection_is_dropped", a_second_connection_is_dropped},
        {"a_moved_from_app_releases_nothing", a_moved_from_app_releases_nothing},
        {"an_app_destroyed_leaves_its_rooms_going", an_app_destroyed_leaves_its_rooms_going},
        {"an_app_assigned_over_releases_its_state", an_app_assigned_over_releases_its_state},
        {"an_app_assigned_to_itself_keeps_its_state", an_app_assigned_to_itself_keeps_its_state},
        {"frames_before_presence_are_kept", frames_before_presence_are_kept},
        {"a_kept_frame_that_kicks_during_a_binding", a_kept_frame_that_kicks_during_a_binding},
        {"a_kept_frame_binds_its_room", a_kept_frame_binds_its_room},
        {"a_leaving_room_binds_no_one", a_leaving_room_binds_no_one},
        {"a_connections_end_is_every_rooms", a_connections_end_is_every_rooms},
        {"a_late_join_binds_over_the_connection", a_late_join_binds_over_the_connection},
        {"a_forged_announcement_binds_nothing", a_forged_announcement_binds_nothing},
        {"a_kick_is_per_room", a_kick_is_per_room},
        {"one_room_leaves", one_room_leaves},
        {
            "a_peer_leaving_every_room_idles_the_connection",
            a_peer_leaving_every_room_idles_the_connection,
        },
        {"an_action_level_kick_is_per_room", an_action_level_kick_is_per_room},
        {
            "a_connection_failing_under_a_waiting_transfer",
            a_connection_failing_under_a_waiting_transfer,
        },
        {"a_room_left_can_be_joined_again", a_room_left_can_be_joined_again},
        {"join_and_leave_sequences_on_one_id", join_and_leave_sequences_on_one_id},
        {"the_farewell_binds_nothing", the_farewell_binds_nothing},
        {"the_last_user_closes_every_connection", the_last_user_closes_every_connection},
        {
            "the_io_context_destroyed_with_operations_pending",
            the_io_context_destroyed_with_operations_pending,
        },
        {"the_rebinding_step", the_rebinding_step},
        {"an_incorrect_proof_rebinds_at_the_steps", an_incorrect_proof_rebinds_at_the_steps},
        {"a_kick_that_closes_the_connection", a_kick_that_closes_the_connection},
        {"binding_needs_the_peers_own_presence", binding_needs_the_peers_own_presence},
        {"a_transient_failure_converges", a_transient_failure_converges},
        {"an_answer_to_an_old_challenge", an_answer_to_an_old_challenge},
        {"a_corrected_response_rejoins", a_corrected_response_rejoins},
        {"a_kept_leave_discards_what_came_before", a_kept_leave_discards_what_came_before},
    });
    std::size_t ran = 0;
    for (const auto& [name, run] : cases) {
        if (argc == 1 || name == argv[1]) {
            run();
            ++ran;
        }
    }
    BOOST_TEST(ran != 0);
    return boost::report_errors();
}
