// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Rooms of one app sharing one connection per remote peer, over the memory backend, on its virtual
// clock.

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
    advance(f, 100ms);
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
    advance(f, 100ms);
    b.on_error([&](std::string_view, boost::system::error_code, std::string_view) { ++calls[2]; });
    advance(f, 100ms);
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
    advance(f, 2000ms);
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

    two_apps(std::string_view a_id, std::string_view b_id)
        : a(f.io.get_executor(), f.net, app_config(a_id)),
          b(f.io.get_executor(), f.net, app_config(b_id)) {}

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
    advance(f, 1000ms);
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
    advance(f, 1000ms);
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
    advance(f, 1000ms);
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
    advance(f, 1000ms);
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

/** The action an inline room frame of `room` carries (`@_hsdata`, `@_leave`, `chat`, ...), or
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
    advance(t.f, 200ms);
    t.f.net.inject(e.a(), internal_frame("r2", "@_hsready"));
    advance(t.f, 50ms);
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
    advance(t.f, 200ms);
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
    const std::uint8_t garbage[] = {9, 9, 9};
    t.f.net.inject(e.a(), trystero::protocol::room_frame(token, garbage));
    t.f.net.inject(e.a(), trystero::protocol::presence_frame(token, true));
    BOOST_TEST(run_until(t.f, [&] { return leaves_sent == 1; }));
    advance(t.f, 1000ms);
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
    BOOST_TEST(run_until(t.f, [&] { return t.a_rooms[1].peers().size() == 1; }));
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
    advance(t.f, 200ms);
    bool left = false;
    t.a_rooms[1].async_leave([&left](boost::system::error_code) { left = true; });
    t.b_rooms.push_back(join(t.b, "r2"));
    std::size_t joins = 0;
    t.b_rooms[1].on_peer_join([&joins](std::string_view) { ++joins; });
    BOOST_TEST(run_until(t.f, [&] { return left; }));
    advance(t.f, 1000ms);
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
            const std::uint8_t bad[] = {3, 0, 1, 'x'};
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

    bool offer_or_answer() const {
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
            advance(t.f, 500ms);
            t.b_rooms.push_back(join(t.b, "r2"));
        } else {
            t.b_rooms.push_back(join(t.b, "r2"));
            advance(t.f, 500ms);
            t.a_rooms.push_back(join(t.a, "r2"));
        }
        BOOST_TEST(run_until(t.f, [&] { return t.all_joined(); }));
        advance(t.f,
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
    advance(t.f, 15s);
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
        advance(t.f, 1s);
        BOOST_TEST_EQ(b_r2_leaves, 0);
        BOOST_TEST_EQ(t.f.net.live_connections(), 2U);
        std::vector<std::string> got;
        auto in_r2 = t.b_rooms[1].make_action<std::string>("chat");
        in_r2.on_message([&](std::string s, const trystero::message_info&) { got.push_back(s); });
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
        advance(f, 100ms);
        BOOST_TEST_EQ(f.net.live_connections(), 2U);
        const auto made = f.net.made_connections;
        b1 = join(b, "r1");  // within the idle time: the connection is reused
        BOOST_TEST(run_until(f, [&] { return a1.peers().size() == 1; }, 1ms));
        BOOST_TEST_EQ(f.net.made_connections, made);
        b1.reset();
        BOOST_TEST(run_until(f, [&] { return a1.peers().empty(); }, 1ms));
        advance(f, 199ms);
        BOOST_TEST_EQ(f.net.live_connections(), 2U);  // not before
        advance(f, 1ms);
        BOOST_TEST(run_until(f, [&] { return f.net.live_connections() == 0; }, 1ms));
    }
}

/** Drops the first @_hsdata of r2 on any end, B's challenge when B is the lower id. */
void drop_first_challenge(two_apps& t) {
    auto dropped = std::make_shared<bool>(false);
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
    const std::uint8_t garbage[] = {9, 9, 9};
    t.f.net.inject(e.a(), trystero::protocol::room_frame(
                              trystero::protocol::room_token(app_id, "r2"), garbage));
    BOOST_TEST(run_until(t.f, [&] { return r2_leaves == 1; }));
    std::vector<std::string> got;
    auto in_r1 = t.b_rooms[0].make_action<std::string>("chat");
    in_r1.on_message([&](std::string s, const trystero::message_info&) { got.push_back(s); });
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
    receiver.on_message([](trystero::bytes, const trystero::message_info&) {});
    int completions = 0;
    std::optional<boost::system::error_code> sent;
    t.a_rooms[1].make_action<trystero::bytes>("big").async_send(
        trystero::bytes(1'000'000, std::uint8_t{4}), [&](boost::system::error_code ec) {
            ++completions;
            sent = ec;
        });
    webcpp::test::require(BOOST_TEST(poll_until(t.f, [&] {
        return t.f.net.buffered(e.a()) > 64 * 1024;  // a chunk waits on the drain
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

// a kick is per room. B's r2 has another password and A the lower id:
// A reports wrong_password in r2 only and r1 stays joined over the same connection. Then a
// handshake that times out: B the lower id, its challenge dropped once by the filter; B reports
// handshake_timed_out in r2 only, 10 s after binding, and r1 stays. (The rest of case 10, the kick
// that closes the connection and the renegotiation after it, needs the rebinding times: Task 9.)
[[maybe_unused]] void a_kick_is_per_room() {
    {
        two_apps t("AAAAAAAAAAAAAAAAAAA1", "BBBBBBBBBBBBBBBBBBB2");
        joined_in(t, {"r1"});
        std::vector<boost::system::error_code> r2_errors;
        t.a_rooms.push_back(join(t.a, "r2", {.password = "one"}));
        t.a_rooms[1].on_error([&](std::string_view, boost::system::error_code ec,
                                  std::string_view) { r2_errors.push_back(ec); });
        const auto made = t.f.net.made_connections;
        t.b_rooms.push_back(join(t.b, "r2", {.password = "two"}));
        BOOST_TEST(run_until(t.f, [&] { return !r2_errors.empty(); }));
        BOOST_TEST(r2_errors.at(0) == trystero::error::wrong_password);
        BOOST_TEST_EQ(t.a_rooms[0].peers().size(), 1U);
        BOOST_TEST_EQ(t.b_rooms[0].peers().size(), 1U);
        BOOST_TEST_EQ(t.f.net.made_connections, made);
        BOOST_TEST_EQ(t.f.net.live_connections(), 2U);
    }
    {
        two_apps t("BBBBBBBBBBBBBBBBBBB1", "AAAAAAAAAAAAAAAAAAA2");  // B the lower id
        joined_in(t, {"r1"});
        drop_first_challenge(t);
        std::vector<boost::system::error_code> b_errors;
        t.a_rooms.push_back(join(t.a, "r2", {.password = "same", .handshake_timeout = 30s}));
        const double joined = t.f.net.now_ms();
        t.b_rooms.push_back(join(t.b, "r2", {.password = "same"}));
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
    advance(t.f, 50ms);
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
    auto last_join = t.a.join("r");
    BOOST_TEST(last_join.has_value());
    BOOST_TEST((presences == std::vector<bool>{true, false, true, false, true}));
    advance(t.f, 1s);
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
        advance(t.f, 1s);
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
        auto b2 = join(b, "r2");
        webcpp::test::require(BOOST_TEST(
            run_until(f, [&] { return made.size() == 2 && f.net.peer_of(made[0]) == made[1]; })));
        bool left = false;
        a2.async_leave([&](boost::system::error_code) { left = true; });
        f.net.open(made[0]);
        f.net.open(made[1]);
        BOOST_TEST(run_until(f, [&] { return left; }));
        advance(f, 1s);
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
    auto b1 = join(b, "r1");
    auto b2 = join(b, "r2");
    webcpp::test::require(BOOST_TEST(
        run_until(f, [&] { return a1->peers().size() == 1 && a2->peers().size() == 1; })));
    const auto ended = [&a_log] {
        return std::ranges::count_if(a_log, [](const std::string& line) {
            return line.find(" ended: ") != std::string::npos;
        });
    };
    a.reset();
    a1.reset();
    advance(f, 1s);
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

}  // namespace

int main() {
    every_room_has_the_apps_id();
    each_setup_report_reaches_the_current_handler_once();
    joining_is_refused_twice_and_beyond_64();
    two_apps_with_one_id_never_meet();
    two_rooms_share_one_connection();
    three_rooms_share_one_connection();
    a_second_connection_is_dropped();
    a_moved_from_app_releases_nothing();
    an_app_destroyed_leaves_its_rooms_going();
    an_app_assigned_over_releases_its_state();
    an_app_assigned_to_itself_keeps_its_state();
    frames_before_presence_are_kept();
    a_kept_frame_that_kicks_during_a_binding();
    a_kept_frame_binds_its_room();
    a_leaving_room_binds_no_one();
    a_connections_end_is_every_rooms();
    a_late_join_binds_over_the_connection();
    a_forged_announcement_binds_nothing();
    one_room_leaves();
    a_peer_leaving_every_room_idles_the_connection();
    an_action_level_kick_is_per_room();
    a_connection_failing_under_a_waiting_transfer();
    a_room_left_can_be_joined_again();
    join_and_leave_sequences_on_one_id();
    the_farewell_binds_nothing();
    the_last_user_closes_every_connection();
    the_io_context_destroyed_with_operations_pending();
    return boost::report_errors();
}
