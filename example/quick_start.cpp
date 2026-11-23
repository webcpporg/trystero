// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The quick start of the documentation (doc: #trystero-quick-start), which includes the tagged
// regions below: two apps of one program, the peers alice and bob, join the room "lobby" of the
// app webcpp-trystero-quick-start; alice greets bob when he joins, bob answers from a coroutine,
// each prints what it received after the other's name, and both leave, which ends the program. Its
// twin is test/oracle/twins/quick_start.mjs. Both meet over the relay whose URL is in the
// environment, TRYSTERO_RELAY_URL, which their lanes start on 127.0.0.1; without it, the program
// meets over Trystero's public relays, as a user's program does.

// tag::program[]
// tag::includes[]
#include <webcpp/trystero.hpp>
#include <webcpp/trystero/platform/backend.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;
// end::includes[]

namespace {

// tag::config[]
/** What one peer's app is made with: the relay `relay` names, or the public ones. Each app makes
 * its own endpoint key, whose public key derives its id. */
trystero::app_config config_for(const char* relay) {
    trystero::app_config config;
    config.app_id = "webcpp-trystero-quick-start";
    if (relay != nullptr) {
        // The peers stay on this machine: the relay given, and a STUN server where nothing answers
        // in place of Trystero's public ones, beside a backend bound to the loopback.
        config.relay_urls = {relay};
        config.ice_servers = {{.urls = {"stun:127.0.0.1:9"}, .username = {}, .credential = {}}};
    }
    // Without a relay, both lists stay empty: Trystero's defaults, as a user's program has them,
    // the public relays its app id selects and Trystero's public STUN servers.
    return config;
}

// end::config[]

// tag::answer[]
/** bob's part: waits for a message, prints it after alice's name, answers its sender, and leaves
 * the room. */
asio::awaitable<void> answer(trystero::room& lobby, trystero::action<std::string>& chat) {
    // as_tuple hands the error over as a value: a room left first ends the wait without throwing.
    auto [ec, text, info] = co_await chat.async_receive(asio::as_tuple(asio::use_awaitable));
    if (ec) {
        co_return;
    }
    // The sender's id derives from a key made on each run: its name prints the same every time.
    std::cout << "alice: " << text << '\n';
    auto [failed] = co_await chat.async_send("hello back", {.targets = {info.from}, .metadata = {}},
                                             asio::as_tuple(asio::use_awaitable));
    if (failed) {
        std::cerr << "not delivered: " << failed.message() << '\n';
    }
    co_await lobby.async_leave(asio::as_tuple(asio::use_awaitable));
}

// end::answer[]

}  // namespace

// tag::main[]
int main() {
    // The relay the lane starts on 127.0.0.1, or none: Trystero's public relays.
    const char* const relay = std::getenv("TRYSTERO_RELAY_URL");
    asio::io_context io;
    // One backend for both apps, whose connections stay on the loopback beside a local relay.
    trystero::platform::backend_config local;
    if (relay != nullptr) {
        local.bind_address = "127.0.0.1";
    }
    trystero::platform::backend backend(io.get_executor(), local);

    // Two apps, two peers, each with an id of its own, in the same room.
    trystero::app alice(io.get_executor(), backend, config_for(relay));
    trystero::app bob(io.get_executor(), backend, config_for(relay));
    auto alice_lobby = alice.join("lobby");
    auto bob_lobby = bob.join("lobby");
    if (!alice_lobby || !bob_lobby) {
        std::cerr << "not joined\n";
        return 1;
    }
    auto alice_chat = alice_lobby->make_action<std::string>("chat");
    auto bob_chat = bob_lobby->make_action<std::string>("chat");

    // alice greets each peer once its handshake completes, prints the answer, and leaves.
    alice_lobby->on_peer_join([&](const trystero::peer_event& joined) {
        alice_chat.async_send("hello",
                              {.targets = {trystero::peer_id(joined.peer_id)}, .metadata = {}},
                              [](boost::system::error_code ec) {
                                  if (ec) {
                                      std::cerr << "not delivered: " << ec.message() << '\n';
                                  }
                              });
    });
    alice_chat.on_message([&](const std::string& text, const trystero::message_info& /*info*/) {
        std::cout << "bob: " << text << '\n';
        alice_lobby->async_leave([](boost::system::error_code) {});
    });
    asio::co_spawn(io, answer(*bob_lobby, bob_chat), asio::detached);

    io.run();  // lint-run: the program's network I/O; libdatachannel's threads post to it
}

// end::main[]
// end::program[]
