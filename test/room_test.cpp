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
#include <boost/json/serialize.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    three_peers_form_a_mesh();
    async_receive_works_in_a_coroutine();
    view_handlers_read_the_frame_without_copying();
    a_rate_limited_relay_does_not_stop_discovery();
    return boost::report_errors();
}
