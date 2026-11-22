// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences-behaviour): where a room's relays and ICE
// servers come from. Trystero JS opens the relays of the first room a page joins, and every room
// joined while one remains uses them, whatever relays it was given; and it makes a connection with
// the RTC configuration of the room that negotiates it, which the page's other rooms then share. A
// room here uses its app's relays and ICE servers, so a room joined with another configuration is
// a room of another app. Two peers of one program meet in a lobby with a first configuration, and
// in a kitchen with a second. Trystero JS hands out the page's relay sockets and a room's
// connection; a room here hands out neither, so each app of this program is given a backend that
// records what the app asks of the platform one. Its twin is
// test/oracle/twins/diff_relay_config.mjs. Both meet over the relay whose URL is in the
// environment, TRYSTERO_RELAY_URL, which their lanes start on 127.0.0.1.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;

namespace {

// The relays and the ICE servers an app is configured with.
struct configuration {
    std::string relay;  // the one relay's URL
    std::string stun;   // the one STUN server's URL
};

// A backend that forwards to another, and records the relays and ICE servers asked of it.
class observing_backend final : public trystero::backend::factory {
public:
    explicit observing_backend(trystero::backend::factory& backend) : backend_(backend) {}

    // The URLs of the relays it was asked to connect to.
    [[nodiscard]] const std::set<std::string>& relays() const { return relays_; }

    // The URLs of the ICE servers of the peer connections it was asked to make.
    [[nodiscard]] const std::set<std::string>& ice_servers() const { return ice_servers_; }

    std::shared_ptr<trystero::backend::websocket> connect(
        std::string_view url, trystero::backend::websocket::handlers h,
        std::pmr::memory_resource* memory) override {
        relays_.emplace(url);
        return backend_.connect(url, std::move(h), memory);
    }

    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
        for (const auto& server : servers) {
            ice_servers_.insert(server.urls.begin(), server.urls.end());
        }
        return backend_.make_peer_connection(servers, memory);
    }

    std::shared_ptr<trystero::backend::timer> make_timer(
        std::pmr::memory_resource* memory) override {
        return backend_.make_timer(memory);
    }

    void derive_password_master(std::span<const std::uint8_t> password,
                                std::span<const std::uint8_t, 32> salt,
                                std::span<std::uint8_t, 32> out,
                                trystero::handler<void(boost::system::error_code)> done) override {
        backend_.derive_password_master(password, salt, out, std::move(done));
    }

    void random(std::span<std::uint8_t> out) override { backend_.random(out); }

    std::chrono::sys_seconds unix_now() override { return backend_.unix_now(); }

    std::chrono::steady_clock::time_point steady_now() override { return backend_.steady_now(); }

private:
    trystero::backend::factory& backend_;
    std::set<std::string> relays_;
    std::set<std::string> ice_servers_;
};

}  // namespace

int main() {
    const char* const relay = std::getenv("TRYSTERO_RELAY_URL");
    if (relay == nullptr) {
        std::cerr << "TRYSTERO_RELAY_URL names no relay\n";
        return 1;
    }
    asio::io_context io;
    // The peers meet on this machine alone: every connection bound to the loopback, and STUN
    // servers where nothing answers in place of Trystero's public ones.
    trystero::platform::backend backend(io.get_executor(), {.bind_address = "127.0.0.1"});
    // Two configurations. The second relay is the same local relay under a path, which it accepts
    // as it accepts any.
    const configuration first{.relay = relay, .stun = "stun:127.0.0.1:9"};
    const configuration second{
        .relay = std::string(relay) + "/second",
        .stun = "stun:127.0.0.1:19",
    };
    const auto config = [](const configuration& c) {
        trystero::app_config out;
        out.app_id = "webcpp-trystero-differences";
        out.relay_urls = {std::pmr::string(c.relay)};
        out.ice_servers = {{.urls = {std::pmr::string(c.stun)}, .username = {}, .credential = {}}};
        return out;
    };
    // The other peer, as another page would be: the lobby with the first configuration, the
    // kitchen with the second.
    trystero::app other_first(io.get_executor(), backend, config(first));
    trystero::app other_second(io.get_executor(), backend, config(second));
    auto other_lobby = other_first.join("lobby");
    auto other_kitchen = other_second.join("kitchen");
    // Each app of this program is given a backend that records what the app asks of the platform
    // one.
    observing_backend first_backend(backend);
    observing_backend second_backend(backend);
    // The configuration whose URL, and only it, a backend was asked for, of the two URLs given.
    const auto which = [](const std::set<std::string>& urls, const std::string& first_url,
                          const std::string& second_url) {
        if (urls == std::set<std::string>{first_url}) {
            return "the first configuration's";
        }
        return urls == std::set<std::string>{second_url} ? "the second configuration's" : "another";
    };
    const auto relays_of = [&](const observing_backend& b) {
        return which(b.relays(), first.relay, second.relay);
    };
    const auto ice_servers_of = [&](const observing_backend& b) {
        return which(b.ice_servers(), first.stun, second.stun);
    };
    // tag::example[]
    // A configuration is an app's: the kitchen, joined with the second, is a room of a second app.
    trystero::app lobby_app(io.get_executor(), first_backend, config(first));
    trystero::app kitchen_app(io.get_executor(), second_backend, config(second));
    auto lobby = lobby_app.join("lobby");
    auto kitchen = kitchen_app.join("kitchen");
    // What each app's backend was asked for, once both rooms have met the other peer.
    const auto report = [&] {
        std::cout << "the lobby's relays: " << relays_of(first_backend) << '\n'
                  << "the kitchen's relays: " << relays_of(second_backend) << '\n'
                  << "the lobby's connection: " << ice_servers_of(first_backend) << " ICE servers\n"
                  << "the kitchen's connection: " << ice_servers_of(second_backend)
                  << " ICE servers\n";
    };
    // end::example[]
    if (!other_lobby || !other_kitchen || !lobby || !kitchen) {
        std::cerr << "not joined\n";
        return 1;
    }
    // Once each of this program's rooms has met the other peer, it reports, and every room leaves.
    bool reported = false;
    const auto met = [&](const trystero::peer_event&) {
        if (reported || lobby->peers().empty() || kitchen->peers().empty()) {
            return;
        }
        reported = true;
        report();
        for (trystero::room* room : {&*lobby, &*kitchen, &*other_lobby, &*other_kitchen}) {
            room->async_leave([](boost::system::error_code) {});
        }
    };
    lobby->on_peer_join(met);
    kitchen->on_peer_join(met);
    io.run();  // lint-run: the program's network I/O; libdatachannel's threads post to it
}
