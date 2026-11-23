// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)
//
// The native C++ side of the interop test: the peer of peer_room.hpp as a process. The driver
// (run.mjs) starts it with the parameters as arguments (`--room <id>` and so on), reads one JSON
// event per line on stdout, and writes commands on stdin: "leave" to leave the room, after which
// the process ends; "offline" and "online" to set the app's relay connectivity; "abort" to make it
// die by a signal, as a crash does. The app's log goes to stderr. Its peer connections are bound
// to 127.0.0.1, so that it meets the other peers of this machine on the loopback alone, with no
// network beyond it, and each one the backend makes is the event `made-connection`, as cpp.html
// counts the page's.
#include "peer_room.hpp"

#include <webcpp/trystero/platform/backend.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>

#include <unistd.h>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

namespace trystero = webcpp::trystero;

namespace {

std::map<std::string, std::string, std::less<>> arguments(int argc, char* const* argv) {
    std::map<std::string, std::string, std::less<>> out;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view key = argv[i];
        if (key.starts_with("--")) {
            out[std::string(key.substr(2))] = argv[i + 1];
        }
    }
    return out;
}

/** Writes one event, a line of JSON, on stdout. */
void emit(std::string_view event) {
    std::cout << event << '\n';
    std::cout.flush();
}

/** The platform backend, whose every connection made is the event `made-connection`. */
class counting_backend : public trystero::platform::backend {
public:
    using trystero::platform::backend::backend;

    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
        auto pc = trystero::platform::backend::make_peer_connection(servers, memory);
        if (pc) {
            emit(R"({"kind":"made-connection"})");
        }
        return pc;
    }
};

/** Reads the driver's commands while the room runs. */
struct command_reader {
    boost::asio::io_context& io;
    interop::peer& peer;
    boost::asio::posix::stream_descriptor input;
    boost::asio::streambuf line;

    command_reader(boost::asio::io_context& context, interop::peer& p)
        : io(context), peer(p), input(context, ::dup(STDIN_FILENO)) {}

    /** Whether another command should be read after this one. */
    bool handle(std::string_view command) {
        if (command.starts_with("abort")) {
            std::abort();
        }
        if (command.starts_with("offline")) {
            peer.set_connectivity(trystero::relay_connectivity::offline);
        } else if (command.starts_with("online")) {
            peer.set_connectivity(trystero::relay_connectivity::online);
        } else if (command.starts_with("leave")) {
            peer.leave([this] { io.stop(); });
            return false;
        }
        return true;
    }

    /**
     * Reads the next command.
     *
     * Tip: the reads are a loop of asynchronous operations, not a recursion: each completion runs
     * once the call that started its read has returned. The next read is started through a
     * pointer, so that the analysis of calls (misc-no-recursion) sees no cycle through Asio.
     */
    void read() {
        boost::asio::async_read_until(
            input, line, '\n',
            [this, again = &command_reader::read](boost::system::error_code ec, std::size_t size) {
                if (ec) {
                    return;
                }
                const std::string command(static_cast<const char*>(line.data().data()), size);
                line.consume(size);
                if (handle(command)) {
                    (this->*again)();
                }
            });
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    const auto args = arguments(argc, argv);
    boost::asio::io_context io;
    counting_backend backend(io.get_executor(), {.bind_address = "127.0.0.1"});
    interop::peer peer(
        io.get_executor(), backend,
        [&args](std::string_view name) {
            const auto found = args.find(name);
            return found == args.end() ? std::string() : found->second;
        },
        emit,
        [](std::string_view category, std::string_view text) {
            std::cerr << category << ": " << text << '\n';
        });
    command_reader commands(io, peer);
    commands.read();
    io.run();  // lint-run: a process whose purpose is network I/O, until "leave" stops it
}
