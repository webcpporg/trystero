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
#include <map>
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
constexpr const char* app_id = "webcpp-trystero-tests";

trystero::room_config config(std::string_view self_id, std::string_view password = {}) {
    trystero::room_config c;
    c.app_id = app_id;
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
        : f_(f), topic_(trystero::protocol::root_topic(app_id, room)) {
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

    /** `plain` sealed under the room key of `room`, with no password, as the memory sealer seals.
     */
    static std::string sealed(std::string_view room, std::string_view plain) {
        const auto key = trystero::protocol::seal_key("", app_id, room);
        return std::string(trystero::protocol::format_sealed(
            std::span(key).first(16),
            std::span(reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size())));
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
    counting_resource memory;  // first: the io_context's pending operations free into it
    fixture f;
    f.net.time_scale = 100;
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

    // Announces `count` fresh ids, a thousand at a time, each thousand passed by the relay before
    // the next, then has a message go through, and returns what the room holds then, beyond what
    // it held once joined. The memory relay queues what it forwards in each room's resource, and
    // a flood sent at once would hold it all there, more than Emscripten's 16 MiB. The page's
    // connections are counted from those it holds when the flood starts, the late peer's among
    // them in the second. The flood adds at most `2 * bound`: the offers of the negotiations under
    // way, and those of the ones just expired, closed but held by the completions queued for them
    // until the executor reaches them, behind the relays' messages on a loaded machine.
    flooder attacker(f, "flood");
    const auto flooded = [&](std::size_t count, std::string_view prefix) {
        const std::size_t held = f.net.live_connections();
        f.net.peak_connections = held;
        constexpr std::size_t batch = 1000;
        for (std::size_t sent = 0; sent < count; sent += batch) {
            const std::size_t now = std::min(batch, count - sent);
            const auto before = f.net.relay(relay_a).events_seen;
            attacker.announce(now, prefix);
            BOOST_TEST(
                run_until(f.io, [&] { return f.net.relay(relay_a).events_seen >= before + now; }));
        }
        const std::size_t messages = heard.size() + 1;
        chat_a.async_send("after the flood", [](boost::system::error_code) {});
        BOOST_TEST(run_until(f.io, [&] { return heard.size() == messages; }));
        BOOST_TEST_LE(f.net.peak_connections, held + (2 * bound));
        return memory.in_use - joined_in_use;
    };
    const std::size_t first = flooded(flood, "1flood");

    // Once every offer to a flooder expired, its negotiation is gone, and a new peer joins.
    BOOST_TEST(run_until(f.io, [&] { return f.net.live_connections() == joined_connections; }));
    const trystero::room late(f.io.get_executor(), f.net, config("CCCCCCCCCCCCCCCCCCCC"), "flood");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 2; }));
    BOOST_TEST_EQ(heard.size(), 1U);

    // A flood twice as large keeps the room as bounded, and the room working. The half of
    // `first` allows for the late peer, joined since, and for the records not yet full after the
    // first flood; without the bound on negotiations, the second flood leaves 2.6 times as much,
    // and 2.8 times without the one on the event ids remembered (measured).
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
    trystero::room a(f.io.get_executor(), f.net, config(a_id), "forged");
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    std::vector<boost::system::error_code> errors;
    a.on_error([&](std::string_view /*peer*/, boost::system::error_code ec,
                   std::string_view /*what*/) { errors.push_back(ec); });
    flooder attacker(f, "forged");
    const auto a_topic = trystero::protocol::peer_topic(app_id, "forged", a_id);
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
    attacker.watch(trystero::protocol::peer_topic(app_id, "forged", b_id));
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
    const auto a_topic = trystero::protocol::peer_topic(app_id, "offers", "zzzzzzzzzzzzzzzzzzzz");
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

// Unsealed offers from 2000 fresh ids, which anyone who reads the relays can send: the room reports
// the first at once, with its peer, and the rest together, at most once per interval, with no peer
// and their number, so that the application is not handed one report per id. The clock runs 10
// times faster.
void signals_that_do_not_open_are_reported_once_per_interval() {
    constexpr int flood = 2000;
    fixture f;
    f.net.time_scale = 10;
    const std::string a_id = "zzzzzzzzzzzzzzzzzzzz";
    trystero::room a(f.io.get_executor(), f.net, config(a_id), "reports");

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
    const auto started = f.net.now_ms();
    const auto before = f.net.relay(relay_a).events_seen;
    // Paced, so that each offer's open completes before later ids evict its negotiation: every
    // twenty, the executor runs until nothing is left to run, the relay, the room and the sealer's
    // opens, however slowly the machine runs them.
    for (int i = 0; i < flood; ++i) {
        attacker.publish(a_topic, {
                                      .peer_id = "0garbage" + std::to_string(i),
                                      .offer_id = "o",
                                      .offer = "not a sealed offer",
                                  });
        if (i % 20 == 19) {
            f.io.poll();
        }
    }
    BOOST_TEST(run_until(f.io, [&] { return f.net.relay(relay_a).events_seen >= before + flood; }));
    // Every failed open is counted once its interval ends.
    const auto counted = [&] {
        std::size_t sum = 0;
        for (const auto& r : reports) {
            sum += r.peer.empty() ? std::stoul(r.what) : 1;
        }
        return sum;
    };
    BOOST_TEST(run_until(f.io, [&] { return counted() == flood; }));
    const double elapsed = f.net.now_ms() - started;
    BOOST_TEST_LE(static_cast<double>(reports.size()), 2 + (elapsed / 10'000));
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
    auto c = config("zzzzzzzzzzzzzzzzzzzz");
    int refusals = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refusals += text.find(" connections were made in the last ") != std::string_view::npos;
    };
    const trystero::room a(f.io.get_executor(), f.net, std::move(c), "churn");
    flooder member(f, "churn");
    const auto a_topic = trystero::protocol::peer_topic(app_id, "churn", "zzzzzzzzzzzzzzzzzzzz");
    // One at a time, each once the previous one made its connection or was refused: sent at once,
    // the offers would evict each other's negotiations before they open. Each is a text of its
    // own, as a member makes them: the room refuses an offer it used before.
    for (std::size_t i = 0; i < flood; ++i) {
        const auto offer =
            flooder::sealed("churn", "not an SDP the connection takes " + std::to_string(i));
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
    auto c = config(a_id);
    std::size_t refusals = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refusals += text.starts_with("refusing ") ? 1 : 0;
    };
    const trystero::room a(f.io.get_executor(), f.net, std::move(c), "churn");
    flooder attacker(f, "churn");
    const auto a_topic = trystero::protocol::peer_topic(app_id, "churn", a_id);
    std::map<std::string, std::string, std::less<>> owners;  // peer topic -> its peer id
    std::size_t forged = 0;
    attacker.on_text = [&](std::string_view text) {
        boost::system::error_code ec;
        const auto document = boost::json::parse(text, ec);
        const auto message =
            ec ? std::nullopt : trystero::protocol::nostr::read_relay_message(document);
        const auto* event =
            message ? std::get_if<trystero::protocol::nostr::event>(&*message) : nullptr;
        const auto topic = event == nullptr ? std::string_view() : event->topic;
        const auto owner = owners.find(topic);
        if (owner == owners.end()) {
            return;
        }
        const auto content = boost::json::parse(event->content, ec);
        const auto signal = ec ? std::nullopt : trystero::protocol::read_signal(content);
        if (signal && signal->peer_id == a_id && !signal->offer.empty()) {
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

/** A relay's event as a watcher reads it: its topic and its signal. */
struct relayed {
    std::string topic;
    std::string peer;
    std::string offer_id;
    std::string offer;
    std::string answer;
    bool announcement = false;
};

/** The topic and the signal of a relay's text, when it is an event that carries one. */
std::optional<relayed> relayed_signal(std::string_view text) {
    boost::system::error_code ec;
    const auto document = boost::json::parse(text, ec);
    const auto message =
        ec ? std::nullopt : trystero::protocol::nostr::read_relay_message(document);
    const auto* event =
        message ? std::get_if<trystero::protocol::nostr::event>(&*message) : nullptr;
    if (event == nullptr) {
        return std::nullopt;
    }
    const auto content = boost::json::parse(event->content, ec);
    const auto signal = ec ? std::nullopt : trystero::protocol::read_signal(content);
    if (!signal) {
        return std::nullopt;
    }
    return relayed{
        .topic = std::string(event->topic),
        .peer = std::string(signal->peer_id),
        .offer_id = std::string(signal->offer_id),
        .offer = std::string(signal->offer),
        .answer = std::string(signal->answer),
        .announcement = signal->is_announcement(),
    };
}

/** Joins B to A, records what `record` keeps of the relays meanwhile, and has B leave. */
void join_and_leave(fixture& f, std::string_view b_id, std::string_view room_id,
                    const std::vector<std::string>& joined, const std::vector<std::string>& left,
                    const std::function<bool()>& recorded) {
    trystero::room b(f.io.get_executor(), f.net, config(b_id), room_id);
    webcpp::test::require(
        BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1 && recorded(); })));
    bool done = false;
    b.async_leave([&](boost::system::error_code) { done = true; });
    webcpp::test::require(BOOST_TEST(run_until(f.io, [&] { return done && left.size() == 1; })));
}

// A watcher without the room key records B's sealed answer to A. Once B has left and come back with
// the same id, as a Trystero JS page does after a network drop, it sends that answer in B's name
// with each of A's new offers to B, before B's own answer. The answer opens with the room key, but
// A used it before: A refuses it, blames no one, and B joins again. The clock runs 10 times
// faster.
//
// The watcher is armed as A sees B leave: an announcement B made before leaving may still reach A
// after it, and A's offer to that announcement is the one the returning B answers, on a relay
// where A does not send it again.
void a_replayed_answer_is_refused() {
    fixture f;
    f.net.time_scale = 10;
    const std::string a_id = "00000000000000000000";
    const std::string b_id = "BBBBBBBBBBBBBBBBBBBB";
    trystero::room a(f.io.get_executor(), f.net, config(a_id), "replay");
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
    int replays = 0;
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
    const trystero::room b(f.io.get_executor(), f.net, config(b_id), "replay");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 2; }));
    BOOST_TEST_GE(replays, 1);
    BOOST_TEST(std::ranges::none_of(errors, [](boost::system::error_code ec) {
        return ec == trystero::error::wrong_password;
    }));
}

// The same for an offer: B is lower and offers. The watcher records B's sealed offer to A, and once
// B is back, sends it in B's name every 0.5 s of the rooms' time, with fresh offer ids. A answered
// that offer before, so it refuses each replay, and B's own offer joins them. The clock runs 10
// times faster.
void a_replayed_offer_is_refused() {
    fixture f;
    f.net.time_scale = 10;
    const std::string a_id = "zzzzzzzzzzzzzzzzzzzz";
    const std::string b_id = "BBBBBBBBBBBBBBBBBBBB";
    trystero::room a(f.io.get_executor(), f.net, config(a_id), "replayo");
    std::vector<std::string> joined;
    std::vector<std::string> left;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    a.on_peer_leave([&](std::string_view p) { left.emplace_back(p); });
    flooder watcher(f, "replayo");
    const auto a_topic = std::string(trystero::protocol::peer_topic(app_id, "replayo", a_id));
    std::string recorded;
    watcher.on_text = [&](std::string_view text) {
        const auto s = relayed_signal(text);
        if (s && s->topic == a_topic && s->peer == b_id && !s->offer.empty() && recorded.empty()) {
            recorded = s->offer;
        }
    };
    watcher.watch(a_topic);
    join_and_leave(f, b_id, "replayo", joined, left, [&] { return !recorded.empty(); });
    const trystero::room b(f.io.get_executor(), f.net, config(b_id), "replayo");
    int replays = 0;
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (joined.size() < 2 && std::chrono::steady_clock::now() < end) {
        watcher.publish(a_topic, {
                                     .peer_id = b_id,
                                     .offer_id = "replay" + std::to_string(replays++),
                                     .offer = recorded,
                                 });
        run_until(f.io, [] { return false; }, 50ms);
    }
    BOOST_TEST_EQ(joined.size(), 2U);
    BOOST_TEST_GE(replays, 1);
}

// A watcher without the room key sends A's own sealed offer to B back to A, at once, as B's answer
// to it. It opens with the room key, but A made it: A refuses it, and B's own answer joins them.
void a_reflected_offer_is_refused_as_an_answer() {
    fixture f;
    const std::string a_id = "00000000000000000000";
    const std::string b_id = "BBBBBBBBBBBBBBBBBBBB";
    trystero::room a(f.io.get_executor(), f.net, config(a_id), "reflect");
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    flooder watcher(f, "reflect");
    const auto a_topic = std::string(trystero::protocol::peer_topic(app_id, "reflect", a_id));
    const auto b_topic = std::string(trystero::protocol::peer_topic(app_id, "reflect", b_id));
    int reflected = 0;
    watcher.on_text = [&](std::string_view text) {
        const auto s = relayed_signal(text);
        if (s && s->topic == b_topic && s->peer == a_id && !s->offer.empty()) {
            watcher.publish(a_topic,
                            {.peer_id = b_id, .offer_id = s->offer_id, .answer = s->offer});
            ++reflected;
        }
    };
    watcher.watch(b_topic);
    const trystero::room b(f.io.get_executor(), f.net, config(b_id), "reflect");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1; }));
    BOOST_TEST_GE(reflected, 1);
}

// A watcher without the room key announces a decoy id above A, records A's sealed offer to it, and
// sends that offer to A in B's name whenever B announces itself, before B's own offer. A made that
// offer: A refuses it, never answers one in B's name but B's own, and B's own offer joins them.
// The clock runs 10 times faster.
void a_reflected_offer_is_refused_as_an_offer() {
    fixture f;
    f.net.time_scale = 10;
    const std::string a_id = "yyyyyyyyyyyyyyyyyyyy";
    const std::string b_id = "BBBBBBBBBBBBBBBBBBBB";
    const std::string decoy = "zzzzzzzzzzzzzzzzzzzz";
    auto c = config(a_id);
    int answered_reflections = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        answered_reflections += text.starts_with("offer reflected") ? 1 : 0;
    };
    trystero::room a(f.io.get_executor(), f.net, std::move(c), "reflecto");
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    flooder watcher(f, "reflecto");
    const auto root = std::string(trystero::protocol::root_topic(app_id, "reflecto"));
    const auto a_topic = std::string(trystero::protocol::peer_topic(app_id, "reflecto", a_id));
    const auto decoy_topic = std::string(trystero::protocol::peer_topic(app_id, "reflecto", decoy));
    std::string recorded;
    int reflected = 0;
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
    webcpp::test::require(BOOST_TEST(run_until(f.io, [&] { return !recorded.empty(); })));
    const trystero::room b(f.io.get_executor(), f.net, config(b_id), "reflecto");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1; }));
    BOOST_TEST_GE(reflected, 1);
    BOOST_TEST_EQ(answered_reflections, 0);
}

// A answers B (B's id is lower), then offers to C (C's id is higher). A watcher without the room
// key sends A's own sealed answer to B back to A as C's answer to that offer. A made it: A refuses
// it, and C's own answer joins them.
void a_reflected_answer_is_refused_as_an_answer() {
    fixture f;
    const std::string a_id = "MMMMMMMMMMMMMMMMMMMM";
    const std::string b_id = "BBBBBBBBBBBBBBBBBBBB";
    const std::string c_id = "ZZZZZZZZZZZZZZZZZZZZ";
    auto c = config(a_id);
    bool refused = false;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        refused = refused || text.starts_with("refused the answer from " + c_id);
    };
    trystero::room a(f.io.get_executor(), f.net, std::move(c), "reflans");
    std::vector<std::string> joined;
    a.on_peer_join([&](std::string_view p) { joined.emplace_back(p); });
    flooder watcher(f, "reflans");
    const auto a_topic = std::string(trystero::protocol::peer_topic(app_id, "reflans", a_id));
    const auto b_topic = std::string(trystero::protocol::peer_topic(app_id, "reflans", b_id));
    const auto c_topic = std::string(trystero::protocol::peer_topic(app_id, "reflans", c_id));
    std::string a_answer;
    int reflected = 0;
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
    const trystero::room b(f.io.get_executor(), f.net, config(b_id), "reflans");
    webcpp::test::require(
        BOOST_TEST(run_until(f.io, [&] { return joined.size() == 1 && !a_answer.empty(); })));
    const trystero::room peer_c(f.io.get_executor(), f.net, config(c_id), "reflans");
    BOOST_TEST(run_until(f.io, [&] { return joined.size() == 2; }));
    BOOST_TEST_GE(reflected, 1);
    BOOST_TEST(refused);
}

// A relay that closes each connection right after its subscription, as one does after a text
// longer than a room reads, is tried again within a bound that doubles every time, to 60 s; the
// bound comes back to 3.3 s only after a connection that stayed open for 60 s. The clock runs 100
// times faster.
void a_relay_that_closes_at_once_is_tried_less_and_less_often() {
    fixture f;
    f.net.time_scale = 100;
    auto& relay = f.net.relay(relay_a);
    relay.close_after_subscribe = true;
    auto c = config("AAAAAAAAAAAAAAAAAAAA");
    c.relay_urls = {relay_a};
    std::vector<long long> bounds;
    int reconnections = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        constexpr std::string_view within = ", next attempt within ";
        if (const auto at = text.find(within); at != std::string_view::npos) {
            bounds.push_back(std::stoll(std::string(text.substr(at + within.size()))));
        }
        reconnections += text.starts_with("reconnected to ") ? 1 : 0;
    };
    const trystero::room a(f.io.get_executor(), f.net, std::move(c), "backoff");
    BOOST_TEST(run_until(f.io, [&] { return bounds.size() >= 6; }));
    const std::vector<long long> growing{3333, 6666, 13332, 26664, 53328, 60000};
    BOOST_TEST_ALL_EQ(bounds.begin(),
                      bounds.begin() + std::min<std::ptrdiff_t>(6, std::ssize(bounds)),
                      growing.begin(), growing.end());

    // A connection that lasts: the 60 s announcement after it opened is on the relay, then the
    // relay goes away.
    relay.close_after_subscribe = false;
    const int before = reconnections;
    BOOST_TEST(run_until(f.io, [&] { return reconnections > before; }));
    const auto seen = relay.events_seen;
    // the warm-up's five announcements, then the first of the steady ones, 60 s later
    BOOST_TEST(run_until(f.io, [&] { return relay.events_seen >= seen + 6; }));
    const auto lost = bounds.size();
    relay.close_all();
    BOOST_TEST(run_until(f.io, [&] { return bounds.size() > lost; }));
    if (bounds.size() > lost) {
        BOOST_TEST_EQ(bounds[lost], 3333);
    }
}

// A flood of fresh ids below ours asks the room for a counter-announcement each: the room sends
// at most one per relay in 5.3 s, Trystero's announce interval, so that no relay rate-limits it.
void counter_announcements_are_paced() {
    constexpr std::size_t flood = 300;
    fixture f;
    auto c = config("zzzzzzzzzzzzzzzzzzzz");
    std::size_t asked = 0;
    c.log = [&](std::string_view /*category*/, std::string_view text) {
        asked += text.ends_with("asking them to offer") ? 1 : 0;
    };
    const trystero::room a(f.io.get_executor(), f.net, std::move(c), "counter");
    flooder attacker(f, "counter");
    const auto start = std::chrono::steady_clock::now();
    const auto before = f.net.relay(relay_a).events_seen;
    attacker.announce(flood, "0low");
    BOOST_TEST(run_until(f.io, [&] { return f.net.relay(relay_a).events_seen >= before + flood; }));
    f.io.poll();  // the room handles what the relay forwarded, posted behind every event it took
    const auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_TEST_GE(asked, 1U);
    BOOST_TEST_LE(asked, 1 + static_cast<std::size_t>(elapsed / 5333ms));
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
    signals_that_do_not_open_are_reported_once_per_interval();
    sealed_offers_that_fail_at_once_are_bounded();
    announcements_with_forged_answers_are_bounded();
    the_replay_cache_remembers_the_last_ones();
    a_replayed_answer_is_refused();
    a_replayed_offer_is_refused();
    a_reflected_offer_is_refused_as_an_answer();
    a_reflected_offer_is_refused_as_an_offer();
    a_reflected_answer_is_refused_as_an_answer();
    a_relay_that_closes_at_once_is_tried_less_and_less_often();
    counter_announcements_are_paced();
    three_peers_form_a_mesh();
    async_receive_works_in_a_coroutine();
    view_handlers_read_the_frame_without_copying();
    a_rate_limited_relay_does_not_stop_discovery();
    return boost::report_errors();
}
