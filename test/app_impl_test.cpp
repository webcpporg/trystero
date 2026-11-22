// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The state an app and its rooms share, alone, over a memory network: the configuration it
// completes and what it sets aside, the registry of rooms, the relays' connectivity it hands every
// room, and the page's watch, which ends with its last user. It shares no connection: each room
// has its own.

#include <webcpp/trystero/detail/app_impl.hpp>
#include <webcpp/trystero/identity.hpp>
#include <webcpp/trystero/options.hpp>
#include <webcpp/trystero/protocol/ids.hpp>
#include <webcpp/trystero/protocol/relays.hpp>
#include <webcpp/trystero/protocol/topics.hpp>

#include "support/memory_backend.hpp"

#include <webcpp/asio/io_context.hpp>

#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

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

/** A room as the app sees it, which records every call: "relays offline", "relays online". */
class fake_room final : public detail::room_endpoint {
public:
    [[nodiscard]] bool leaving() const override { return false; }

    void finish_farewell() override {}

    void relays_offline(bool offline) override {
        events.emplace_back(offline ? "relays offline" : "relays online");
    }

    std::vector<std::string> events;
};

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

// The self id derives from the endpoint key the app drew, the user id from the ephemeral user
// identity it made, the ICE servers default to Trystero's, and the relays are derived from the app
// id; nothing is set aside.
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
    BOOST_TEST_EQ(app->self_id(), trystero::protocol::derive_peer_id(app->endpoint_key()));
    BOOST_TEST(trystero::protocol::parse_compact_public_key(app->endpoint_key()).has_value());
    BOOST_TEST_EQ(app->user_id().size(), trystero::protocol::user_id_size);
    BOOST_TEST(app->persistence() == trystero::identity_persistence::ephemeral);
    BOOST_TEST(app->identity_failure().empty());
    BOOST_TEST(urls_of(app->ice_servers()) == urls_of(trystero::default_ice_servers()));
    const auto derived =
        trystero::protocol::derive_relays("app", trystero::protocol::default_nostr_relays, 5);
    BOOST_TEST_EQ(app->relay_urls().size(), 5U);
    BOOST_TEST(std::ranges::equal(app->relay_urls(), derived));
    BOOST_TEST(app->setup_reports().empty());
    BOOST_TEST(log.empty());
    BOOST_TEST(app->memory() == std::pmr::get_default_resource());
    BOOST_TEST(app->get_executor() == io.get_executor());
    BOOST_TEST(&app->factory() == &net);
    BOOST_TEST_EQ(app->counted_rooms(), 0U);
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
    });
    BOOST_TEST_EQ(app->password(), "the immutable password of every room in this app");
    BOOST_TEST_EQ(app->relay_urls().size(), 1U);
    BOOST_TEST_EQ(app->relay_urls()[0], "wss://relay.example");
    BOOST_TEST(urls_of(app->ice_servers()) == (std::vector<std::string>{"stun:stun.example:3478"}));
    BOOST_TEST(app->setup_reports().empty());
}

// Every ICE server no peer connection could be made with is set aside, each reported once and
// logged, in their order.
void what_no_peer_could_use_is_set_aside() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto app = make_app(io, net, log, [](trystero::app_config& c) {
        c.ice_servers.push_back(server("http://example"));
        c.ice_servers.push_back(server("stun:stun.example"));
        c.ice_servers.push_back({});
    });
    BOOST_TEST(urls_of(app->ice_servers()) == (std::vector<std::string>{"stun:stun.example"}));
    const std::vector<std::string> expected{
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

// No configuration names the self id, as 0.27.0 has no custom one: each app draws its own
// endpoint key from the system's generator, as 0.27.0 draws it with WebCrypto, not from the
// factory's randomness, and derives its 20 base62 characters from it, so that two apps of one
// network differ; and no room option names a handshake timeout (the static assertions above).
void no_custom_self_id() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    lines log;
    const auto before = net.random_bytes;
    const auto a = make_app(io, net, log);
    BOOST_TEST_EQ(net.random_bytes, before);
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
    app->register_room("r1", r1);
    app->register_room("r2", r2);
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
    app->register_room("r1", room);
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
    app->register_room("r1", room);
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

// Every allocation of the app's construction, of its registry, of a log sentence, of the
// connectivity's fan-out to the rooms, of its key records and verified signatures and of the last
// user's release goes through its resource: the default resource refuses every request meanwhile.
void every_allocation_goes_through_the_apps_resource() {
    std::pmr::unsynchronized_pool_resource pool;  // outlives the io_context
    asio::io_context io;
    trystero::test::memory_network net(io);
    trystero::app_config config{.memory = &pool};
    config.app_id = "app";
    const std::string expected_password(60, 'p');
    config.password = expected_password;
    config.ice_servers.push_back(server("stun:stun.example:3478"));
    std::size_t sentences = 0;
    config.log = [&sentences](std::string_view, std::string_view) { ++sentences; };
    const auto room = std::make_shared<fake_room>();
    const auto signer = trystero::create_user_identity();
    if (!BOOST_TEST(signer.has_value())) {
        return;
    }
    constexpr std::string_view payload = "payload";
    const std::span<const std::uint8_t> payload_bytes(
        reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    const auto signature = signer->sign(payload_bytes);
    if (!BOOST_TEST(signature.has_value())) {
        return;
    }
    std::shared_ptr<detail::app_impl> app;
    auto* const previous = std::pmr::set_default_resource(std::pmr::null_memory_resource());
    {
        // The endpoint key and the ephemeral user identity drawn, their texts derived.
        app = std::allocate_shared<detail::app_impl>(std::pmr::polymorphic_allocator<>(&pool),
                                                     io.get_executor(), net, std::move(config));
        BOOST_TEST_EQ(app->self_id().size(), trystero::protocol::peer_id_size);
        BOOST_TEST_EQ(app->user_id().size(), trystero::protocol::user_id_size);
        BOOST_TEST_EQ(app->password(), expected_password);
        BOOST_TEST(app->identities().verify_peer_id(app->self_id(), app->endpoint_key()));
        BOOST_TEST(app->identities().verify(signer->public_key(), *signature, payload_bytes));
        BOOST_TEST_EQ(app->identities().signatures(), 1U);
        app->register_room("a room id longer than the short string", room);
        app->add_user();
        app->set_connectivity(trystero::relay_connectivity::offline);
        app->release_user();
    }
    std::pmr::set_default_resource(previous);
    BOOST_TEST_EQ(sentences, 1U);  // the relays go offline
    BOOST_TEST(room->events == (std::vector<std::string>{"relays offline"}));
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
    app->register_room("r1", r1);
    BOOST_TEST_EQ(app->counted_rooms(), 1U);
    BOOST_TEST(app->find_room("r1") == r1);
    BOOST_TEST(app->find_room("r2") == nullptr);
    app->unregister_room("r1", other.get());
    BOOST_TEST(app->find_room("r1") == r1);
    app->unregister_room("r1", r1.get());
    BOOST_TEST_EQ(app->counted_rooms(), 0U);
    BOOST_TEST(app->find_room("r1") == nullptr);
    app->register_room("r1", r1);
    app->register_room("r1", other);  // the same id: the later room's entry
    BOOST_TEST_EQ(app->counted_rooms(), 1U);
    BOOST_TEST(app->find_room("r1") == other);
    app->register_room("r2", r1);
    r1.reset();
    BOOST_TEST(app->find_room("r2") == nullptr);  // the registry did not keep it
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

// The app's destruction ends nothing and calls nothing back: a page event after it reaches no room
// and no log, as the watch went with it.
void the_apps_destruction_is_quiet() {
    asio::io_context io;
    trystero::test::memory_network net(io);
    net.page_events = true;
    lines log;
    auto app = make_app(io, net, log);
    const auto room = std::make_shared<fake_room>();
    app->register_room("r1", room);
    app.reset();
    settle(io);
    net.page_connectivity(false);
    settle(io);
    BOOST_TEST(room->events.empty());
    BOOST_TEST(log.empty());
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
    run_case(the_apps_destruction_is_quiet);
    run_case(the_password_is_wiped_before_it_is_freed);
    // Under Asyncify, a main that suspended loses what it returns: the status goes first.
    return webcpp::test::exit_status(boost::report_errors());
}
