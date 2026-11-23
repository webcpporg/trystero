// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// A difference from Trystero (doc: #trystero-differences-behaviour): the relay sockets of a page's
// rooms. Trystero JS opens one WebSocket per relay for the page, which every room shares; each
// room here opens its own. Trystero JS hands out the page's sockets (getRelaySockets); a room here
// hands out none, so the app of this program is given a backend that counts the WebSockets the
// app asks of the platform one. Its twin is test/oracle/twins/diff_relay_sockets.mjs. Both join
// over the relay whose URL is in the environment, TRYSTERO_RELAY_URL, which their lanes start on
// 127.0.0.1.

#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;

namespace {

// A backend that forwards to another, and counts the WebSockets asked of it.
class counting_backend final : public trystero::backend::factory {
public:
    explicit counting_backend(trystero::backend::factory& backend) : backend_(backend) {}

    // How many WebSockets the app asked for.
    [[nodiscard]] std::size_t websockets() const { return websockets_; }

    std::shared_ptr<trystero::backend::websocket> connect(
        std::string_view url, trystero::backend::websocket::handlers h,
        std::pmr::memory_resource* memory) override {
        ++websockets_;
        return backend_.connect(url, std::move(h), memory);
    }

    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
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
    std::size_t websockets_ = 0;
};

}  // namespace

int main() {
    const char* const relay = std::getenv("TRYSTERO_RELAY_URL");
    if (relay == nullptr) {
        std::cerr << "TRYSTERO_RELAY_URL names no relay\n";
        return 1;
    }
    asio::io_context io;
    trystero::platform::backend platform(io.get_executor());
    counting_backend backend(platform);
    // tag::example[]
    trystero::app app(io.get_executor(), backend,
                      {.app_id = "webcpp-trystero-differences", .relay_urls = {relay}});
    auto lobby = app.join("lobby");
    auto kitchen = app.join("kitchen");
    // Once both rooms have started, which they do on the executor.
    asio::post(io, [&] {
        std::cout << "the WebSockets the two rooms opened to their one relay: "
                  << backend.websockets() << '\n';
        lobby->async_leave([](boost::system::error_code) {});
        kitchen->async_leave([](boost::system::error_code) {});
    });
    // end::example[]
    if (!lobby || !kitchen) {
        std::cerr << "not joined\n";
        return 1;
    }
    io.run();  // lint-run: the rooms' relays, over the network
}
