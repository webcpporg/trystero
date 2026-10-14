// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The state an app and its rooms share, alone, over a memory network: the configuration it
// completes and what it sets aside, the registry of rooms, the adoption of connections, the
// routing of a connection's events to the rooms, and the end of every connection with its last
// user.

#include <webcpp/trystero/detail/app_impl.hpp>
#include <webcpp/trystero/options.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include "support/memory_backend.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "require.hpp"

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;
namespace detail = webcpp::trystero::detail;
using namespace std::chrono_literals;

namespace {

template <class Options>
concept has_password_option = requires(Options options) { options.password; };
static_assert(has_password_option<trystero::app_config>);
static_assert(!has_password_option<trystero::room_options>);

bool run_until(asio::io_context& io, const std::function<bool()>& done,
               std::chrono::milliseconds limit = 5s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < end) {
        io.run_for(10ms);  // lint-run: the memory network posts to it
        if (io.stopped()) {
            io.restart();
        }
    }
    return done();
}

/** Runs the ready work of `io` until there is none. */
void settle(asio::io_context& io) {
    io.restart();
    while (io.poll() > 0) {
        io.restart();
    }
}

/** A room as the app sees it, which records every call: "+P", "-P", "kept P", "ended P: why", and
 * the connection each end names. */
class fake_room final : public detail::room_endpoint {
public:
    [[nodiscard]] bool leaving() const override { return false; }

    void finish_farewell() override {}

    void presence_changed(std::string_view peer, bool present) override {
        events.push_back((present ? "+" : "-") + std::string(peer));
    }

    [[nodiscard]] bool keeps_frames_from(std::string_view /*peer*/) const override { return keeps; }

    void frame_kept(std::string_view peer) override {
        events.push_back("kept " + std::string(peer));
    }

    void connection_ended(const std::shared_ptr<detail::connection>& ended, std::string_view peer,
                          std::string_view reason) override {
        ended_connections.push_back(ended);
        events.push_back("ended " + std::string(peer) + ": " + std::string(reason));
        if (on_end) {
            on_end();
        }
    }

    std::vector<std::string> events;
    /** The connection each end named, in the order of `events`' ends; weak, so that the record
     * keeps none alive. */
    std::vector<std::weak_ptr<detail::connection>> ended_connections;
    bool keeps = true;
    /** Called after a connection's end is recorded: a room that leaves the registry there. */
    std::function<void()> on_end;
};

/** One negotiated pair: the app's end, whose connection and channel it adopts, and the remote's,
 * which records every frame it receives. */
struct pair_ends {
    std::shared_ptr<trystero::backend::peer_connection> local_pc;
    std::shared_ptr<trystero::backend::data_channel> local;
    std::shared_ptr<trystero::backend::peer_connection> remote_pc;
    std::shared_ptr<trystero::backend::data_channel> remote;
    std::size_t local_id = 0;  // the memory network's id of `local_pc`
    std::shared_ptr<std::vector<std::vector<std::uint8_t>>> received =
        std::make_shared<std::vector<std::vector<std::uint8_t>>>();
};

pair_ends negotiate(asio::io_context& io, trystero::test::memory_network& net) {
    auto* m = std::pmr::get_default_resource();
    pair_ends p;
    p.local_pc = net.make_peer_connection({}, m);
    p.remote_pc = net.make_peer_connection({}, m);
    p.local_id = std::static_pointer_cast<trystero::test::memory_peer_connection>(p.local_pc)->id();
    p.local = p.local_pc->create_data_channel("data");
    p.remote_pc->set_handlers({
        .ice_candidate = {},
        .data_channel =
            [&p](std::shared_ptr<trystero::backend::data_channel> c) { p.remote = std::move(c); },
        .state = {},
    });
    std::pmr::string offer(m);
    std::pmr::string answer(m);
    p.local_pc->set_local_description([&offer](trystero::backend::error_code, std::string_view,
                                               std::string_view sdp) { offer = sdp; });
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return !offer.empty(); })));
    p.remote_pc->set_remote_description("offer", offer, [](trystero::backend::error_code) {});
    p.remote_pc->set_local_description([&answer](trystero::backend::error_code, std::string_view,
                                                 std::string_view sdp) { answer = sdp; });
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return !answer.empty(); })));
    p.local_pc->set_remote_description("answer", answer, [](trystero::backend::error_code) {});
    webcpp::test::require(BOOST_TEST(
        run_until(io, [&] { return p.local->is_open() && p.remote && p.remote->is_open(); })));
    p.remote_pc->set_handlers({});  // its handler names `p`, which the return may move
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

std::vector<std::uint8_t> presence(std::string_view token, bool present) {
    const auto frame = trystero::protocol::presence_frame(token, present);
    return {frame.begin(), frame.end()};
}

/** What the app's sink received: "category: text". */
using lines = std::vector<std::string>;

trystero::detail::logger::sink record_log(lines& log) {
    return [&log](std::string_view category, std::string_view text) {
        log.push_back(std::string(category) + ": " + std::string(text));
    };
}

/** An app over `net`, configured with its defaults but `edit`'s changes. */
std::shared_ptr<detail::app_impl> make_app(
    asio::io_context& io, trystero::test::memory_network& net, lines& log,
    const std::function<void(trystero::app_config&)>& edit = {}) {
    trystero::app_config config{.app_id = "app", .log = record_log(log)};
    if (edit) {
        edit(config);
    }
    return std::make_shared<detail::app_impl>(io.get_executor(), net, std::move(config));
}

/** An ICE server of one URL, without credentials. */
trystero::backend::ice_server server(std::string_view url) {
    return {.urls = {std::pmr::string(url)}, .username = {}, .credential = {}};
}

std::vector<std::string> urls_of(std::span<const trystero::backend::ice_server> servers) {
    std::vector<std::string> out;
    for (const auto& server : servers) {
        for (const auto& url : server.urls) {
            out.emplace_back(url);
        }
    }
    return out;
}

// An empty self id is drawn from the factory, the ICE servers default to Trystero's, and the
// relays are derived from the app id; nothing is set aside.
void the_configuration_is_copied_and_completed() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    BOOST_TEST_EQ(app->app_id(), "app");
    BOOST_TEST(app->password().empty());
    BOOST_TEST_EQ(app->self_id().size(), trystero::protocol::peer_id_size);
    BOOST_TEST_EQ(app->self_id().find_first_not_of(trystero::protocol::id_alphabet),
                  std::string_view::npos);
    BOOST_TEST(urls_of(app->ice_servers()) == urls_of(trystero::default_ice_servers()));
    const auto derived =
        trystero::protocol::derive_relays("app", trystero::protocol::default_nostr_relays, 5);
    BOOST_TEST_EQ(app->relay_urls().size(), 5U);
    BOOST_TEST(std::ranges::equal(app->relay_urls(), derived));
    BOOST_TEST(app->idle_timeout() == 123'333ms);
    BOOST_TEST(app->setup_reports().empty());
    BOOST_TEST(log.empty());
    BOOST_TEST(app->memory() == std::pmr::get_default_resource());
    BOOST_TEST(app->get_executor() == io.get_executor());
    BOOST_TEST(&app->factory() == &net);
    BOOST_TEST_EQ(app->counted_rooms(), 0U);
    BOOST_TEST(app->live_connections().empty());
}

// What the configuration names is kept: a self id of 64 bytes, the relays, a redundancy that
// their list makes moot, the ICE servers with their scheme lowercased.
void what_the_configuration_names_is_kept() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const std::string id(trystero::protocol::max_peer_id_bytes, 'x');
    const auto app = make_app(io, net, log, [&](trystero::app_config& c) {
        c.self_id = id;
        c.password = "the immutable password of every room in this app";
        c.relay_urls.emplace_back("wss://relay.example");
        c.relay_redundancy = 1;
        c.ice_servers.push_back(server("STUN:stun.example:3478"));
        c.idle_timeout = 0ms;
    });
    BOOST_TEST_EQ(app->self_id(), id);
    BOOST_TEST_EQ(app->password(), "the immutable password of every room in this app");
    BOOST_TEST_EQ(app->relay_urls().size(), 1U);
    BOOST_TEST_EQ(app->relay_urls()[0], "wss://relay.example");
    BOOST_TEST(urls_of(app->ice_servers()) == (std::vector<std::string>{"stun:stun.example:3478"}));
    BOOST_TEST(app->idle_timeout() == 0ms);
    BOOST_TEST(app->setup_reports().empty());
}

// An over-long self id, a negative idle time and every ICE server no peer connection could be
// made with are set aside, each reported once and logged, in that order.
void what_no_peer_could_use_is_set_aside() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log, [](trystero::app_config& c) {
        c.self_id = std::pmr::string(trystero::protocol::max_peer_id_bytes + 1, 'x');
        c.idle_timeout = -1ms;
        c.ice_servers.push_back(server("http://example"));
        c.ice_servers.push_back(server("stun:stun.example"));
        c.ice_servers.push_back({});
    });
    BOOST_TEST_EQ(app->self_id().size(), trystero::protocol::peer_id_size);
    BOOST_TEST(app->idle_timeout() == 0ms);
    BOOST_TEST(urls_of(app->ice_servers()) == (std::vector<std::string>{"stun:stun.example"}));
    const std::vector<std::string> expected{
        "self_id is longer than any peer accepts, so the app uses a random one",
        "idle_timeout is negative, so the app closes an unused connection at once",
        "the ICE server http://example is not stun:, stuns:, turn: or turns:, so the app goes on "
        "without it",
        "the ICE server (no URL) names no URL, so the app goes on without it",
    };
    const auto reports = app->setup_reports();
    BOOST_TEST(std::vector<std::string>(reports.begin(), reports.end()) == expected);
    lines logged;
    for (const auto& r : expected) {
        logged.push_back("app: " + r);
    }
    BOOST_TEST(log == logged);
}

// Every allocation of the app's construction, of its registry, of an adoption, of a stale
// connection's replacement and end, of the list of live connections and of the last user's release
// goes through its resource: the default resource refuses every request meanwhile.
void every_allocation_goes_through_the_apps_resource() {
    std::pmr::unsynchronized_pool_resource pool;  // outlives the io_context and the connections
    asio::io_context io;
    trystero::test::memory_network net(io);
    const auto ends = negotiate(io, net);
    const auto next = negotiate(io, net);
    trystero::app_config config{.memory = &pool};
    config.app_id = "app";
    const std::string expected_password(60, 'p');
    config.password = expected_password;
    config.self_id = std::pmr::string(40, 'x');  // beyond the short string
    config.ice_servers.push_back(server("stun:stun.example:3478"));
    std::size_t sentences = 0;
    config.log = [&sentences](std::string_view, std::string_view) { ++sentences; };
    const auto room = std::make_shared<fake_room>();
    std::shared_ptr<detail::app_impl> app;
    std::size_t live = 0;
    const std::string_view peer = "a peer id longer than the short string";
    auto* const previous = std::pmr::set_default_resource(std::pmr::null_memory_resource());
    {
        app = std::allocate_shared<detail::app_impl>(std::pmr::polymorphic_allocator<>(&pool),
                                                     io.get_executor(), net, std::move(config));
        BOOST_TEST_EQ(app->password(), expected_password);
        app->register_room("r1", "a room token longer than the short string", room);
        app->add_user();
        app->adopt(peer, ends.local_pc, ends.local);
        ends.local->close();
        app->adopt(peer, next.local_pc, next.local);
        live = app->live_connections().size();
        app->release_user();
    }
    std::pmr::set_default_resource(previous);
    BOOST_TEST_EQ(live, 1U);
    BOOST_TEST_EQ(sentences, 4U);  // adopted, ended, adopted, ended
    BOOST_TEST_EQ(room->events.size(), 2U);
    BOOST_TEST(app->live_connections().empty());
}

// Rooms are found by id through a weak reference, and a room leaves the registry only by its own
// entry.
void the_registry_holds_rooms_by_a_weak_reference() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    auto r1 = std::make_shared<fake_room>();
    const auto other = std::make_shared<fake_room>();
    app->register_room("r1", "T1", r1);
    BOOST_TEST_EQ(app->counted_rooms(), 1U);
    BOOST_TEST(app->find_room("r1") == r1);
    BOOST_TEST(app->find_room("r2") == nullptr);
    app->unregister_room("T1", other.get());
    BOOST_TEST(app->find_room("r1") == r1);
    app->unregister_room("T1", r1.get());
    BOOST_TEST_EQ(app->counted_rooms(), 0U);
    BOOST_TEST(app->find_room("r1") == nullptr);
    app->register_room("r1", "T1", r1);
    app->register_room("r1", "T1", other);  // the same token: the later room's entry
    BOOST_TEST_EQ(app->counted_rooms(), 1U);
    BOOST_TEST(app->find_room("r1") == other);
    app->register_room("r2", "T2", r1);
    r1.reset();
    BOOST_TEST(app->find_room("r2") == nullptr);  // the registry did not keep it
}

// An adopted connection advertises every registered room's token, is found by its peer and is
// live; a second connection to the same peer is closed, and the first returned.
void adopt_starts_one_connection_per_peer() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    const auto r1 = std::make_shared<fake_room>();
    const auto r2 = std::make_shared<fake_room>();
    app->register_room("r1", "T1", r1);
    app->register_room("r2", "T2", r2);
    const auto first = negotiate(io, net);
    const auto c = app->adopt("B", first.local_pc, first.local);
    webcpp::test::require(BOOST_TEST(c != nullptr));
    BOOST_TEST_EQ(c->peer(), "B");
    BOOST_TEST(app->find("B") == c);
    BOOST_TEST(app->find("C") == nullptr);
    BOOST_TEST_EQ(app->live_connections().size(), 1U);
    BOOST_TEST(run_until(io, [&] { return first.received->size() == 2; }));
    BOOST_TEST(*first.received == (std::vector<std::vector<std::uint8_t>>{presence("T1", true),
                                                                          presence("T2", true)}));
    const auto second = negotiate(io, net);
    BOOST_TEST(app->adopt("B", second.local_pc, second.local) == c);
    BOOST_TEST(!second.local->is_open());
    BOOST_TEST(c->live());
    BOOST_TEST_EQ(app->live_connections().size(), 1U);
    BOOST_TEST(log == (lines{
                          "webrtc: connection with B adopted",
                          "webrtc: a second connection with B closed: the first is shared",
                      }));
    BOOST_TEST(r1->events.empty() && r2->events.empty());
}

// The remote's presence reaches the room of its token, a frame of a room not bound is kept when
// the room keeps frames, and the connection's end reaches every room, even when one leaves the
// registry from inside the call; the ended connection is let go.
void the_connections_events_reach_the_rooms() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    const auto r1 = std::make_shared<fake_room>();
    const auto r2 = std::make_shared<fake_room>();
    r1->keeps = false;
    app->register_room("r1", "T1", r1);
    app->register_room("r2", "T2", r2);
    r1->on_end = [&] { app->unregister_room("T1", r1.get()); };
    const auto ends = negotiate(io, net);
    auto c = app->adopt("B", ends.local_pc, ends.local);
    const std::weak_ptr<detail::connection> weak = c;
    ends.remote->send(trystero::protocol::presence_frame("T1", true));
    ends.remote->send(trystero::protocol::presence_frame("T9", true));  // no room of the app
    ends.remote->send(trystero::protocol::presence_frame("T1", false));
    const std::array<std::uint8_t, 2> action{2, 0};
    ends.remote->send(trystero::protocol::room_frame("T1", action));  // r1 drops frames
    ends.remote->send(trystero::protocol::room_frame("T2", action));
    ends.remote->send(trystero::protocol::room_frame("T9", action));  // no room: dropped
    BOOST_TEST(run_until(io, [&] { return r2->events.size() == 1; }));
    settle(io);
    BOOST_TEST(r1->events == (std::vector<std::string>{"+B", "-B"}));
    BOOST_TEST(r2->events == (std::vector<std::string>{"kept B"}));
    BOOST_TEST_EQ(c->kept_count(), 1U);
    BOOST_TEST_EQ(c->take_kept("T2").size(), 1U);
    c.reset();
    net.fail(ends.local_id);
    BOOST_TEST(run_until(io, [&] { return r1->events.size() == 3; }));
    settle(io);
    BOOST_TEST_EQ(r1->events.back(), "ended B: connection failed");
    BOOST_TEST_EQ(r2->events.back(), "ended B: connection failed");
    BOOST_TEST_EQ(app->counted_rooms(), 1U);  // r1 left during its call
    BOOST_TEST(app->find("B") == nullptr);
    BOOST_TEST(app->live_connections().empty());
    BOOST_TEST(weak.expired());
}

// The last user's release ends every connection, at that call; an earlier release ends none.
void the_last_user_ends_every_connection() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", "T1", room);
    const auto b = negotiate(io, net);
    const auto c = negotiate(io, net);
    app->add_user();
    app->add_user();
    const auto to_b = app->adopt("B", b.local_pc, b.local);
    const auto to_c = app->adopt("C", c.local_pc, c.local);
    app->release_user();
    BOOST_TEST(to_b->live() && to_c->live());
    BOOST_TEST(room->events.empty());
    app->release_user();
    BOOST_TEST(!to_b->live() && !to_c->live());
    BOOST_TEST(room->events == (std::vector<std::string>{
                                   "ended B: the app has no room left",
                                   "ended C: the app has no room left",
                               }));
    BOOST_TEST(app->live_connections().empty());
    app->release_user();  // no user left: nothing to release
    BOOST_TEST_EQ(room->events.size(), 2U);
    const auto d = negotiate(io, net);
    const auto to_d = app->adopt("D", d.local_pc, d.local);
    app->add_user();
    app->release_user();
    BOOST_TEST(!to_d->live());
    BOOST_TEST_EQ(room->events.back(), "ended D: the app has no room left");
}

// A connection whose channel is closing is not reused; the connection that replaces it ends it
// first, as Trystero clears a stale peer (C/shared-peer.mjs:142-149), so the rooms hear its end
// before the new one is adopted, and its later close event ends nothing.
void a_stale_connection_ends_before_its_replacement() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", "T1", room);
    const auto first = negotiate(io, net);
    const auto second = negotiate(io, net);
    const auto stale = app->adopt("B", first.local_pc, first.local);
    first.local->close();  // closing: its close event is posted
    BOOST_TEST(app->find("B") == nullptr);
    BOOST_TEST(app->live_connections().empty());
    const auto fresh = app->adopt("B", second.local_pc, second.local);
    BOOST_TEST(fresh != stale);
    BOOST_TEST(room->events ==
               (std::vector<std::string>{"ended B: replaced by a new connection to the peer"}));
    settle(io);
    BOOST_TEST(app->find("B") == fresh);
    BOOST_TEST(fresh->live());
    BOOST_TEST_EQ(room->events.size(), 1U);
}

// A room that adopts a connection to the peer from inside the stale connection's end: the
// replacement the outer adoption brought is closed, and the room's is the one kept, so the peer
// has one connection, whose end leaves none (Trystero's owns(), C/shared-peer.mjs:210-212).
void an_adoption_from_inside_a_stale_end_is_the_one_kept() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", "T1", room);
    const auto first = negotiate(io, net);
    const auto second = negotiate(io, net);
    const auto third = negotiate(io, net);
    app->adopt("B", first.local_pc, first.local);
    std::shared_ptr<detail::connection> nested;
    room->on_end = [&] {
        if (!nested) {
            nested = app->adopt("B", third.local_pc, third.local);
        }
    };
    first.local->close();
    const auto outer = app->adopt("B", second.local_pc, second.local);
    webcpp::test::require(BOOST_TEST(nested != nullptr));
    BOOST_TEST(outer == nested);
    BOOST_TEST(!second.local->is_open());
    BOOST_TEST(nested->live());
    BOOST_TEST_EQ(app->live_connections().size(), 1U);
    settle(io);
    BOOST_TEST(app->find("B") == nested);
    room->on_end = {};
    nested->end("test");
    BOOST_TEST(app->find("B") == nullptr);
    BOOST_TEST(app->live_connections().empty());
    BOOST_TEST_EQ(room->events.back(), "ended B: test");
}

// A room adopts a connection to the peer from inside the stale connection's end, and its channel
// starts closing before the outer adoption goes on: that one is stale too, and ends before the
// outer one is adopted, so no room hears an end once the outer one is the peer's.
void a_nested_adoption_gone_stale_ends_before_the_outer_one() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", "T1", room);
    const auto first = negotiate(io, net);
    const auto second = negotiate(io, net);
    const auto third = negotiate(io, net);
    app->adopt("B", first.local_pc, first.local);
    std::shared_ptr<detail::connection> nested;
    room->on_end = [&] {
        if (!nested) {
            nested = app->adopt("B", third.local_pc, third.local);
            third.local->close();  // closing: its close event is posted
        }
    };
    first.local->close();
    const auto outer = app->adopt("B", second.local_pc, second.local);
    webcpp::test::require(BOOST_TEST(nested != nullptr));
    BOOST_TEST(outer != nested);
    const std::vector<std::string> ends{
        "ended B: replaced by a new connection to the peer",
        "ended B: replaced by a new connection to the peer",
    };
    BOOST_TEST(room->events == ends);
    settle(io);
    BOOST_TEST(room->events == ends);
    BOOST_TEST(app->find("B") == outer);
    BOOST_TEST(outer->live());
    BOOST_TEST_EQ(app->live_connections().size(), 1U);
}

// An adoption from inside a connection's own end, before the rooms hear it (a drain waiter the end
// fails): the ending connection is let go, the new one is the peer's, and the rooms hear the end
// once.
void an_adoption_from_inside_an_end_under_way() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", "T1", room);
    const auto first = negotiate(io, net);
    const auto second = negotiate(io, net);
    const auto c = app->adopt("B", first.local_pc, first.local);
    auto* m = std::pmr::get_default_resource();
    const std::pmr::polymorphic_allocator<> alloc(m);
    const auto b = std::allocate_shared<detail::binding>(
        alloc, c, "T1",
        trystero::handler<void(trystero::backend::bytes&&, std::span<const std::uint8_t>)>(
            std::allocator_arg, alloc,
            [](trystero::backend::bytes&&, std::span<const std::uint8_t>) {}),
        m);
    net.channel_capacity = 1;
    const std::vector<std::uint8_t> payload(16'000, 0);
    for (int i = 0; i < 5; ++i) {
        webcpp::test::require(
            BOOST_TEST(b->send(trystero::protocol::room_frame("T9", payload, m))));
    }
    std::shared_ptr<detail::connection> nested;
    b->wait_drained({std::allocator_arg, alloc,
                     [&](bool) { nested = app->adopt("B", second.local_pc, second.local); }});
    c->end("test");
    webcpp::test::require(BOOST_TEST(nested != nullptr));
    BOOST_TEST(nested != c);
    BOOST_TEST(nested->live());
    BOOST_TEST(app->find("B") == nested);
    BOOST_TEST(room->events == (std::vector<std::string>{"ended B: test"}));
    // The end the rooms hear names the connection that ended, not the peer's new one.
    webcpp::test::require(BOOST_TEST_EQ(room->ended_connections.size(), 1U));
    BOOST_TEST(room->ended_connections.front().lock() == c);
    settle(io);
    BOOST_TEST(app->find("B") == nested);
    BOOST_TEST_EQ(room->events.size(), 1U);
}

// A connection's end takes its own entry only: the last user's release ends the connections it
// took out of the registry, and a room that adopts one to the same peer from inside the calls keeps
// it registered, though the rooms still hear the released connection's end.
void an_end_takes_only_its_own_entry() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", "T1", room);
    const auto b = negotiate(io, net);
    const auto c = negotiate(io, net);
    const auto again = negotiate(io, net);
    app->add_user();
    app->adopt("B", b.local_pc, b.local);
    app->adopt("C", c.local_pc, c.local);
    std::shared_ptr<detail::connection> nested;
    room->on_end = [&] {
        if (!nested) {
            nested = app->adopt("C", again.local_pc, again.local);
        }
    };
    app->release_user();
    webcpp::test::require(BOOST_TEST(nested != nullptr));
    BOOST_TEST(room->events == (std::vector<std::string>{
                                   "ended B: the app has no room left",
                                   "ended C: the app has no room left",
                               }));
    BOOST_TEST(nested->live());
    BOOST_TEST(app->find("C") == nested);
}

// The peer named by a view into the stale connection, which its end frees: the replacement is
// adopted under a copy taken before the end.
void a_view_into_the_stale_connection_names_its_replacement() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    const auto first = negotiate(io, net);
    const auto second = negotiate(io, net);
    const std::string id(40, 'p');  // beyond the short string: the view is into the heap
    const std::weak_ptr<detail::connection> stale = app->adopt(id, first.local_pc, first.local);
    first.local->close();
    const auto view = stale.lock()->peer();
    const auto fresh = app->adopt(view, second.local_pc, second.local);
    BOOST_TEST(stale.expired());
    BOOST_TEST_EQ(fresh->peer(), id);
    BOOST_TEST(app->find(id) == fresh);
}

// The app's destruction ends nothing and calls nothing back, and a connection that outlives it
// routes nothing.
void the_apps_destruction_is_quiet() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    auto app = make_app(io, net, log);
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", "T1", room);
    const auto ends = negotiate(io, net);
    const auto c = app->adopt("B", ends.local_pc, ends.local);
    const auto logged = log.size();
    app.reset();
    settle(io);
    BOOST_TEST(c->live());
    ends.remote->send(trystero::protocol::presence_frame("T1", true));
    const std::array<std::uint8_t, 2> action{2, 0};
    ends.remote->send(trystero::protocol::room_frame("T1", action));
    settle(io);
    BOOST_TEST(c->advertised("T1"));
    BOOST_TEST_EQ(c->kept_count(), 0U);  // no app: no room keeps it
    c->end("test");
    BOOST_TEST(room->events.empty());
    BOOST_TEST_EQ(log.size(), logged + 1);  // the connection's own line, through the shared sink
    BOOST_TEST_EQ(log.back(), "webrtc: connection with B ended: test");
}

}  // namespace

int main() {
    the_configuration_is_copied_and_completed();
    what_the_configuration_names_is_kept();
    what_no_peer_could_use_is_set_aside();
    every_allocation_goes_through_the_apps_resource();
    the_registry_holds_rooms_by_a_weak_reference();
    adopt_starts_one_connection_per_peer();
    the_connections_events_reach_the_rooms();
    the_last_user_ends_every_connection();
    a_stale_connection_ends_before_its_replacement();
    an_adoption_from_inside_a_stale_end_is_the_one_kept();
    a_nested_adoption_gone_stale_ends_before_the_outer_one();
    an_adoption_from_inside_an_end_under_way();
    an_end_takes_only_its_own_entry();
    a_view_into_the_stale_connection_names_its_replacement();
    the_apps_destruction_is_quiet();
    return boost::report_errors();
}
