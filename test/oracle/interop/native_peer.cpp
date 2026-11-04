// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)
//
// The native C++ side of the interop test: the browser peer's behaviour (peer.cpp), as a process.
// The driver (run.mjs) starts it with the room's parameters, reads one JSON event per line on
// stdout, and writes "leave" on stdin to make it leave, "replace" to move a room of another room
// id over the joined one with a receive pending, "leave-room" to leave the first room and go on,
// or "abort" to make it die by a signal, as a crash does. Diagnostics go to
// stderr. Its peer connections are bound to 127.0.0.1, so that it meets the other peers of this
// machine on the loopback alone, with no network beyond it.
//
// | Argument        | Meaning                                                            |
// | --app, --room   | the app id and room id                                             |
// | --password      | the app password, shared by all its rooms, optional                |
// | --self          | this peer's id, optional                                           |
// | --relays        | comma-separated relay URLs, optional (Trystero's default list)     |
// | --ice           | a STUN or TURN server's URL, optional (Trystero's default list)    |
// | --room2         | a second room of the same app id to join too, optional             |
// | --room3         | a third room of the same app, optional                             |
// | --room2Delay    | the ms before joining room2, optional (0)                          |
// | --room3Delay    | the ms before joining room3, optional (0)                          |
// | --idle          | the app's idle_timeout in ms, optional (123,333)                   |
// | --handshake     | every room's handshake_timeout in ms, optional (10,000)            |
// | --delayOpen     | defer native open delivery while transport messages arrive, optional |
// | --ping2         | any value: ping each peer of room2 every second, optional          |
#include <webcpp/trystero/app.hpp>
#include <webcpp/trystero/native/backend.hpp>
#include <webcpp/trystero/room.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/json.hpp>

#include <unistd.h>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace trystero = webcpp::trystero;

namespace {

/** An event for the driver: a kind and fields, one JSON object per line. */
void record(std::string_view kind, boost::json::object fields = {}) {
    fields["kind"] = kind;
    // flushed: the driver reads each event from a pipe as it comes
    std::cout << boost::json::serialize(fields) << '\n' << std::flush;
}

/** Milliseconds from a parameter's digits, or `fallback` when it has none. */
std::chrono::milliseconds millis(std::string_view text, std::chrono::milliseconds fallback) {
    std::int64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [at, ec] = std::from_chars(begin, end, value);
    return text.empty() || ec != std::errc{} || at != end || value < 0
               ? fallback
               : std::chrono::milliseconds(value);
}

/** The events the driver counts connections by, from the app's log lines
 * `connection with <peer> adopted` and `connection with <peer> ended: <reason>`. */
void record_connection(std::string_view category, std::string_view text) {
    constexpr std::string_view prefix = "connection with ";
    constexpr std::string_view adopted = " adopted";
    constexpr std::string_view ended = " ended: ";
    if (category != "webrtc" || !text.starts_with(prefix)) {
        return;
    }
    const auto rest = text.substr(prefix.size());
    if (rest.ends_with(adopted)) {
        record("adopted", {{"peer", rest.substr(0, rest.size() - adopted.size())}});
    } else if (const auto at = rest.find(ended); at != std::string_view::npos) {
        record("ended", {{"peer", rest.substr(0, at)}, {"reason", rest.substr(at + ended.size())}});
    }
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

/** The app's configuration, from the driver's arguments. */
trystero::app_config app_config_of(std::map<std::string, std::string>& args) {
    trystero::app_config config;
    config.app_id = args["app"];
    config.self_id = args["self"];
    config.password = args["password"];
    for (std::string_view relays = args["relays"]; !relays.empty();) {
        const auto comma = relays.find(',');
        config.relay_urls.emplace_back(relays.substr(0, comma));
        relays = comma == std::string_view::npos ? std::string_view() : relays.substr(comma + 1);
    }
    if (!args["ice"].empty()) {
        config.ice_servers.push_back(
            {.urls = {std::pmr::string(args["ice"])}, .username = {}, .credential = {}});
    }
    config.idle_timeout = millis(args["idle"], std::chrono::milliseconds(123'333));
    config.log = [](std::string_view category, std::string_view text) {
        record_connection(category, text);
        std::cerr << category << ": " << text << '\n';
    };
    return config;
}

/** A room's options: the driver's handshake timeout. */
trystero::room_options options_of(std::map<std::string, std::string>& args) {
    return {
        .handshake_timeout = millis(args["handshake"], std::chrono::milliseconds(10'000)),
    };
}

/** Joins `room_id` through `app`; when the app refuses, the process ends after an event. */
trystero::room join(trystero::app& app, const std::string& room_id,
                    trystero::room_options options) {
    auto joined = app.join(room_id, options);
    if (!joined) {
        record("not-joined", {{"room", room_id}, {"error", joined.error().message()}});
        std::exit(1);
    }
    return std::move(*joined);
}

// A controlled native handoff: the transport is open, but signaling receives its open event
// 100 ms later. Real messages still use the native wrapper, including its pre-handoff queue.
class delayed_channel : public trystero::backend::data_channel,
                        public std::enable_shared_from_this<delayed_channel> {
public:
    delayed_channel(boost::asio::any_io_executor ex,
                    std::shared_ptr<trystero::backend::data_channel> channel)
        : timer_(std::move(ex)), channel_(std::move(channel)) {}

    void set_handlers(handlers h) override {
        timer_.cancel();
        opening_ =
            h.open ? std::make_shared<trystero::handler<void()>>(std::move(h.open)) : nullptr;
        if (opening_) {
            const auto weak = weak_from_this();
            h.open = [weak] {
                if (const auto self = weak.lock()) {
                    record("delayed-open");
                    self->timer_.expires_after(std::chrono::milliseconds(100));
                    self->timer_.async_wait([weak](boost::system::error_code ec) {
                        if (const auto channel = weak.lock(); channel && !ec && channel->opening_) {
                            channel->released_ = true;
                            const auto callback = channel->opening_;
                            (*callback)();
                        }
                    });
                }
            };
        }
        channel_->set_handlers(std::move(h));
    }

    bool is_open() const override { return released_ && channel_->is_open(); }

    void send(std::span<const std::uint8_t> frame) override { channel_->send(frame); }

    std::size_t buffered_amount() const override { return channel_->buffered_amount(); }

    void close() override {
        timer_.cancel();
        channel_->close();
    }

private:
    boost::asio::steady_timer timer_;
    std::shared_ptr<trystero::backend::data_channel> channel_;
    std::shared_ptr<trystero::handler<void()>> opening_;
    bool released_ = false;
};

// Keeps the fixture's channel observer at both ends without changing SDP or ICE handling.
class observed_connection : public trystero::backend::peer_connection {
    using channel = std::shared_ptr<trystero::backend::data_channel>;
    using observer = std::function<channel(channel)>;

public:
    observed_connection(std::shared_ptr<trystero::backend::peer_connection> pc, observer observe)
        : pc_(std::move(pc)), observe_(std::move(observe)) {}

    void set_handlers(handlers h) override {
        if (h.data_channel) {
            h.data_channel = [observe = observe_, deliver = std::move(h.data_channel)](
                                 channel c) mutable { deliver(observe(std::move(c))); };
        }
        pc_->set_handlers(std::move(h));
    }

    channel create_data_channel(std::string_view label) override {
        auto c = pc_->create_data_channel(label);
        return c ? observe_(std::move(c)) : nullptr;
    }

    void set_local_description(
        trystero::handler<void(boost::system::error_code, std::string_view, std::string_view)> done)
        override {
        pc_->set_local_description(std::move(done));
    }

    void set_remote_description(std::string_view type, std::string_view sdp,
                                trystero::handler<void(boost::system::error_code)> done) override {
        pc_->set_remote_description(type, sdp, std::move(done));
    }

    void add_ice_candidate(std::string_view candidate,
                           trystero::handler<void(boost::system::error_code)> done) override {
        pc_->add_ice_candidate(candidate, std::move(done));
    }

    bool has_remote_description() const override { return pc_->has_remote_description(); }

    void close() override { pc_->close(); }

private:
    std::shared_ptr<trystero::backend::peer_connection> pc_;
    observer observe_;
};

/** The native backend, which records each peer connection it makes, for the scenarios that count
 * them. */
class counting_backend : public trystero::native::backend {
public:
    explicit counting_backend(boost::asio::any_io_executor ex,
                              trystero::native::backend_config config)
        : trystero::native::backend(ex, std::move(config)), ex_(std::move(ex)) {}

    std::shared_ptr<trystero::backend::peer_connection> make_peer_connection(
        std::span<const trystero::backend::ice_server> servers,
        std::pmr::memory_resource* memory) override {
        auto pc = trystero::native::backend::make_peer_connection(servers, memory);
        if (pc) {
            record("made-connection");
            if (delay_open) {
                return std::make_shared<observed_connection>(pc, [this](auto channel) {
                    return std::make_shared<delayed_channel>(ex_, std::move(channel));
                });
            }
        }
        return pc;
    }

    bool delay_open = false;

private:
    boost::asio::any_io_executor ex_;
};

/**
 * A further room of the app: it sends each peer that joins a chat message and a 100 KB file,
 * echoes each chat message, and records what it sees with `room: <which>`.
 */
struct extra_room {
    std::string which;
    trystero::room room;
    trystero::action<std::string> chat = room.make_action<std::string>("chat");
    trystero::action<trystero::bytes> file = room.make_action<trystero::bytes>("file");

    extra_room(trystero::app& app, std::string which_room, std::string_view room_id,
               trystero::room_options options)
        : which(std::move(which_room)), room(join(app, std::string(room_id), options)) {
        record("joined", {{"room", which}, {"self", room.self_id()}});
        room.on_peer_join([this](std::string_view peer_view) {
            const trystero::peer_id peer(peer_view);
            record("peer-join", {{"room", which}, {"peer", peer}});
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
        room.on_peer_leave([this](std::string_view peer) {
            record("peer-leave", {{"room", which}, {"peer", peer}});
        });
        room.on_error(
            [this](std::string_view peer, boost::system::error_code ec, std::string_view what) {
                record("error",
                       {{"room", which}, {"peer", peer}, {"error", ec.message()}, {"what", what}});
            });
        chat.on_message([this](std::string text, const trystero::message_info& info) {
            record("chat", {{"room", which}, {"from", info.from}, {"text", text}});
            if (!text.starts_with("echo: ")) {
                chat.async_send("echo: " + text,
                                trystero::send_options{.targets = {info.from}, .metadata = {}},
                                [](boost::system::error_code) {});
            }
        });
        file.on_message([this](const trystero::bytes& data, const trystero::message_info& info) {
            record("file", {
                               {"room", which},
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

/** Pings every joined peer of room2 once a second, with an id for each start and completion. */
struct room_pinger {
    boost::asio::steady_timer timer;
    const std::unique_ptr<extra_room>& second;
    std::uint64_t next_ping = 0;

    room_pinger(boost::asio::io_context& io, const std::unique_ptr<extra_room>& room)
        : timer(io), second(room) {}

    void ping() {
        if (second) {
            for (const auto& peer : second->room.peers()) {
                const auto id = ++next_ping;
                record("ping-started", {{"room", "second"}, {"peer", peer}, {"id", id}});
                second->room.async_ping(
                    peer, [peer = trystero::peer_id(peer), id](boost::system::error_code ec,
                                                               std::chrono::milliseconds rtt) {
                        record("ping", {
                                           {"room", "second"},
                                           {"peer", peer},
                                           {"id", id},
                                           {"error", ec ? ec.message() : ""},
                                           {"ms", rtt.count()},
                                       });
                    });
            }
        }
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait([this](boost::system::error_code ec) {
            if (!ec) {
                ping();
            }
        });
    }
};

/** Reads the driver's commands while the rooms and timers continue running. */
struct command_reader {
    boost::asio::io_context& io;
    trystero::app& app;
    trystero::room& room;
    std::map<std::string, std::string>& args;
    boost::asio::posix::stream_descriptor input;
    boost::asio::streambuf line;
    std::function<void()> read_again;

    command_reader(boost::asio::io_context& context, trystero::app& application,
                   trystero::room& joined, std::map<std::string, std::string>& arguments)
        : io(context),
          app(application),
          room(joined),
          args(arguments),
          input(context, ::dup(STDIN_FILENO)),
          read_again([this] { read(); }) {}

    /** Whether another command should be read after this one. */
    bool handle(std::string_view command) {
        if (command.starts_with("abort")) {
            std::abort();
        }
        if (command.starts_with("leave-room")) {
            room.async_leave([](boost::system::error_code ec) {
                record("left", {{"room", "first"}, {"error", ec ? ec.message() : ""}});
            });
            return true;
        }
        if (command.starts_with("replace")) {
            replace(room, join(app, args["room"] + "-replaced", options_of(args)));
            return true;
        }
        if (command.starts_with("leave")) {
            leave(room, io);
            return false;
        }
        return true;
    }

    void read() {
        boost::asio::async_read_until(
            input, line, '\n', [this](boost::system::error_code ec, std::size_t size) {
                if (ec) {
                    return;
                }
                const std::string command(static_cast<const char*>(line.data().data()), size);
                line.consume(size);
                if (handle(command)) {
                    read_again();
                }
            });
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    auto args = arguments(argc, argv);
    boost::asio::io_context io;
    counting_backend backend(io.get_executor(), {.bind_address = "127.0.0.1"});
    backend.delay_open = !args["delayOpen"].empty();
    trystero::app app(io.get_executor(), backend, app_config_of(args));
    app.on_error([](std::string_view peer, boost::system::error_code ec, std::string_view what) {
        record("error", {{"peer", peer}, {"error", ec.message()}, {"what", what}});
    });
    trystero::room room = join(app, args["room"], options_of(args));
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
            record("ping", {
                               {"peer", peer},
                               {"error", ec ? ec.message() : ""},
                               {"ms", rtt.count()},
                           });
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

    std::unique_ptr<extra_room> second;
    boost::asio::steady_timer second_delay(io);
    if (!args["room2"].empty()) {
        second_delay.expires_after(millis(args["room2Delay"], std::chrono::milliseconds(0)));
        second_delay.async_wait([&](boost::system::error_code ec) {
            if (!ec) {
                second =
                    std::make_unique<extra_room>(app, "second", args["room2"], options_of(args));
            }
        });
    }
    std::unique_ptr<extra_room> third;
    boost::asio::steady_timer third_delay(io);
    if (!args["room3"].empty()) {
        third_delay.expires_after(millis(args["room3Delay"], std::chrono::milliseconds(0)));
        third_delay.async_wait([&](boost::system::error_code ec) {
            if (!ec) {
                third = std::make_unique<extra_room>(app, "third", args["room3"], options_of(args));
            }
        });
    }
    room_pinger pinger(io, second);
    if (!args["room2"].empty() && !args["ping2"].empty()) {
        pinger.ping();
    }

    command_reader commands(io, app, room, args);
    commands.read();
    io.run();  // lint-run: a process whose purpose is network I/O, until "leave" stops it
}
