// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)
//
// The native C++ side of the interop test: the browser peer's behaviour (peer.cpp), as a process.
// The driver (run.mjs) starts it with the room's parameters, reads one JSON event per line on
// stdout, and writes "leave" on stdin to make it leave, "replace" to move a room of another room
// id over the joined one, with a receive of the joined room's pending, or "abort" to make it die
// by a signal, as a crash does. Diagnostics go to stderr.
// Its peer connections are bound to 127.0.0.1, so that it meets the other peers of this machine on
// the loopback alone, with no network beyond it.
//
// | Argument      | Meaning                                                          |
// | --app, --room | the app id and room id                                           |
// | --password    | the room password, optional                                      |
// | --self        | this peer's id, optional                                         |
// | --relays      | comma-separated relay URLs, optional (Trystero's default list)   |
// | --ice         | a STUN or TURN server's URL, optional (Trystero's default list)  |
// | --room2       | a second room of the same app to join too, optional              |
#include <webcpp/trystero/native/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/json.hpp>

#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>

namespace trystero = webcpp::trystero;

namespace {

/** An event for the driver: a kind and fields, one JSON object per line. */
void record(std::string_view kind, boost::json::object fields = {}) {
    fields["kind"] = kind;
    // flushed: the driver reads each event from a pipe as it comes
    std::cout << boost::json::serialize(fields) << '\n' << std::flush;
}

std::uint32_t checksum(const trystero::bytes& data) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < data.size(); ++i) {
        sum += data[i] * static_cast<std::uint32_t>(i + 1);
    }
    return sum;
}

std::map<std::string, std::string> arguments(int argc, char* const* argv) {
    std::map<std::string, std::string> out;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view key = argv[i];
        if (key.starts_with("--")) {
            out[std::string(key.substr(2))] = argv[i + 1];
        }
    }
    return out;
}

/** The room's configuration, from the driver's arguments. */
trystero::room_config config_of(std::map<std::string, std::string>& args) {
    trystero::room_config config;
    config.app_id = args["app"];
    config.password = args["password"];
    config.self_id = args["self"];
    for (std::string_view relays = args["relays"]; !relays.empty();) {
        const auto comma = relays.find(',');
        config.relay_urls.emplace_back(relays.substr(0, comma));
        relays = comma == std::string_view::npos ? std::string_view() : relays.substr(comma + 1);
    }
    if (!args["ice"].empty()) {
        config.ice_servers.push_back(
            {.urls = {std::pmr::string(args["ice"])}, .username = {}, .credential = {}});
    }
    config.log = [](std::string_view category, std::string_view text) {
        std::cerr << category << ": " << text << '\n';
    };
    return config;
}

/** The native backend, which records each peer connection it makes, for the scenarios that count
 * them. */
class counting_backend : public trystero::native::backend {
public:
    using trystero::native::backend::backend;

    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
        auto pc = trystero::native::backend::make_peer_connection(servers, memory);
        if (pc) {
            record("made-connection");
        }
        return pc;
    }
};

trystero::room room_of(boost::asio::io_context& io, trystero::native::backend& backend,
                       std::map<std::string, std::string>& args, std::string_view suffix = {}) {
    return {io.get_executor(), backend, config_of(args), args["room"] + std::string(suffix)};
}

/**
 * A second room of the same app, with the configuration of the first but its room id: it sends each
 * peer that joins a chat message and a 100 KB file, and records what it sees with `room: second`.
 */
struct second_room {
    trystero::room room;
    trystero::action<std::string> chat = room.make_action<std::string>("chat");
    trystero::action<trystero::bytes> file = room.make_action<trystero::bytes>("file");

    second_room(boost::asio::io_context& io, trystero::native::backend& backend,
                std::map<std::string, std::string>& args)
        : room(io.get_executor(), backend, config_of(args), args["room2"]) {
        record("joined", {{"room", "second"}, {"self", room.self_id()}});
        room.on_peer_join([this](std::string_view peer_view) {
            const trystero::peer_id peer(peer_view);
            record("peer-join", {{"room", "second"}, {"peer", peer}});
            chat.async_send("hello from C++",
                            trystero::send_options{.targets = {peer}, .metadata = {}},
                            [](boost::system::error_code) {});
            trystero::bytes big(100'000);
            for (std::size_t i = 0; i < big.size(); ++i) {
                big[i] = static_cast<std::uint8_t>((i * 13) % 256);
            }
            file.async_send(std::move(big),
                            trystero::send_options{.targets = {peer}, .metadata = {}},
                            [](boost::system::error_code) {});
        });
        room.on_peer_leave([](std::string_view peer) {
            record("peer-leave", {{"room", "second"}, {"peer", peer}});
        });
        room.on_error([](std::string_view peer, boost::system::error_code ec,
                         std::string_view what) {
            record("error",
                   {{"room", "second"}, {"peer", peer}, {"error", ec.message()}, {"what", what}});
        });
        chat.on_message([](std::string text, const trystero::message_info& info) {
            record("chat", {{"room", "second"}, {"from", info.from}, {"text", text}});
        });
        file.on_message([](const trystero::bytes& data, const trystero::message_info& info) {
            record("file", {
                               {"room", "second"},
                               {"from", info.from},
                               {"size", data.size()},
                               {"checksum", checksum(data)},
                           });
        });
    }
};

/**
 * Moves `next` over `joined`, with a receive pending on the joined room: the receive, on an action
 * no peer sends, completes when the joined room is left, while the action keeps its state alive.
 */
void replace(trystero::room& joined, trystero::room next) {
    const auto pending =
        std::make_shared<trystero::action<std::string>>(joined.make_action<std::string>("pending"));
    pending->async_receive([pending](boost::system::error_code ec, const std::string& /*text*/,
                                     const trystero::message_info& /*info*/) {
        record("pending", {{"error", ec ? ec.message() : ""}});
    });
    joined = std::move(next);
    record("replaced", {{"self", joined.self_id()}});
}

/** Leaves with Trystero's farewell, then stops `io`, which ends the process. */
void leave(trystero::room& room, boost::asio::io_context& io) {
    room.async_leave([&io](boost::system::error_code ec) {
        record("left", {{"error", ec ? ec.message() : ""}});
        io.stop();
    });
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = arguments(argc, argv);
    boost::asio::io_context io;
    counting_backend backend(io.get_executor(), {.bind_address = "127.0.0.1"});
    trystero::room room = room_of(io, backend, args);
    boost::json::array relays;
    for (const auto& url : room.relay_urls()) {
        relays.emplace_back(url);
    }
    record("joined", {{"self", room.self_id()}, {"relays", relays}});

    auto chat = room.make_action<std::string>("chat");
    auto drink = room.make_action<boost::json::value>("drink");
    auto file = room.make_action<trystero::bytes>("file");

    room.on_peer_join([&](std::string_view peer_view) {
        const trystero::peer_id peer(peer_view);
        record("peer-join", {{"peer", peer}});
        // Each handler holds a copy of its own, not const, so that moving the handler moves it.
        const auto done = [peer](const char* what) {
            return [peer = trystero::peer_id(peer), what](boost::system::error_code ec) {
                record("sent", {{"what", what}, {"peer", peer}, {"error", ec ? ec.message() : ""}});
            };
        };
        chat.async_send("hello from C++", trystero::send_options{.targets = {peer}, .metadata = {}},
                        done("chat"));
        drink.async_send(
            boost::json::object{{"drink", "cafezinho"}, {"withIce", false}, {"from", "c++"}},
            trystero::send_options{.targets = {peer}, .metadata = {}}, done("drink"));
        trystero::bytes big(100'000);
        for (std::size_t i = 0; i < big.size(); ++i) {
            big[i] = static_cast<std::uint8_t>((i * 13) % 256);
        }
        file.async_send(
            std::move(big),
            trystero::send_options{.targets = {peer}, .metadata = R"({"name":"from-cpp.bin"})"},
            done("file"));
        room.async_ping(peer, [peer = trystero::peer_id(peer)](boost::system::error_code ec,
                                                               std::chrono::milliseconds rtt) {
            record("ping",
                   {{"peer", peer}, {"error", ec ? ec.message() : ""}, {"ms", rtt.count()}});
        });
    });
    room.on_peer_leave([](std::string_view peer) { record("peer-leave", {{"peer", peer}}); });
    room.on_error([](std::string_view peer, boost::system::error_code ec, std::string_view what) {
        record("error", {{"peer", peer}, {"error", ec.message()}, {"what", what}});
    });
    chat.on_message([&](std::string text, const trystero::message_info& info) {
        record("chat", {{"from", info.from}, {"text", text}});
        if (!text.starts_with("echo: ")) {
            chat.async_send("echo: " + text,
                            trystero::send_options{.targets = {info.from}, .metadata = {}},
                            [](boost::system::error_code) {});
        }
    });
    drink.on_message([](boost::json::value v, const trystero::message_info& info) {
        record("drink", {{"from", info.from}, {"value", v}});
    });
    file.on_message([](const trystero::bytes& data, const trystero::message_info& info) {
        record("file", {
                           {"from", info.from},
                           {"size", data.size()},
                           {"checksum", checksum(data)},
                           {"metadata", info.metadata},
                       });
    });

    const std::unique_ptr<second_room> second =
        args["room2"].empty() ? nullptr : std::make_unique<second_room>(io, backend, args);

    // "leave" on stdin: Trystero's farewell, then the process ends; "replace": a room moved over;
    // "abort": SIGABRT, for the scenario that checks the driver names a peer killed by a signal.
    boost::asio::posix::stream_descriptor input(io, ::dup(STDIN_FILENO));
    boost::asio::streambuf line;
    std::function<void()> read_command = [&] {
        boost::asio::async_read_until(
            input, line, '\n', [&](boost::system::error_code ec, std::size_t size) {
                if (ec) {
                    return;
                }
                const std::string command(static_cast<const char*>(line.data().data()), size);
                line.consume(size);
                if (command.starts_with("abort")) {
                    std::abort();
                }
                if (command.starts_with("replace")) {
                    replace(room, room_of(io, backend, args, "-replaced"));
                }
                if (!command.starts_with("leave")) {
                    read_command();
                    return;
                }
                leave(room, io);
            });
    };
    read_command();
    io.run();  // lint-run: a process whose purpose is network I/O, until "leave" stops it
}
