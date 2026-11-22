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

#include <webcpp/asio/io_context.hpp>

#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

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

#include "cases.hpp"
#include "require.hpp"

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
namespace detail = webcpp::trystero::detail;
using namespace std::chrono_literals;

namespace {

template <class Options>
concept has_password_option = requires(Options options) { options.password; };
static_assert(has_password_option<trystero::app_config>);
static_assert(!has_password_option<trystero::room_options>);

// The options 0.27.0 removed: a custom peer id (`peerId`, once `selfId`) and `handshakeTimeoutMs`.
template <class Options>
concept has_self_id_option = requires(Options options) { options.self_id; };
template <class Options>
concept has_handshake_timeout_option = requires(Options options) { options.handshake_timeout; };
static_assert(!has_self_id_option<trystero::app_config>);
static_assert(!has_handshake_timeout_option<trystero::room_options>);
static_assert(!has_handshake_timeout_option<trystero::app_config>);

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

    void relays_offline(bool offline) override {
        events.emplace_back(offline ? "relays offline" : "relays online");
    }

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

// The self id is drawn from the factory, the ICE servers default to Trystero's, and the relays
// are derived from the app id; nothing is set aside.
void the_configuration_is_copied_and_completed() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    BOOST_TEST_EQ(app->app_id(), "app");
    BOOST_TEST(app->password().empty());
    BOOST_TEST_EQ(app->self_id().size(), trystero::protocol::peer_id_size);
    BOOST_TEST_EQ(app->self_id().find_first_not_of(trystero::protocol::base62_alphabet),
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

// What the configuration names is kept: the relays, a redundancy that their list makes moot, the
// ICE servers with their scheme lowercased.
void what_the_configuration_names_is_kept() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log, [&](trystero::app_config& c) {
        c.password = "the immutable password of every room in this app";
        c.relay_urls.emplace_back("wss://relay.example");
        c.relay_redundancy = 1;
        c.ice_servers.push_back(server("STUN:stun.example:3478"));
        c.idle_timeout = 0ms;
    });
    BOOST_TEST_EQ(app->password(), "the immutable password of every room in this app");
    BOOST_TEST_EQ(app->relay_urls().size(), 1U);
    BOOST_TEST_EQ(app->relay_urls()[0], "wss://relay.example");
    BOOST_TEST(urls_of(app->ice_servers()) == (std::vector<std::string>{"stun:stun.example:3478"}));
    BOOST_TEST(app->idle_timeout() == 0ms);
    BOOST_TEST(app->setup_reports().empty());
}

// A negative idle time and every ICE server no peer connection could be made with are set aside,
// each reported once and logged, in that order.
void what_no_peer_could_use_is_set_aside() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log, [](trystero::app_config& c) {
        c.idle_timeout = -1ms;
        c.ice_servers.push_back(server("http://example"));
        c.ice_servers.push_back(server("stun:stun.example"));
        c.ice_servers.push_back({});
    });
    BOOST_TEST(app->idle_timeout() == 0ms);
    BOOST_TEST(urls_of(app->ice_servers()) == (std::vector<std::string>{"stun:stun.example"}));
    const std::vector<std::string> expected{
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

// No configuration names the self id, as 0.27.0 has no custom one: each app draws its own, 20
// base62 characters from the factory's randomness, so that two apps of one network differ; and no
// room option names a handshake timeout (the static assertions above).
void no_custom_self_id() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto before = net.random_bytes;
    const auto a = make_app(io, net, log);
    BOOST_TEST_GT(net.random_bytes, before);
    const auto b = make_app(io, net, log);
    for (const auto& app : {a, b}) {
        BOOST_TEST_EQ(app->self_id().size(), trystero::protocol::peer_id_size);
        BOOST_TEST_EQ(app->self_id().find_first_not_of(trystero::protocol::base62_alphabet),
                      std::string_view::npos);
    }
    BOOST_TEST_NE(a->self_id(), b->self_id());
    BOOST_TEST(a->setup_reports().empty());
    BOOST_TEST(log.empty());
}

// The connectivity reaches every room of the app, as 0.27.0's `setRelayConnectivity` reaches every
// socket: `offline` holds the relays at each call, `online` reconnects them at each call, and
// `automatic` reconnects only relays a program held offline. Natively there is no page to watch:
// the app is automatic and online from the start, and asks the backend for a watch again each time
// it becomes automatic.
void the_connectivity_reaches_every_room() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    BOOST_TEST(app->connectivity() == trystero::relay_connectivity::automatic);
    BOOST_TEST(!app->relays_offline());
    const auto r1 = std::make_shared<fake_room>();
    const auto r2 = std::make_shared<fake_room>();
    app->register_room("r1", "T1", r1);
    app->register_room("r2", "T2", r2);
    BOOST_TEST(app->set_connectivity(trystero::relay_connectivity::offline) ==
               trystero::relay_connectivity::offline);
    BOOST_TEST(app->relays_offline());
    BOOST_TEST(app->set_connectivity(trystero::relay_connectivity::offline) ==
               trystero::relay_connectivity::offline);
    BOOST_TEST(app->set_connectivity(trystero::relay_connectivity::automatic) ==
               trystero::relay_connectivity::automatic);
    BOOST_TEST(!app->relays_offline());
    BOOST_TEST(app->set_connectivity(trystero::relay_connectivity::automatic) ==
               trystero::relay_connectivity::automatic);
    BOOST_TEST(app->set_connectivity(trystero::relay_connectivity::online) ==
               trystero::relay_connectivity::online);
    BOOST_TEST(app->set_connectivity(trystero::relay_connectivity::online) ==
               trystero::relay_connectivity::online);
    BOOST_TEST(app->connectivity() == trystero::relay_connectivity::online);
    const std::vector<std::string> expected{
        "relays offline", "relays offline", "relays online", "relays online", "relays online",
    };
    BOOST_TEST(r1->events == expected);
    BOOST_TEST(r2->events == expected);
    BOOST_TEST(log == (lines{
                          "app: the relays go offline",
                          "app: the relays go offline",
                          "app: the relays go online",
                          "app: the relays go online",
                          "app: the relays go online",
                      }));
}

// Where the page has events, an automatic app follows them, each after the event, and an app a
// program set online or offline does not, until it is automatic again.
void an_automatic_app_follows_the_pages_events() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    net.page_events = true;
    lines log;
    const auto app = make_app(io, net, log);
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", "T1", room);
    net.page_connectivity(false);
    BOOST_TEST(room->events.empty());  // the event is posted
    settle(io);
    BOOST_TEST(app->relays_offline());
    BOOST_TEST(app->connectivity() == trystero::relay_connectivity::automatic);
    net.page_connectivity(true);
    settle(io);
    BOOST_TEST(!app->relays_offline());
    app->set_connectivity(trystero::relay_connectivity::online);
    net.page_connectivity(false);
    settle(io);
    BOOST_TEST(!app->relays_offline());
    app->set_connectivity(trystero::relay_connectivity::automatic);
    net.page_connectivity(false);
    settle(io);
    BOOST_TEST(app->relays_offline());
    BOOST_TEST(room->events == (std::vector<std::string>{
                                   "relays offline",
                                   "relays online",
                                   "relays online",
                                   "relays offline",
                               }));
}

// The last user's release ends the watch of the page: an app a pending operation still holds
// hears no page event after it, so nothing reaches the rooms or the program's log.
void the_last_user_ends_the_page_watch() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    net.page_events = true;
    lines log;
    const auto app = make_app(io, net, log);
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", "T1", room);
    app->add_user();
    net.page_connectivity(false);
    settle(io);
    BOOST_TEST(app->relays_offline());
    app->release_user();
    log.clear();
    net.page_connectivity(true);
    settle(io);
    BOOST_TEST(app->relays_offline());
    BOOST_TEST(log.empty());
    BOOST_TEST(room->events == (std::vector<std::string>{"relays offline"}));
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
    const std::string self_id(40, 'x');  // beyond the short string
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
                                                     io.get_executor(), net, std::move(config),
                                                     self_id);
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
// live; a second connection to the same peer is held, and the first returned; once the peer sends
// on the first, the second is closed.
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
    settle(io);
    BOOST_TEST(second.local->is_open());
    BOOST_TEST(c->live());
    BOOST_TEST_EQ(app->live_connections().size(), 1U);
    first.remote->send(trystero::protocol::presence_frame("T1", true));
    BOOST_TEST(run_until(io, [&] { return !second.local->is_open(); }));
    BOOST_TEST(c->live());
    BOOST_TEST(app->find("B") == c);
    BOOST_TEST(
        log ==
        (lines{
            "webrtc: connection with B adopted",
            "webrtc: a second connection with B held until the peer shows which one it keeps",
            "webrtc: the second connection with B closed: the peer keeps the first",
        }));
    BOOST_TEST(r1->events == (std::vector<std::string>{"+B"}));
    BOOST_TEST(r2->events.empty());
}

// A connection opened once the peer sent on the first is closed at once: the peer keeps the
// first, and closes its own other one.
void another_connection_after_the_peers_first_frame_is_closed() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log);
    app->register_room("r1", "T1", std::make_shared<fake_room>());
    const auto first = negotiate(io, net);
    const auto c = app->adopt("B", first.local_pc, first.local);
    first.remote->send(trystero::protocol::presence_frame("T1", true));
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return c->used(); })));
    const auto second = negotiate(io, net);
    BOOST_TEST(app->adopt("B", second.local_pc, second.local) == c);
    BOOST_TEST(!second.local->is_open());
    BOOST_TEST(log == (lines{
                          "webrtc: connection with B adopted",
                          "webrtc: another connection with B closed: the peer keeps the first",
                      }));
}

/** An app of self id `self`, whose timers run on the virtual clock of `net`. */
std::shared_ptr<detail::app_impl> make_app_as(asio::io_context& io,
                                              trystero::test::memory_network& net, lines& log,
                                              std::string_view self) {
    net.virtual_clock = true;
    return std::make_shared<detail::app_impl>(
        io.get_executor(), net, trystero::app_config{.app_id = "app", .log = record_log(log)},
        self);
}

// The lower id holds the second until the peer closes the first, as Trystero JS closes the other
// connection of a crossed pair: the second then takes the first's place, after the rooms heard
// the first's end, and hands them the frames it received meanwhile.
void the_second_replaces_a_first_the_peer_closes() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app_as(io, net, log, "A");
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", "T1", room);
    const auto first = negotiate(io, net);
    const auto c = app->adopt("B", first.local_pc, first.local);
    const auto second = negotiate(io, net);
    app->adopt("B", second.local_pc, second.local);
    second.remote->send(trystero::protocol::presence_frame("T1", true));
    settle(io);
    BOOST_TEST(app->find("B") == c);  // the lower id waits for the peer
    BOOST_TEST(room->events.empty());
    first.remote->close();
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return app->find("B") != c; })));
    const auto replacement = app->find("B");
    webcpp::test::require(BOOST_TEST(replacement != nullptr));
    BOOST_TEST(replacement->live());
    BOOST_TEST(replacement->advertised("T1"));
    BOOST_TEST(room->events == (std::vector<std::string>{"ended B: data channel closed", "+B"}));
    BOOST_TEST(run_until(io, [&] { return second.received->size() == 1; }));
    BOOST_TEST(second.received->at(0) == presence("T1", true));  // the replacement advertises
    BOOST_TEST_EQ(app->live_connections().size(), 1U);
}

// The higher id gives way: the peer's first frame on the second makes the second replace the
// first, which ends and so closes the peer's second.
void the_higher_id_takes_the_second_the_peer_sends_on() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app_as(io, net, log, "Z");
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", "T1", room);
    const auto first = negotiate(io, net);
    const auto c = app->adopt("B", first.local_pc, first.local);
    const auto second = negotiate(io, net);
    app->adopt("B", second.local_pc, second.local);
    second.remote->send(trystero::protocol::presence_frame("T1", true));
    webcpp::test::require(BOOST_TEST(run_until(io, [&] { return app->find("B") != c; })));
    BOOST_TEST(!c->live());
    BOOST_TEST(run_until(io, [&] { return !first.remote->is_open(); }));
    BOOST_TEST(app->find("B")->advertised("T1"));
    BOOST_TEST(room->events ==
               (std::vector<std::string>{"ended B: the peer keeps the second connection", "+B"}));
    BOOST_TEST_EQ(app->live_connections().size(), 1U);
}

// A second the peer neither sends on nor closes, while it sends nothing on the first either, is
// closed after 2 s by the higher id and after 10 s by the lower id; the first stays.
void a_second_the_peer_leaves_alone_is_closed_in_time() {
    for (const auto& [self, wait] :
         {std::pair{std::string_view("Z"), 2'000ms}, std::pair{std::string_view("A"), 10'000ms}}) {
        asio::io_context io;
        trystero::test::memory_network net(io);
        lines log;
        const auto app = make_app_as(io, net, log, self);
        app->register_room("r1", "T1", std::make_shared<fake_room>());
        const auto first = negotiate(io, net);
        const auto c = app->adopt("B", first.local_pc, first.local);
        const auto second = negotiate(io, net);
        app->adopt("B", second.local_pc, second.local);
        BOOST_TEST(net.advance(wait - 1ms));
        settle(io);
        BOOST_TEST(second.local->is_open());
        BOOST_TEST(net.advance(1ms));
        settle(io);
        BOOST_TEST(!second.local->is_open());
        BOOST_TEST(c->live());
        BOOST_TEST_EQ(log.back(),
                      "webrtc: the second connection with B closed: the peer kept neither");
    }
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
// first, as Trystero clears a stale peer (dist/shared-peer.mjs:142-149), so the rooms hear its end
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
// replacement the outer adoption brought is held as the second, and the room's is the one kept,
// so the peer has one connection, whose end by this side closes the second too and leaves none
// (Trystero's owns(), dist/shared-peer.mjs:210-212).
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
    BOOST_TEST(second.local->is_open());
    BOOST_TEST(nested->live());
    BOOST_TEST_EQ(app->live_connections().size(), 1U);
    settle(io);
    BOOST_TEST(app->find("B") == nested);
    room->on_end = {};
    nested->end("test");
    BOOST_TEST(app->find("B") == nullptr);
    BOOST_TEST(app->live_connections().empty());
    BOOST_TEST(!second.local->is_open());
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
    for (std::size_t i = 0; i < 5; ++i) {
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

/** A resource over new and delete that counts the blocks still holding `secret` when freed. */
class secret_scanning_resource final : public std::pmr::memory_resource {
public:
    explicit secret_scanning_resource(std::string_view secret) : secret_(secret) {}

    /** How many freed blocks held the secret's bytes. */
    std::size_t holding = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        const std::string_view block(static_cast<const char*>(p), bytes);
        if (block.find(secret_) != std::string_view::npos) {
            ++holding;
        }
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::string_view secret_;
};

// No block the app frees holds its password: the app's own copy, the cache's and a derivation's
// are wiped before they are released, as the app itself is.
void the_password_is_wiped_before_it_is_freed() {
    const std::string password = "a password no freed block of the app may still hold";
    secret_scanning_resource memory(password);
    {
        asio::io_context io;
        trystero::test::memory_network net(io);
        trystero::app_config config{.memory = &memory};
        config.app_id = "app";
        config.password = password;
        auto app = std::allocate_shared<detail::app_impl>(
            std::pmr::polymorphic_allocator<>(&memory), io.get_executor(), net, std::move(config));
        bool derived = false;
        app->room_keys().get(
            "room", [&derived](boost::system::error_code ec, const trystero::protocol::room_keys&) {
                BOOST_TEST(!ec);
                derived = true;
            });
        BOOST_TEST(run_until(io, [&derived] { return derived; }));
        BOOST_TEST_EQ(app->password(), password);
        app.reset();
        settle(io);
    }
    BOOST_TEST_EQ(memory.holding, 0U);
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
    // Each case in a frame of its own, which main's never holds (cases.hpp).
    using webcpp::test::run_case;
    run_case(the_configuration_is_copied_and_completed);
    run_case(what_the_configuration_names_is_kept);
    run_case(what_no_peer_could_use_is_set_aside);
    run_case(no_custom_self_id);
    run_case(the_connectivity_reaches_every_room);
    run_case(an_automatic_app_follows_the_pages_events);
    run_case(the_last_user_ends_the_page_watch);
    run_case(every_allocation_goes_through_the_apps_resource);
    run_case(the_registry_holds_rooms_by_a_weak_reference);
    run_case(adopt_starts_one_connection_per_peer);
    run_case(another_connection_after_the_peers_first_frame_is_closed);
    run_case(the_second_replaces_a_first_the_peer_closes);
    run_case(the_higher_id_takes_the_second_the_peer_sends_on);
    run_case(a_second_the_peer_leaves_alone_is_closed_in_time);
    run_case(the_connections_events_reach_the_rooms);
    run_case(the_last_user_ends_every_connection);
    run_case(a_stale_connection_ends_before_its_replacement);
    run_case(an_adoption_from_inside_a_stale_end_is_the_one_kept);
    run_case(a_nested_adoption_gone_stale_ends_before_the_outer_one);
    run_case(an_adoption_from_inside_an_end_under_way);
    run_case(an_end_takes_only_its_own_entry);
    run_case(a_view_into_the_stale_connection_names_its_replacement);
    run_case(the_apps_destruction_is_quiet);
    run_case(the_password_is_wiped_before_it_is_freed);
    // Under Asyncify, a main that suspended loses what it returns: the status goes first.
    return webcpp::test::exit_status(boost::report_errors());
}
