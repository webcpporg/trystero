// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Rooms of one app sharing one connection per remote peer, over the memory backend, on its virtual
// clock.

#include <webcpp/trystero.hpp>

#include "support/memory_backend.hpp"
#include "support/rooms.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
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
    return boost::report_errors();
}
