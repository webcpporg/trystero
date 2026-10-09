// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests whole rooms over the native backend: real WebRTC through libdatachannel, AES-GCM through
// OpenSSL, Asio's timers. Only the relays are the memory ones of support/memory_backend.hpp, and
// the backend binds every connection to 127.0.0.1, so the loopback is the only network needed.

#include <webcpp/trystero/native/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/memory_backend.hpp"

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;
using namespace std::chrono_literals;

namespace {

constexpr const char* relay_url = "wss://relay.test";

/** The native backend bound to the loopback, with the memory relays instead of the network's. */
class native_over_memory_relays : public trystero::native::backend {
public:
    native_over_memory_relays(asio::io_context& io, trystero::test::memory_network& relays)
        : trystero::native::backend(io.get_executor(), {.bind_address = "127.0.0.1"}),
          relays_(relays) {}

    std::shared_ptr<trystero::backend::websocket> connect(
        std::string_view url, trystero::backend::websocket::handlers h,
        std::pmr::memory_resource* memory) override {
        return relays_.connect(url, std::move(h), memory);
    }

private:
    trystero::test::memory_network& relays_;
};

struct fixture {
    asio::io_context io;
    trystero::test::memory_network relays{io};
    native_over_memory_relays backend{io, relays};

    fixture() { relays.relay(relay_url); }
};

trystero::room_config config(std::string_view self_id, std::string_view password = {}) {
    trystero::room_config c;
    c.app_id = "webcpp-trystero-native-tests";
    c.password = password;
    c.relay_urls = {relay_url};
    // Nothing answers there: with every connection bound to 127.0.0.1, the loopback's host
    // candidate is the only one, and it is enough.
    c.ice_servers = {{.urls = {"stun:127.0.0.1:9"}, .username = {}, .credential = {}}};
    c.self_id = self_id;
    return c;
}

bool run_until(asio::io_context& io, const std::function<bool()>& done,
               std::chrono::milliseconds limit = 15s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < end) {
        io.run_for(10ms);  // lint-run: libdatachannel's threads post the connections' events to it
        if (io.stopped()) {
            io.restart();
        }
    }
    return done();
}

void native_rooms_join_and_exchange_every_format() {
    fixture f;
    trystero::room a(f.io.get_executor(), f.backend, config("AAAAAAAAAAAAAAAAAAAA", "s3cret"),
                     "lobby");
    trystero::room z(f.io.get_executor(), f.backend, config("zzzzzzzzzzzzzzzzzzzz", "s3cret"),
                     "lobby");
    std::string text;
    boost::json::value json;
    trystero::bytes file;
    std::string metadata;
    z.make_action<std::string>("chat").on_message(
        [&](std::string v, const trystero::message_info&) { text = std::move(v); });
    z.make_action<boost::json::value>("drink").on_message(
        [&](boost::json::value v, const trystero::message_info&) { json = std::move(v); });
    z.make_action<trystero::bytes>("file").on_message(
        [&](trystero::bytes v, const trystero::message_info& info) {
            file = std::move(v);
            metadata = info.metadata;
        });
    if (!BOOST_TEST(
            run_until(f.io, [&] { return a.peers().size() == 1 && z.peers().size() == 1; }))) {
        return;
    }

    trystero::bytes big(100'000);
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<std::uint8_t>(i * 31);
    }
    a.make_action<std::string>("chat").async_send(
        "hello over libdatachannel", [](boost::system::error_code ec) { BOOST_TEST(!ec); });
    a.make_action<boost::json::value>("drink").async_send(
        boost::json::object{{"drink", "negroni"}},
        [](boost::system::error_code ec) { BOOST_TEST(!ec); });
    a.make_action<trystero::bytes>("file").async_send(
        big, trystero::send_options{.targets = {}, .metadata = R"({"name":"big.bin"})"},
        [](boost::system::error_code ec) { BOOST_TEST(!ec); });
    if (!BOOST_TEST(
            run_until(f.io, [&] { return !text.empty() && !json.is_null() && !file.empty(); }))) {
        return;
    }
    BOOST_TEST_EQ(text, "hello over libdatachannel");
    BOOST_TEST_EQ(json.at("drink").as_string(), "negroni");
    BOOST_TEST_ALL_EQ(file.begin(), file.end(), big.begin(), big.end());
    BOOST_TEST_EQ(metadata, R"({"name":"big.bin"})");

    std::optional<boost::system::error_code> pinged;
    a.async_ping(z.self_id(),
                 [&](boost::system::error_code ec, std::chrono::milliseconds) { pinged = ec; });
    if (!BOOST_TEST(run_until(f.io, [&] { return pinged.has_value(); }))) {
        return;
    }
    BOOST_TEST(!*pinged);
}

void another_password_never_joins() {
    fixture f;
    const trystero::room a(f.io.get_executor(), f.backend, config("BBBBBBBBBBBBBBBBBBBB", "s3cret"),
                           "private");
    trystero::room b(f.io.get_executor(), f.backend, config("CCCCCCCCCCCCCCCCCCCC", "guess"),
                     "private");
    std::vector<boost::system::error_code> errors;
    b.on_error([&](std::string_view, boost::system::error_code ec, std::string_view) {
        errors.push_back(ec);
    });
    run_until(f.io, [] { return false; }, 2s);
    BOOST_TEST(a.peers().empty());
    BOOST_TEST(b.peers().empty());
    BOOST_TEST(!errors.empty());
}

void leaving_is_seen_over_a_real_connection() {
    fixture f;
    trystero::room a(f.io.get_executor(), f.backend, config("DDDDDDDDDDDDDDDDDDDD"), "leave");
    std::vector<std::string> left;
    a.on_peer_leave([&](std::string_view p) { left.emplace_back(p); });
    trystero::room b(f.io.get_executor(), f.backend, config("EEEEEEEEEEEEEEEEEEEE"), "leave");
    if (!BOOST_TEST(run_until(f.io, [&] { return a.peers().size() == 1; }))) {
        return;
    }
    bool done = false;
    b.async_leave([&](boost::system::error_code ec) { done = !ec; });
    BOOST_TEST(run_until(f.io, [&] { return done && left.size() == 1; }));
}

}  // namespace

int main() {
    native_rooms_join_and_exchange_every_format();
    another_password_never_joins();
    leaving_is_seen_over_a_real_connection();
    return boost::report_errors();
}
