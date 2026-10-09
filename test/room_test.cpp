// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests whole rooms of C++ peers in one process, over the memory backend of
// support/memory_backend.hpp. WEBCPP_TEST_TRYSTERO_VERBOSE in the environment prints each room's
// log on the standard error.

#include <webcpp/trystero/room.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/core/lightweight_test.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "require.hpp"
#include "support/memory_backend.hpp"

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;
using namespace std::chrono_literals;

namespace {

constexpr const char* relay_a = "wss://relay-a.test";
constexpr const char* relay_b = "wss://relay-b.test";

trystero::room_config config(std::string_view self_id, std::string_view password = {}) {
    trystero::room_config c;
    c.app_id = "webcpp-trystero-tests";
    c.password = password;
    c.relay_urls = {relay_a, relay_b};
    c.self_id = self_id;
    if (std::getenv("WEBCPP_TEST_TRYSTERO_VERBOSE") != nullptr) {
        c.log = [id = c.self_id](std::string_view category, std::string_view text) {
            std::cerr << "  [" << id << "] " << category << ": " << text << "\n";
        };
    }
    return c;
}

/** Runs the io_context until `done` holds or `limit` passes; whether `done` held. */
bool run_until(asio::io_context& io, const std::function<bool()>& done,
               std::chrono::milliseconds limit = 10s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < end) {
        io.run_for(20ms);  // lint-run: the memory relays and the rooms' timers post to it
        if (io.stopped()) {
            io.restart();
        }
    }
    return done();
}

struct fixture {
    asio::io_context io;
    trystero::test::memory_network net{io};

    fixture() {
        net.relay(relay_a);
        net.relay(relay_b);
    }
};

void two_peers_join_and_exchange_every_payload_format() {
    fixture f;
    // "A..." sorts below "z...", so A offers and z answers; the other test below swaps nothing but
    // the ids.
    trystero::room a(f.io.get_executor(), f.net, config("AAAAAAAAAAAAAAAAAAAA"), "lobby");
    trystero::room z(f.io.get_executor(), f.net, config("zzzzzzzzzzzzzzzzzzzz"), "lobby");
    std::vector<std::string> joined_a;
    std::vector<std::string> joined_z;
    a.on_peer_join([&](std::string_view p) { joined_a.emplace_back(p); });
    z.on_peer_join([&](std::string_view p) { joined_z.emplace_back(p); });

    auto chat_a = a.make_action<std::string>("chat");
    auto chat_z = z.make_action<std::string>("chat");
    auto drink_z = z.make_action<boost::json::value>("drink");
    auto pic_z = z.make_action<trystero::bytes>("pic");
    std::vector<std::pair<std::string, trystero::message_info>> chats_z;
    boost::json::value drink;
    trystero::bytes pic;
    chat_z.on_message([&](std::string text, const trystero::message_info& info) {
        chats_z.emplace_back(std::move(text), info);
    });
    drink_z.on_message(
        [&](boost::json::value v, const trystero::message_info&) { drink = std::move(v); });
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
    a.make_action<boost::json::value>("drink").async_send(
        boost::json::object{{"drink", "negroni"}},
        [&](boost::system::error_code ec) { sent.push_back(ec); });
    a.make_action<trystero::bytes>("pic").async_send(
        trystero::bytes{0, 1, 255}, [&](boost::system::error_code ec) { sent.push_back(ec); });

    BOOST_TEST(run_until(
        f.io, [&] { return chats_z.size() == 1 && !drink.is_null() && pic.size() == 3; }));
    BOOST_TEST_EQ(sent.size(), 3U);
    for (const auto& ec : sent) {
        BOOST_TEST(!ec);
    }
    BOOST_TEST_EQ(chats_z.at(0).first, "hello");
    BOOST_TEST_EQ(chats_z.at(0).second.from, "AAAAAAAAAAAAAAAAAAAA");
    BOOST_TEST_EQ(chats_z.at(0).second.metadata, R"({"n":1})");
    BOOST_TEST_EQ(drink.at("drink").as_string(), "negroni");
    const trystero::bytes sent_pic{0, 1, 255};
    BOOST_TEST_ALL_EQ(pic.begin(), pic.end(), sent_pic.begin(), sent_pic.end());
}

void large_payloads_travel_in_chunks_both_ways() {
    fixture f;
    trystero::room a(f.io.get_executor(), f.net, config("BBBBBBBBBBBBBBBBBBBB"), "files");
    trystero::room b(f.io.get_executor(), f.net, config("CCCCCCCCCCCCCCCCCCCC"), "files");
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
    int done = 0;
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

void ping_measures_a_round_trip() {
    fixture f;
    trystero::room a(f.io.get_executor(), f.net, config("DDDDDDDDDDDDDDDDDDDD"), "ping");
    const trystero::room b(f.io.get_executor(), f.net, config("EEEEEEEEEEEEEEEEEEEE"), "ping");
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
    fixture f;
    trystero::room a(f.io.get_executor(), f.net, config("FFFFFFFFFFFFFFFFFFFF", "s3cret"),
                     "private");
    trystero::room b(f.io.get_executor(), f.net, config("GGGGGGGGGGGGGGGGGGGG", "s3cret"),
                     "private");
    BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }));

    // The intruder's signals cannot be opened with the room key, so it never connects; both sides
    // report it.
    std::vector<boost::system::error_code> errors;
    trystero::room intruder(f.io.get_executor(), f.net, config("HHHHHHHHHHHHHHHHHHHH", "guess"),
                            "private");
    intruder.on_error([&](std::string_view, boost::system::error_code ec, std::string_view) {
        errors.push_back(ec);
    });
    a.on_error([&](std::string_view, boost::system::error_code ec, std::string_view) {
        errors.push_back(ec);
    });
    run_until(f.io, [] { return false; }, 1500ms);
    BOOST_TEST(intruder.peers().empty());
    BOOST_TEST_EQ(a.peers().size(), 1U);
    BOOST_TEST(!errors.empty());
    for (const auto& ec : errors) {
        BOOST_TEST(ec == trystero::error::wrong_password);
    }
}

void leaving_is_seen_by_the_others_and_a_return_reconnects() {
    fixture f;
    trystero::room a(f.io.get_executor(), f.net, config("IIIIIIIIIIIIIIIIIIII"), "leave");
    std::vector<std::string> left;
    std::vector<std::string> joined;
    a.on_peer_leave([&](std::string_view p) { left.emplace_back(p); });
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    {
        trystero::room b(f.io.get_executor(), f.net, config("JJJJJJJJJJJJJJJJJJJJ"), "leave");
        BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1; }));
        bool done = false;
        b.async_leave([&](boost::system::error_code ec) { done = !ec; });
        BOOST_TEST(run_until(f.io, [&] { return done && left.size() == 1; }));
        BOOST_TEST(a.peers().empty());
    }
    const trystero::room b2(f.io.get_executor(), f.net, config("JJJJJJJJJJJJJJJJJJJJ"), "leave");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 2; }));
}

// A room moved over a joined one leaves it as destroying it does: the other peer sees it go, and
// its pending operations complete with room_left, while an action of it still holds its state.
void moving_a_room_over_a_joined_one_leaves_it() {
    fixture f;
    trystero::room a(f.io.get_executor(), f.net, config("KKKKKKKKKKKKKKKKKKKK"), "move");
    std::vector<std::string> left;
    std::vector<std::string> joined;
    a.on_peer_leave([&](std::string_view p) { left.emplace_back(p); });
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    trystero::room b(f.io.get_executor(), f.net, config("LLLLLLLLLLLLLLLLLLLL"), "move");
    auto pending = b.make_action<std::string>("pending");
    std::optional<boost::system::error_code> received;
    pending.async_receive([&](boost::system::error_code ec, const std::string& /*text*/,
                              const trystero::message_info& /*info*/) { received = ec; });
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1; }));

    b = trystero::room(f.io.get_executor(), f.net, config("MMMMMMMMMMMMMMMMMMMM"), "elsewhere");
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

// A server WebRTC would refuse is dropped when the room joins, and reported to on_error with no
// peer, after the constructor returns; the room goes on with the usable one, and joins.
void unusable_ice_servers_are_dropped_and_reported() {
    fixture f;
    f.net.record_servers = true;
    auto c = config("NNNNNNNNNNNNNNNNNNNN");
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
    trystero::room a(f.io.get_executor(), f.net, std::move(c), "ice");
    std::vector<std::pair<std::string, boost::system::error_code>> errors;
    a.on_error([&](std::string_view peer, boost::system::error_code ec, std::string_view /*what*/) {
        errors.emplace_back(peer, ec);
    });
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    const trystero::room b(f.io.get_executor(), f.net, config("OOOOOOOOOOOOOOOOOOOO"), "ice");
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
    trystero::room a(f.io.get_executor(), f.net, config("PPPPPPPPPPPPPPPPPPPP"), "refused");
    trystero::room b(f.io.get_executor(), f.net, config("QQQQQQQQQQQQQQQQQQQQ"), "refused");
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

/** A resource that counts the bytes it has out, and the most it had out at once. */
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t in_use = 0;
    std::size_t peak = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        in_use += bytes;
        peak = std::max(peak, in_use);
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        in_use -= bytes;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

/** Announcements of fresh peer ids on a room's topic, signed, through a socket of their own. */
class flooder {
public:
    flooder(fixture& f, std::string_view room)
        : f_(f), topic_(trystero::protocol::root_topic("webcpp-trystero-tests", room)) {
        socket_ = f.net.connect(relay_a,
                                {
                                    .open = [this] { open_ = true; },
                                    .message =
                                        [this](std::string_view text) {
                                            if (on_text) {
                                                on_text(text);
                                            }
                                        },
                                    .close = {},
                                },
                                std::pmr::get_default_resource());
        webcpp::test::require(BOOST_TEST(run_until(f.io, [this] { return open_; })));
        std::array<std::uint8_t, 32> secret{};
        secret.fill(9);
        signer_.emplace(*trystero::protocol::nostr::signer::from_secret(secret));
    }

    /** What the relay sends the flooder: the events of the topics it watches. */
    std::function<void(std::string_view)> on_text;

    /** Subscribes to a topic's events, which then reach `on_text`. */
    void watch(std::string_view topic) {
        const std::array<std::pmr::string, 1> topics{std::pmr::string(topic)};
        socket_->send(trystero::protocol::nostr::subscribe_message(
            "watch" + std::to_string(next_++), topics, 0));
    }

    /** Publishes a signal on a topic, in any peer's name. */
    void publish(std::string_view topic, const trystero::protocol::signal_message& m) {
        const std::array<std::uint8_t, 32> aux{};
        socket_->send(trystero::protocol::nostr::event_message(
            *signer_, f_.net.unix_seconds(), topic, trystero::protocol::write_signal(m), aux));
    }

    /** Announces one id `times` times. */
    void announce_as(std::string_view id, int times) {
        const std::array<std::uint8_t, 32> aux{};
        for (int i = 0; i < times; ++i) {
            const auto nonce = std::to_string(next_++);
            const auto content = trystero::protocol::write_signal({.peer_id = id}, nonce);
            socket_->send(trystero::protocol::nostr::event_message(*signer_, f_.net.unix_seconds(),
                                                                   topic_, content, aux));
        }
    }

    /** Announces `count` ids, each `prefix` and a number. */
    void announce(std::size_t count, std::string_view prefix) {
        const std::array<std::uint8_t, 32> aux{};
        for (std::size_t i = 0; i < count; ++i) {
            std::pmr::string id(prefix);
            id += std::to_string(next_++);
            const auto content = trystero::protocol::write_signal({.peer_id = id});
            socket_->send(trystero::protocol::nostr::event_message(*signer_, f_.net.unix_seconds(),
                                                                   topic_, content, aux));
        }
    }

private:
    fixture& f_;
    std::pmr::string topic_;
    std::shared_ptr<trystero::backend::websocket> socket_;
    std::optional<trystero::protocol::nostr::signer> signer_;
    bool open_ = false;
    std::size_t next_ = 0;
};

// Announcements are not sealed: anyone who knows the app and the room ids can announce any number
// of fresh ids. A room offers to at most max_unconnected_peers of them at once, its memory and the
// page's connections stay bounded, the peer it is connected to keeps working, and once the offers
// to the flooders expire their negotiations are gone and a new peer joins. The clock runs 100
// times faster, so that offers expire in about 0.6 s.
void a_flood_of_announcements_is_bounded() {
    constexpr std::size_t flood = 3000;
    constexpr std::size_t bound = trystero::detail::signaling::max_unconnected_peers;
    fixture f;
    f.net.time_scale = 100;
    counting_resource memory;
    auto c = config("00000000000000000000");  // lower than every flooder: it offers to each
    c.memory = &memory;
    trystero::room a(f.io.get_executor(), f.net, std::move(c), "flood");
    trystero::room b(f.io.get_executor(), f.net, config("BBBBBBBBBBBBBBBBBBBB"), "flood");
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    auto chat_a = a.make_action<std::string>("chat");
    auto chat_b = b.make_action<std::string>("chat");
    std::vector<std::string> heard;
    chat_b.on_message([&](const std::string& text, const trystero::message_info& /*info*/) {
        heard.push_back(text);
    });
    webcpp::test::require(BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1; })));
    const std::size_t joined_in_use = memory.in_use;
    const std::size_t joined_connections = f.net.live_connections();

    flooder attacker(f, "flood");
    const auto before = f.net.relay(relay_a).events_seen;
    attacker.announce(flood, "1flood");
    BOOST_TEST(run_until(f.io, [&] { return f.net.relay(relay_a).events_seen >= before + flood; }));
    chat_a.async_send("after the flood", [](boost::system::error_code) {});
    BOOST_TEST(run_until(f.io, [&] { return heard.size() == 1; }));
    BOOST_TEST_LE(f.net.peak_connections, joined_connections + (2 * bound));
    // Bounded: about 0.5 MiB here, the negotiations under way and the events remembered against
    // replays; without the bound, every flooder's negotiation (7.7 MiB for 3000, measured).
    constexpr std::size_t bounded = std::size_t{1} << 20U;
    BOOST_TEST_LE(memory.in_use, joined_in_use + bounded);

    // Once every offer to a flooder expired, its negotiation is gone, and a new peer joins.
    BOOST_TEST(run_until(f.io, [&] { return f.net.live_connections() == joined_connections; }));
    const trystero::room late(f.io.get_executor(), f.net, config("CCCCCCCCCCCCCCCCCCCC"), "flood");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 2; }));
    BOOST_TEST_EQ(heard.size(), 1U);

    // A second flood keeps the room as bounded, and the room working.
    const auto again = f.net.relay(relay_a).events_seen;
    attacker.announce(flood, "1again");
    BOOST_TEST(run_until(f.io, [&] { return f.net.relay(relay_a).events_seen >= again + flood; }));
    chat_a.async_send("after the second flood", [](boost::system::error_code) {});
    BOOST_TEST(run_until(f.io, [&] { return heard.size() == 2; }));
    BOOST_TEST_LE(memory.in_use, joined_in_use + bounded);
}

// A watcher without the room key answers every offer of A to B with garbage, in B's name, before
// B's own answer: A opens an answer before acting on it, so the forged ones change nothing, A and
// B join, and A reports the failed open at most once for B.
void a_forged_answer_changes_nothing() {
    fixture f;
    const std::string_view a_id = "AAAAAAAAAAAAAAAAAAAA";
    const std::string_view b_id = "BBBBBBBBBBBBBBBBBBBB";
    trystero::room a(f.io.get_executor(), f.net, config(a_id), "forged");
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    std::vector<boost::system::error_code> errors;
    a.on_error([&](std::string_view /*peer*/, boost::system::error_code ec,
                   std::string_view /*what*/) { errors.push_back(ec); });
    flooder attacker(f, "forged");
    const auto a_topic = trystero::protocol::peer_topic("webcpp-trystero-tests", "forged", a_id);
    int forged = 0;
    attacker.on_text = [&](std::string_view text) {
        boost::system::error_code ec;
        const auto document = boost::json::parse(text, ec);
        const auto message =
            ec ? std::nullopt : trystero::protocol::nostr::read_relay_message(document);
        const auto* event =
            message ? std::get_if<trystero::protocol::nostr::event>(&*message) : nullptr;
        if (event == nullptr) {
            return;
        }
        const auto content = boost::json::parse(event->content, ec);
        const auto signal = ec ? std::nullopt : trystero::protocol::read_signal(content);
        if (signal && signal->peer_id == a_id && !signal->offer.empty()) {
            attacker.publish(
                a_topic,
                {.peer_id = b_id, .offer_id = signal->offer_id, .answer = "not a sealed answer"});
            ++forged;
        }
    };
    attacker.watch(trystero::protocol::peer_topic("webcpp-trystero-tests", "forged", b_id));
    const trystero::room b(f.io.get_executor(), f.net, config(b_id), "forged");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1; }));
    BOOST_TEST_GE(forged, 1);
    BOOST_TEST_LE(errors.size(), 1U);
}

// Offers from fresh ids that are not sealed with the room key make no connection: a negotiation
// changes only once its offer opened.
void forged_offers_make_no_connection() {
    fixture f;
    const trystero::room a(f.io.get_executor(), f.net, config("zzzzzzzzzzzzzzzzzzzz"), "offers");
    flooder attacker(f, "offers");
    const auto a_topic =
        trystero::protocol::peer_topic("webcpp-trystero-tests", "offers", "zzzzzzzzzzzzzzzzzzzz");
    const auto before = f.net.relay(relay_a).events_seen;
    for (int i = 0; i < 200; ++i) {
        const std::string id = "0offer" + std::to_string(i);
        attacker.publish(a_topic,
                         {.peer_id = id, .offer_id = "offer", .offer = "not a sealed offer"});
    }
    BOOST_TEST(run_until(f.io, [&] { return f.net.relay(relay_a).events_seen >= before + 200; }));
    run_until(f.io, [] { return false; }, 200ms);  // the opens complete
    BOOST_TEST_EQ(f.net.made_connections, 0U);
}

// Beyond the bound, a new peer is refused, and one refused again and again is logged once: the
// offers to the first ids hold their places (the clock runs at its own pace here).
void a_refused_peer_is_logged_once() {
    fixture f;
    auto c = config("00000000000000000000");
    int refusals_of_one = 0;
    int refusals = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refusals += text.starts_with("refusing ") ? 1 : 0;
        refusals_of_one += text.starts_with("refusing 1same") ? 1 : 0;
    };
    const trystero::room a(f.io.get_executor(), f.net, std::move(c), "refusal");
    flooder attacker(f, "refusal");
    const auto before = f.net.relay(relay_a).events_seen;
    const std::size_t bound = trystero::detail::signaling::max_unconnected_peers;
    attacker.announce(bound + 8, "1fill");
    attacker.announce_as("1same", 5);
    BOOST_TEST(run_until(
        f.io, [&] { return f.net.relay(relay_a).events_seen >= before + bound + 8 + 5; }));
    BOOST_TEST(run_until(f.io, [&] { return refusals == 9; }));
    BOOST_TEST_EQ(refusals_of_one, 1);
    BOOST_TEST_LE(f.net.live_connections(), 2 * bound);
}

// A peer id longer than protocol::max_peer_id_bytes is ignored in a signal; a room configured with
// one uses a random id instead, and reports it.
void long_peer_ids_are_bounded() {
    const std::string longest(trystero::protocol::max_peer_id_bytes, 'x');
    const std::string too_long(trystero::protocol::max_peer_id_bytes + 1, 'x');
    const auto signal = [](std::string_view id) {
        return boost::json::value(boost::json::object{{"peerId", id}});
    };
    BOOST_TEST(trystero::protocol::read_signal(signal(longest)).has_value());
    BOOST_TEST(!trystero::protocol::read_signal(signal(too_long)).has_value());

    fixture f;
    trystero::room a(f.io.get_executor(), f.net, config(too_long), "long");
    std::vector<boost::system::error_code> errors;
    a.on_error([&](std::string_view /*peer*/, boost::system::error_code ec,
                   std::string_view /*what*/) { errors.push_back(ec); });
    BOOST_TEST_EQ(a.self_id().size(), trystero::protocol::peer_id_size);
    BOOST_TEST(run_until(f.io, [&] { return errors.size() == 1; }));
    if (!errors.empty()) {
        BOOST_TEST(errors.front() == trystero::error::invalid_configuration);
    }
}

void three_peers_form_a_mesh() {
    fixture f;
    std::vector<std::unique_ptr<trystero::room>> rooms;
    for (const char* id :
         {"KKKKKKKKKKKKKKKKKKKK", "LLLLLLLLLLLLLLLLLLLL", "MMMMMMMMMMMMMMMMMMMM"}) {
        rooms.push_back(
            std::make_unique<trystero::room>(f.io.get_executor(), f.net, config(id), "mesh"));
    }
    BOOST_TEST(run_until(f.io, [&] {
        for (auto& r : rooms) {
            if (r->peers().size() != 2) {
                return false;
            }
        }
        return true;
    }));
    // A broadcast reaches both others.
    auto shout = rooms[0]->make_action<std::string>("shout");
    int heard = 0;
    for (std::size_t i = 1; i < rooms.size(); ++i) {
        rooms[i]->make_action<std::string>("shout").on_message(
            [&](const std::string& text, const trystero::message_info& /*info*/) {
                heard += text == "all" ? 1 : 0;
            });
    }
    shout.async_send("all", [](boost::system::error_code ec) { BOOST_TEST(!ec); });
    BOOST_TEST(run_until(f.io, [&] { return heard == 2; }));
}

void async_receive_works_in_a_coroutine() {
    fixture f;
    trystero::room a(f.io.get_executor(), f.net, config("NNNNNNNNNNNNNNNNNNNN"), "coro");
    trystero::room b(f.io.get_executor(), f.net, config("OOOOOOOOOOOOOOOOOOOO"), "coro");
    auto inbox = b.make_action<trystero::payload>("any");
    std::vector<std::string> seen;
    asio::co_spawn(
        f.io,
        [&]() -> asio::awaitable<void> {
            for (int i = 0; i < 2; ++i) {
                auto [ec, value, info] =
                    co_await inbox.async_receive(asio::as_tuple(asio::use_awaitable));
                if (ec) {
                    co_return;
                }
                seen.push_back(std::holds_alternative<std::string>(value)
                                   ? std::get<std::string>(value)
                                   : boost::json::serialize(std::get<boost::json::value>(value)));
            }
        },
        asio::detached);
    BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1; }));
    a.make_action<std::string>("any").async_send("text", asio::detached);
    a.make_action<boost::json::value>("any").async_send(boost::json::array{1, 2}, asio::detached);
    BOOST_TEST(run_until(f.io, [&] { return seen.size() == 2; }));
    const std::vector<std::string> both{"text", "[1,2]"};
    BOOST_TEST_ALL_EQ(seen.begin(), seen.end(), both.begin(), both.end());
}

void view_handlers_read_the_frame_without_copying() {
    fixture f;
    trystero::room a(f.io.get_executor(), f.net, config("RRRRRRRRRRRRRRRRRRRR"), "views");
    trystero::room b(f.io.get_executor(), f.net, config("SSSSSSSSSSSSSSSSSSSS"), "views");
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
    trystero::room a(f.io.get_executor(), f.net, config("PPPPPPPPPPPPPPPPPPPP"), "limits");
    trystero::room b(f.io.get_executor(), f.net, config("QQQQQQQQQQQQQQQQQQQQ"), "limits");
    BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1 && b.peers().size() == 1; }));
    // Backoff: after the first refusal each peer stops publishing on relay A, so it sees only a
    // handful of attempts.
    BOOST_TEST_LT(1000 - f.net.relay(relay_a).rate_limit_next, 12);
}

}  // namespace

int main() {
    two_peers_join_and_exchange_every_payload_format();
    large_payloads_travel_in_chunks_both_ways();
    ping_measures_a_round_trip();
    the_same_password_joins_and_another_does_not();
    leaving_is_seen_by_the_others_and_a_return_reconnects();
    moving_a_room_over_a_joined_one_leaves_it();
    ice_urls_are_checked_as_webrtc_checks_them();
    unusable_ice_servers_are_dropped_and_reported();
    a_refused_peer_connection_is_reported_and_tried_again();
    a_flood_of_announcements_is_bounded();
    long_peer_ids_are_bounded();
    a_refused_peer_is_logged_once();
    a_forged_answer_changes_nothing();
    forged_offers_make_no_connection();
    three_peers_form_a_mesh();
    async_receive_works_in_a_coroutine();
    view_handlers_read_the_frame_without_copying();
    a_rate_limited_relay_does_not_stop_discovery();
    return boost::report_errors();
}
