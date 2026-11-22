// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the platform backend's peer connection natively, webcpp::webrtc's over libdatachannel,
// between two connections in one process, wired to each other directly: what the signaling would
// carry over the relays is handed across here. Each connection is bound to 127.0.0.1, so that its
// one host candidate is the loopback's and no other interface is needed. The backend makes at most
// `backend_config::max_peer_connections_per_minute` of them in a minute, whatever became of them,
// and holds at most its runtime's `max_peer_connections` at once.

#include <webcpp/trystero/platform/backend.hpp>
#include <webcpp/trystero/protocol/action_wire.hpp>
#include <webcpp/trystero/protocol/signal.hpp>
#include <webcpp/trystero/webrtc/data_channel.hpp>
#include <webcpp/trystero/webrtc/peer_connection.hpp>

#include <webcpp/webrtc/peer_connection.hpp>
#include <webcpp/webrtc/runtime.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/literal.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/stringify.hpp>

#include <boost/asio/execution.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "require.hpp"

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;
namespace backend = trystero::backend;
namespace protocol = trystero::protocol;
using namespace std::chrono_literals;

using webcpp::test::require;

namespace {

constexpr const char* loopback = "127.0.0.1";

bool run_until(asio::io_context& io, const std::function<bool()>& done,
               std::chrono::milliseconds limit = 10s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < end) {
        io.run_for(10ms);  // lint-run: libdatachannel's threads post the connections' events to it
        if (io.stopped()) {
            io.restart();
        }
    }
    return done();
}

/** The program's runtime, which outlives its io_context and every connection. */
std::shared_ptr<webcpp::webrtc::runtime> program_runtime(webcpp::webrtc::runtime& runtime) {
    return {std::shared_ptr<webcpp::webrtc::runtime>(), &runtime};
}

/** Two connected peers: the offerer's channel and the answerer's, with what each received. */
struct pair {
    webcpp::webrtc::runtime runtime;
    asio::io_context io;
    trystero::platform::backend factory{io.get_executor(), runtime, {.bind_address = loopback}};
    std::thread::id io_thread = std::this_thread::get_id();
    std::shared_ptr<backend::peer_connection> offerer;
    std::shared_ptr<backend::peer_connection> answerer;
    std::shared_ptr<backend::data_channel> offer_end;
    std::shared_ptr<backend::data_channel> answer_end;
    std::vector<backend::bytes> at_offerer, at_answerer;
    bool offer_open = false, answer_open = false, off_thread = false;
    std::vector<backend::connection_state> answerer_states;
    std::size_t answer_closes = 0;

    /** What makes the offerer's executor of the io_context's. */
    using executor_wrapper = std::function<asio::any_io_executor(asio::any_io_executor)>;

    /** The offerer is made in `offer_memory`, on the executor `wrap` makes of the io_context's;
     * the answerer as an app's. */
    explicit pair(std::pmr::memory_resource* offer_memory = std::pmr::get_default_resource(),
                  const executor_wrapper& wrap = {})
        : offerer(made(trystero::webrtc::peer_connection::make(
              program_runtime(runtime), wrap ? wrap(io.get_executor()) : io.get_executor(), {},
              loopback, offer_memory))),
          answerer(made(trystero::webrtc::peer_connection::make(
              program_runtime(runtime), io.get_executor(), {}, loopback,
              std::pmr::get_default_resource()))) {}

    pair(const pair&) = delete;
    pair& operator=(const pair&) = delete;

    /** Lets go of both connections and channels, which aborts what they wait for. */
    void reset() {
        offer_end.reset();
        answer_end.reset();
        offerer.reset();
        answerer.reset();
    }

    static std::shared_ptr<backend::peer_connection> made(
        boost::system::result<std::shared_ptr<trystero::webrtc::peer_connection>> r) {
        require(BOOST_TEST(r.has_value()));
        return std::move(*r);
    }

    void on_io_thread() {
        if (std::this_thread::get_id() != io_thread) {
            off_thread = true;
        }
    }

    void connect() {
        offerer->set_handlers({
            .ice_candidate =
                [this](std::string_view c) {
                    on_io_thread();
                    answerer->add_ice_candidate(c, [](backend::error_code) {});
                },
            .data_channel = {},
            .state = [this](backend::connection_state) { on_io_thread(); },
        });
        answerer->set_handlers({
            .ice_candidate =
                [this](std::string_view c) {
                    on_io_thread();
                    offerer->add_ice_candidate(c, [](backend::error_code) {});
                },
            .data_channel =
                [this](std::shared_ptr<backend::data_channel> channel) {
                    on_io_thread();
                    answer_end = std::move(channel);
                    watch(answer_end, answer_open, at_answerer, &answer_closes);
                },
            .state =
                [this](backend::connection_state s) {
                    on_io_thread();
                    answerer_states.push_back(s);
                },
        });
        offer_end = offerer->create_data_channel("data");
        require(BOOST_TEST(offer_end != nullptr));
        watch(offer_end, offer_open, at_offerer, nullptr);
        offerer->set_local_description([this](backend::error_code ec, std::string_view type,
                                              std::string_view sdp) {
            on_io_thread();
            require(BOOST_TEST(!ec));
            BOOST_TEST_EQ(type, "offer");
            answerer->set_remote_description(type, sdp, [this](backend::error_code ec) {
                require(BOOST_TEST(!ec));
                BOOST_TEST(answerer->has_remote_description());
                answerer->set_local_description(
                    [this](backend::error_code ec, std::string_view type, std::string_view sdp) {
                        require(BOOST_TEST(!ec));
                        BOOST_TEST_EQ(type, "answer");
                        offerer->set_remote_description(
                            type, sdp, [](backend::error_code ec) { BOOST_TEST(!ec); });
                    });
            });
        });
    }

    void watch(const std::shared_ptr<backend::data_channel>& channel, bool& open,
               std::vector<backend::bytes>& received, std::size_t* closes) {
        channel->set_handlers({
            .open =
                [this, &open] {
                    on_io_thread();
                    open = true;
                },
            .message =
                [this, &received](backend::bytes b) {
                    on_io_thread();
                    received.push_back(std::move(b));
                },
            .close =
                [this, closes] {
                    on_io_thread();
                    if (closes != nullptr) {
                        ++*closes;
                    }
                },
            .buffered_amount_low = {},
        });
    }
};

// Observes the offerer's posted callbacks, webcpp::webrtc's and the channel's, without running
// them. This lets a test install a receiver after a real message is read and before its delivery
// is dispatched.
struct queued_tasks {
    std::mutex mutex;
    std::deque<std::function<void()>> ready;

    std::size_t size() {
        const std::scoped_lock lock(mutex);
        return ready.size();
    }

    /** Runs the one task queued first, if any. */
    void run_one() {
        std::function<void()> f;
        {
            const std::scoped_lock lock(mutex);
            if (ready.empty()) {
                return;
            }
            f = std::move(ready.front());
            ready.pop_front();
        }
        f();
    }

    void run() {
        for (;;) {
            std::function<void()> f;
            {
                const std::scoped_lock lock(mutex);
                if (ready.empty()) {
                    return;
                }
                f = std::move(ready.front());
                ready.pop_front();
            }
            f();
        }
    }
};

class queued_executor {
    asio::any_io_executor inner_;
    std::shared_ptr<queued_tasks> tasks_;

public:
    queued_executor(asio::any_io_executor inner, std::shared_ptr<queued_tasks> tasks)
        : inner_(std::move(inner)), tasks_(std::move(tasks)) {}

    template <class F>
    void execute(F f) const {
        const auto operation = std::make_shared<F>(std::move(f));
        const std::scoped_lock lock(tasks_->mutex);
        tasks_->ready.emplace_back([operation] { (*operation)(); });
    }

    template <class Property>
    [[nodiscard]] auto query(Property p) const -> decltype(asio::query(inner_, p)) {
        return asio::query(inner_, p);
    }

    [[nodiscard]] queued_executor require(asio::execution::blocking_t::never_t p) const {
        return {asio::require(inner_, p), tasks_};
    }

    template <class Property>
    [[nodiscard]] queued_executor prefer(Property p) const {
        return {asio::prefer(inner_, p), tasks_};
    }

    friend bool operator==(const queued_executor& a, const queued_executor& b) noexcept {
        return a.inner_ == b.inner_ && a.tasks_ == b.tasks_;
    }
};

/** A pair whose offerer posts to a queue the test runs, in `memory`. */
struct queued_pair : pair {
    std::shared_ptr<queued_tasks> tasks;

    explicit queued_pair(
        std::pmr::memory_resource* memory = std::pmr::get_default_resource(),
        const std::shared_ptr<queued_tasks>& queue = std::make_shared<queued_tasks>())
        : pair(memory,
               [queue](asio::any_io_executor inner) {
                   return asio::any_io_executor(queued_executor(std::move(inner), queue));
               }),
          tasks(queue) {}

    queued_pair(const queued_pair&) = delete;
    queued_pair& operator=(const queued_pair&) = delete;

    /** Lets go of the connections, then of what their ends posted: a queued task holds the
     * queue's executor, which holds the queue. */
    // The queued tasks are the connections' handlers, which raise nothing; a failure to allocate
    // while they run ends the test, as in any destructor.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~queued_pair() {
        reset();
        tasks->run();
        const std::scoped_lock lock(tasks->mutex);
        tasks->ready.clear();
    }

    /** Connects the pair, and runs the offerer's events until both channels are open. */
    void open() {
        connect();
        require(BOOST_TEST(until([&] { return offer_open && answer_open; })));
    }

    bool until(const std::function<bool()>& done, std::chrono::milliseconds limit = 10s) {
        return run_until(
            io,
            [&] {
                tasks->run();
                return done();
            },
            limit);
    }

    /** Runs the offerer's events until its channel keeps `count` frames for a receiver. */
    void receive(std::size_t count) {
        require(BOOST_TEST(until([&] { return kept() >= count; })));
    }

    /** The frames the offerer's channel keeps for a receiver. */
    [[nodiscard]] std::size_t kept() const {
        return std::static_pointer_cast<trystero::webrtc::data_channel>(offer_end)
            ->pending_frames();
    }
};

void two_connections_open_a_data_channel_and_exchange_bytes() {
    pair p;
    p.connect();
    if (!BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; }))) {
        return;
    }
    BOOST_TEST(p.offer_end->is_open());
    BOOST_TEST(p.answer_end->is_open());
    const backend::bytes hello{1, 2, 3, 250};
    p.offer_end->send(hello);
    // The largest frame arrives.
    p.answer_end->send(backend::bytes(protocol::max_bulk_frame_bytes, 7));
    if (!BOOST_TEST(run_until(
            p.io, [&] { return p.at_answerer.size() == 1 && p.at_offerer.size() == 1; }))) {
        return;
    }
    BOOST_TEST_ALL_EQ(p.at_answerer.front().begin(), p.at_answerer.front().end(), hello.begin(),
                      hello.end());
    BOOST_TEST_EQ(p.at_offerer.front().size(), protocol::max_bulk_frame_bytes);
    BOOST_TEST_LE(p.offer_end->buffered_amount(), backend::data_channel::low_threshold);
    // One byte more than any frame reaches no handler and closes the channel, as 0.27.0's
    // failData closes it; the other end sees the close.
    p.answer_end->send(backend::bytes(protocol::max_bulk_frame_bytes + 1, 6));
    BOOST_TEST(run_until(p.io, [&] { return !p.offer_end->is_open() && p.answer_closes == 1; }));
    BOOST_TEST_EQ(p.at_offerer.size(), 1U);
    // Every handler ran on the executor, never on libdatachannel's threads.
    BOOST_TEST(!p.off_thread);
}

void closing_one_end_is_seen_by_the_other() {
    pair p;
    p.connect();
    if (!BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; }))) {
        return;
    }
    p.offerer->close();
    BOOST_TEST(run_until(p.io, [&] { return p.answer_closes == 1; }));
    BOOST_TEST(!p.off_thread);
}

// Presence can reach the native executor before the posted open event hands a channel over
// from signaling. Installing the receiver afterwards must deliver those frames in order.
void messages_wait_for_the_receiver_during_handoff() {
    queued_pair p;
    p.open();
    require(BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; })));
    p.offer_end->set_handlers({});
    const backend::bytes first{2, 1, 0, 1, 'a'};
    const backend::bytes second{2, 1, 0, 1, 'b'};
    p.answer_end->send(first);
    p.answer_end->send(second);
    p.receive(2);
    p.watch(p.offer_end, p.offer_open, p.at_offerer, nullptr);
    require(BOOST_TEST(p.until([&] { return p.at_offerer.size() == 2; })));
    BOOST_TEST(p.at_offerer[0] == first);
    BOOST_TEST(p.at_offerer[1] == second);
}

void a_full_pending_queue_drains_in_order_and_uses_replacement_handlers() {
    queued_pair p;
    p.open();
    require(BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; })));
    p.offer_end->set_handlers({});
    for (std::uint8_t i = 0; i < 64; ++i) {
        p.answer_end->send(backend::bytes{i});
    }
    p.receive(64);
    std::vector<std::uint8_t> received;
    // The 65th frame's delivery is already queued before set_handlers posts its flush: the
    // channel's read, then its completion.
    p.answer_end->send(backend::bytes{64});
    require(BOOST_TEST(run_until(p.io, [&] { return p.tasks->size() >= 1; })));
    p.tasks->run_one();
    require(BOOST_TEST(run_until(p.io, [&] { return p.tasks->size() >= 1; })));
    bool first = true;
    bool registering = true;
    p.offer_end->set_handlers({
        .open = {},
        .message =
            [&](backend::bytes frame) {
                BOOST_TEST(!registering);
                received.push_back(frame[0]);
                BOOST_TEST(first);
                first = false;
                p.offer_end->set_handlers({
                    .open = {},
                    .message = [&](backend::bytes later) { received.push_back(later[0]); },
                    .close = {},
                    .buffered_amount_low = {},
                });
            },
        .close = {},
        .buffered_amount_low = {},
    });
    registering = false;
    require(BOOST_TEST(p.until([&] { return received.size() == 65; })));
    for (std::size_t i = 0; i < received.size(); ++i) {
        BOOST_TEST_EQ(received[i], static_cast<std::uint8_t>(i));
    }
    // A later handoff must post a new flush after the first one has finished.
    p.offer_end->set_handlers({});
    p.answer_end->send(backend::bytes{65});
    p.receive(1);
    p.offer_end->set_handlers({
        .open = {},
        .message = [&](backend::bytes later) { received.push_back(later[0]); },
        .close = {},
        .buffered_amount_low = {},
    });
    require(BOOST_TEST(p.until([&] { return received.size() == 66; })));
    BOOST_TEST_EQ(received.back(), 65U);
}

void a_pending_queue_overflow_closes_the_channel() {
    queued_pair p;
    p.open();
    require(BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; })));
    bool closed = false;
    p.offer_end->set_handlers({
        .open = {},
        .message = {},
        .close = [&] { closed = true; },
        .buffered_amount_low = {},
    });
    for (std::uint8_t i = 0; i < 65; ++i) {
        p.answer_end->send(backend::bytes{i});
    }
    require(BOOST_TEST(p.until([&] { return closed; }, 5s)));
    BOOST_TEST(!p.offer_end->is_open());
}

// A frame kept for a receiver is at most 0.27.0's maxRoomFrameBytes, 17 KiB, as the room's link
// keeps before its room binds it: one byte more closes the channel, as the link would end on it.
// A frame of that size is kept, with no time bound of its own.
void a_pending_frame_past_the_room_frame_bound_closes_the_channel() {
    namespace webrtc = trystero::webrtc;
    BOOST_TEST_EQ(webrtc::data_channel::max_pending_frame_bytes, std::size_t{17} * 1024);
    BOOST_TEST_EQ(webrtc::data_channel::max_pending_frames, 64U);
    queued_pair p;
    p.open();
    require(BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; })));
    bool closed = false;
    p.offer_end->set_handlers({
        .open = {},
        .message = {},
        .close = [&] { closed = true; },
        .buffered_amount_low = {},
    });
    p.answer_end->send(backend::bytes(webrtc::data_channel::max_pending_frame_bytes, 1));
    p.receive(1);
    BOOST_TEST(p.offer_end->is_open());
    p.answer_end->send(backend::bytes(webrtc::data_channel::max_pending_frame_bytes + 1, 2));
    require(BOOST_TEST(p.until([&] { return closed; })));
    BOOST_TEST(!p.offer_end->is_open());
    BOOST_TEST_EQ(p.kept(), 0U);
}

// The SCTP transport's largest message is unknown until a remote description gives the connection
// one, then the remote's `a=max-message-size`, libdatachannel's 256 KiB, within what this end
// sends, webcpp::webrtc's 256 KiB: a peer that takes wide chunks.
void max_message_size_is_known_once_connected() {
    pair p;
    BOOST_TEST(!p.offerer->max_message_size().has_value());
    BOOST_TEST(!p.answerer->max_message_size().has_value());
    p.connect();
    require(BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; })));
    const std::optional<std::size_t> expected = std::size_t{256} * 1024;
    BOOST_TEST(p.offerer->max_message_size() == expected);
    BOOST_TEST(p.answerer->max_message_size() == expected);
}

// What the remote description says, as the W3C and libdatachannel read it: RFC 8841's 64 KiB
// when it says nothing, any size when it says 0, never more than this end sends, and nothing
// without a data channel section.
void max_message_size_is_read_from_the_remote_description() {
    using trystero::webrtc::detail::sctp_max_message_size;
    constexpr std::size_t local = std::size_t{256} * 1024;
    const std::string_view head = "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n";
    const auto with = [&](std::string_view section) {
        return std::string(head) + std::string(section);
    };
    const std::string app =
        "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\n";
    BOOST_TEST(sctp_max_message_size(with(app + "a=max-message-size:262144\r\n"), local) ==
               std::optional<std::size_t>(local));
    BOOST_TEST(sctp_max_message_size(with(app + "a=max-message-size:16384\r\n"), local) ==
               std::optional<std::size_t>(16384));
    BOOST_TEST(sctp_max_message_size(with(app + "a=max-message-size:1073741823\n"), local) ==
               std::optional<std::size_t>(local));
    BOOST_TEST(sctp_max_message_size(with(app + "a=max-message-size:0\r\n"), local) ==
               std::optional<std::size_t>(local));
    BOOST_TEST(sctp_max_message_size(with(app), local) == std::optional<std::size_t>(65536));
    BOOST_TEST(sctp_max_message_size(with(app + "a=max-message-size:12x\r\n"), local) ==
               std::optional<std::size_t>(65536));
    // Another section's attribute is not the transport's.
    BOOST_TEST(sctp_max_message_size(with("m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
                                          "a=max-message-size:16384\r\n" +
                                          app),
                                     local) == std::optional<std::size_t>(65536));
    BOOST_TEST(!sctp_max_message_size(with("m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"), local));
    BOOST_TEST(!sctp_max_message_size("", local));
}

void a_receiver_can_close_and_destroy_its_channel_during_pending_delivery() {
    queued_pair p;
    p.open();
    require(BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; })));
    p.offer_end->set_handlers({});
    p.answer_end->send(backend::bytes{1});
    p.answer_end->send(backend::bytes{2});
    p.receive(2);
    std::size_t received = 0;
    p.offer_end->set_handlers({
        .open = {},
        .message =
            [&](const backend::bytes&) {
                ++received;
                p.offer_end->close();
                p.offer_end.reset();
            },
        .close = {},
        .buffered_amount_low = {},
    });
    require(BOOST_TEST(p.until([&] { return p.answer_closes == 1; })));
    BOOST_TEST_EQ(received, 1U);
}

class pending_memory : public std::pmr::memory_resource {
public:
    std::atomic<std::size_t> outstanding{0};

    [[nodiscard]] std::unordered_set<const void*> allocations() {
        const std::scoped_lock lock(mutex_);
        return allocations_;
    }

private:
    std::mutex mutex_;
    std::unordered_set<const void*> allocations_;

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* p = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        outstanding += bytes;
        {
            const std::scoped_lock lock(mutex_);
            allocations_.insert(p);
        }
        return p;
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        {
            const std::scoped_lock lock(mutex_);
            allocations_.erase(p);
        }
        outstanding -= bytes;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

void pending_frames_and_delivery_use_the_channels_resource() {
    pending_memory memory;
    {
        queued_pair p(&memory);
        p.open();
        require(BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; })));
        p.offer_end->set_handlers({});
        for (std::size_t i = 0; i < 64; ++i) {
            p.answer_end->send(backend::bytes(1024, static_cast<std::uint8_t>(i)));
        }
        p.receive(64);
        // Deque bookkeeping can allocate the same byte count as a payload. Each
        // delivered buffer must already belong to this resource before registration.
        auto queued = memory.allocations();
        bool registering = true;
        p.offer_end->set_handlers({
            .open = {},
            .message =
                [&](backend::bytes frame) {
                    BOOST_TEST(!registering);
                    BOOST_TEST(frame.get_allocator().resource() == &memory);
                    BOOST_TEST_EQ(frame.size(), 1024U);
                    BOOST_TEST_EQ(queued.erase(frame.data()), 1U);
                    p.at_offerer.push_back(std::move(frame));
                },
            .close = {},
            .buffered_amount_low = {},
        });
        registering = false;
        require(BOOST_TEST(p.until([&] { return p.at_offerer.size() == 64; })));
    }
    BOOST_TEST_EQ(memory.outstanding.load(), 0U);
}

void a_malformed_candidate_fails_without_throwing() {
    asio::io_context io;
    trystero::platform::backend factory(io.get_executor(), {.bind_address = loopback});
    const auto pc = factory.make_peer_connection({}, std::pmr::get_default_resource());
    require(BOOST_TEST(pc != nullptr));
    std::vector<backend::error_code> results;
    pc->add_ice_candidate("not json", [&](backend::error_code ec) { results.push_back(ec); });
    pc->add_ice_candidate(R"({"sdpMid":"0"})",
                          [&](backend::error_code ec) { results.push_back(ec); });
    // A candidate before a remote description, which the signaling never adds.
    pc->add_ice_candidate(
        R"({"candidate":"candidate:1 1 UDP 1 127.0.0.1 9 typ host","sdpMid":"0"})",
        [&](backend::error_code ec) { results.push_back(ec); });
    pc->set_remote_description("neither", "v=0",
                               [&](backend::error_code ec) { results.push_back(ec); });
    if (!BOOST_TEST(run_until(io, [&] { return results.size() == 4; }))) {
        return;
    }
    for (const auto& ec : results) {
        BOOST_TEST(ec == trystero::error::backend_failure);
    }
}

/** `json`, an object's text, with spaces before its closing brace, to `size` bytes. */
std::string padded(std::string_view json, std::size_t size) {
    require(BOOST_TEST(json.ends_with('}') && json.size() <= size));
    std::string out(json.substr(0, json.size() - 1));
    out.append(size - json.size(), ' ');
    out += '}';
    return out;
}

// A remote candidate of protocol::max_sealed_candidate_bytes, a host candidate's JSON with spaces
// before its closing brace, is added once an offer is the connection's remote description; one
// byte longer fails with error::backend_failure before it is read.
void a_candidate_past_its_bound_fails() {
    asio::io_context io;
    trystero::platform::backend factory(io.get_executor(), {.bind_address = loopback});
    const auto offerer = factory.make_peer_connection({}, std::pmr::get_default_resource());
    const auto answerer = factory.make_peer_connection({}, std::pmr::get_default_resource());
    require(BOOST_TEST(offerer != nullptr && answerer != nullptr));
    const auto channel = offerer->create_data_channel("data");
    require(BOOST_TEST(channel != nullptr));
    std::string mid;
    bool applied = false;
    offerer->set_local_description(
        [&](backend::error_code ec, std::string_view type, std::string_view sdp) {
            require(BOOST_TEST(!ec));
            // The data channel's media section, "a=mid:<mid>", which a candidate names.
            const auto at = sdp.find("a=mid:");
            require(BOOST_TEST(at != std::string_view::npos));
            const auto rest = sdp.substr(at + 6);
            mid = rest.substr(0, rest.find_first_of("\r\n"));
            answerer->set_remote_description(type, sdp, [&](backend::error_code ec) {
                require(BOOST_TEST(!ec));
                applied = true;
            });
        });
    require(BOOST_TEST(run_until(io, [&] { return applied; })));
    const std::string candidate =
        R"({"candidate":"candidate:1 1 UDP 2122317823 127.0.0.1 9 typ host","sdpMid":")" + mid +
        R"("})";
    constexpr std::size_t bound = protocol::max_sealed_candidate_bytes;
    std::vector<backend::error_code> results;
    answerer->add_ice_candidate(padded(candidate, bound),
                                [&](backend::error_code ec) { results.push_back(ec); });
    answerer->add_ice_candidate(padded(candidate, bound + 1),
                                [&](backend::error_code ec) { results.push_back(ec); });
    if (!BOOST_TEST(run_until(io, [&] { return results.size() == 2; }))) {
        return;
    }
    BOOST_TEST(!results[0]);
    BOOST_TEST(results[1] == trystero::error::backend_failure);
}

// A local candidate is reported as the JSON of an RTCIceCandidateInit, as JSON.stringify writes a
// browser's: its mid and index null when unknown, its user name fragment only when it has one. One
// whose candidate, mid or user name fragment is not UTF-8, which JSON cannot carry, is refused, so
// the connection reports none.
void a_candidate_that_is_not_utf8_is_not_written() {
    const auto json = [](std::string_view candidate, std::string_view mid,
                         std::optional<std::uint16_t> index, std::string_view fragment) {
        return trystero::webrtc::detail::candidate_json(candidate, mid, index, fragment,
                                                        std::pmr::get_default_resource());
    };
    const auto full = json("candidate:1 1 UDP 1 127.0.0.1 9 typ host", "0", 0, "uf\xc3\xa9");
    if (BOOST_TEST(full.has_value())) {
        BOOST_TEST_EQ(*full,
                      "{\"candidate\":\"candidate:1 1 UDP 1 127.0.0.1 9 typ host\","
                      "\"sdpMid\":\"0\",\"sdpMLineIndex\":0,\"usernameFragment\":\"uf\xc3\xa9\"}");
    }
    const auto bare = json("candidate:\"", {}, std::nullopt, {});
    if (BOOST_TEST(bare.has_value())) {
        BOOST_TEST_EQ(*bare,
                      "{\"candidate\":\"candidate:\\\"\",\"sdpMid\":null,\"sdpMLineIndex\":null}");
    }
    for (const auto& refused : {
             json("candidate:\xff", "0", 0, {}),
             json("candidate:1", "\xc0\xaf", 0, {}),
             json("candidate:1", "0", 0, "\xed\xa0\x80"),
         }) {
        BOOST_TEST(refused.has_error() && refused.error() == webcpp::document::errc::type);
    }
}

void the_backend_makes_at_most_its_budget_in_a_minute() {
    asio::io_context io;
    trystero::platform::backend factory(
        io.get_executor(),
        {.bind_address = std::pmr::string(loopback), .max_peer_connections_per_minute = 3});
    std::vector<std::shared_ptr<backend::peer_connection>> made;
    for (std::size_t i = 0; i < 3; ++i) {
        made.push_back(factory.make_peer_connection({}, std::pmr::get_default_resource()));
        BOOST_TEST(made.back() != nullptr);
    }
    BOOST_TEST(factory.make_peer_connection({}, std::pmr::get_default_resource()) == nullptr);
    // Closed and dropped, they still count: the budget counts what was made.
    for (const auto& pc : made) {
        pc->close();
    }
    made.clear();
    BOOST_TEST(factory.make_peer_connection({}, std::pmr::get_default_resource()) == nullptr);
}

void the_runtime_holds_at_most_its_cap_at_once() {
    asio::io_context io;
    trystero::platform::backend factory(
        io.get_executor(),
        {.bind_address = std::pmr::string(loopback), .runtime = {.max_peer_connections = 2}});
    auto first = factory.make_peer_connection({}, std::pmr::get_default_resource());
    const auto second = factory.make_peer_connection({}, std::pmr::get_default_resource());
    BOOST_TEST(first != nullptr);
    BOOST_TEST(second != nullptr);
    BOOST_TEST_EQ(factory.runtime().peer_connections(), 2U);
    BOOST_TEST(factory.make_peer_connection({}, std::pmr::get_default_resource()) == nullptr);
    // Unlike the minute's budget, a connection gives its place back once it is gone.
    first.reset();
    BOOST_TEST_EQ(factory.runtime().peer_connections(), 1U);
    BOOST_TEST(factory.make_peer_connection({}, std::pmr::get_default_resource()) != nullptr);
}

void a_url_libdatachannel_cannot_parse_is_left_out() {
    asio::io_context io;
    trystero::platform::backend factory(io.get_executor(), {.bind_address = loopback});
    // stuns:, which WebRTC admits and libdatachannel does not know, beside a server it parses.
    std::vector<backend::ice_server> servers;
    servers.push_back({
        .urls = {"stuns:127.0.0.1:9", "stun:127.0.0.1:9"},
        .username = {},
        .credential = {},
    });
    servers.push_back({.urls = {"stuns:127.0.0.1:10"}, .username = {}, .credential = {}});
    const auto pc = factory.make_peer_connection(servers, std::pmr::get_default_resource());
    BOOST_TEST(pc != nullptr);
    // The probes are given up: only the connection made counts.
    BOOST_TEST_EQ(factory.runtime().peer_connections(), 1U);
}

/** The JSON of a candidate of webcpp::webrtc, as a peer sends it. */
std::string json_of(const webcpp::webrtc::ice_candidate& c) {
    auto made = webcpp::document::document::create();
    require(BOOST_TEST(made.has_value()));
    const auto set = made->root().set(webcpp::document::object{
        {"candidate", std::string_view(c.candidate)},
        {"sdpMid", std::string_view(c.sdp_mid)},
        {"sdpMLineIndex", c.sdp_m_line_index.value_or(0)},
    });
    require(BOOST_TEST(set.has_value()));
    return made->stringify();
}

/**
 * The platform backend's connection, offering, and a peer connection of webcpp::webrtc's own,
 * answering, in one process: what the signaling would carry is handed across, each candidate held
 * until the side it goes to has both descriptions.
 */
struct mixed_pair {
    webcpp::webrtc::runtime runtime;
    asio::io_context io;
    trystero::platform::backend factory{io.get_executor(), runtime, {.bind_address = loopback}};
    std::shared_ptr<backend::peer_connection> local =
        factory.make_peer_connection({}, std::pmr::get_default_resource());
    std::optional<webcpp::webrtc::peer_connection> remote;
    std::optional<webcpp::webrtc::data_channel> sender;
    std::shared_ptr<backend::data_channel> channel;
    std::vector<backend::bytes> received;
    std::size_t closes = 0;  // the closes the backend's channel reported
    bool negotiated = false;
    std::vector<std::string> to_remote;
    std::vector<std::string> to_local;

    mixed_pair() {
        require(BOOST_TEST(local != nullptr));
        webcpp::webrtc::configuration config;
        config.bind_address = loopback;
        auto made = webcpp::webrtc::peer_connection::make(runtime, io.get_executor(), config);
        require(BOOST_TEST(made.has_value()));
        remote.emplace(std::move(*made));
    }

    mixed_pair(const mixed_pair&) = delete;
    mixed_pair& operator=(const mixed_pair&) = delete;

    /** Lets go of the remote peer first, then of the backend's connection. */
    ~mixed_pair() {
        sender.reset();
        remote.reset();
        channel.reset();
        local.reset();
    }

    void add_to_remote(std::string_view json) {
        const auto parsed = webcpp::document::parse(json);
        require(BOOST_TEST(parsed.has_value()));
        const auto text = parsed->root()["candidate"].get<std::string_view>();
        const auto mid = parsed->root()["sdpMid"].get<std::string_view>();
        require(BOOST_TEST(text.has_value() && mid.has_value()));
        remote->async_add_ice_candidate({.candidate = *text, .sdp_mid = *mid},
                                        [](webcpp::webrtc::detail::error_code) {});
    }

    void add_to_local(const webcpp::webrtc::ice_candidate& c) {
        if (negotiated) {
            local->add_ice_candidate(json_of(c), [](backend::error_code) {});
        } else {
            to_local.push_back(json_of(c));
        }
    }

    void trickle_remote() {
        remote->async_wait_ice_candidate(
            [this](webcpp::webrtc::detail::error_code ec, const webcpp::webrtc::ice_candidate& c) {
                if (!ec && !c.candidate.empty()) {
                    add_to_local(c);
                    trickle_remote();
                }
            });
    }

    /** Both descriptions are set: the candidates held go now. */
    void on_negotiated() {
        negotiated = true;
        for (const auto& c : std::exchange(to_remote, {})) {
            add_to_remote(c);
        }
        for (const auto& c : std::exchange(to_local, {})) {
            local->add_ice_candidate(c, [](backend::error_code) {});
        }
    }

    void answer(std::string_view offer) {
        remote->async_set_remote_description(
            {.type = webcpp::webrtc::sdp_type::offer, .sdp = offer},
            [this](webcpp::webrtc::detail::error_code ec) {
                require(BOOST_TEST(!ec));
                remote->async_set_local_description(
                    [this](webcpp::webrtc::detail::error_code ec,
                           const webcpp::webrtc::session_description& d) {
                        require(BOOST_TEST(!ec));
                        local->set_remote_description("answer", d.sdp,
                                                      [this](backend::error_code ec) {
                                                          require(BOOST_TEST(!ec));
                                                          on_negotiated();
                                                      });
                    });
            });
    }

    /** Opens the backend's channel to the remote peer, which accepts it as `sender`. */
    void connect() {
        local->set_handlers({
            .ice_candidate =
                [this](std::string_view json) {
                    if (negotiated) {
                        add_to_remote(json);
                    } else {
                        to_remote.emplace_back(json);
                    }
                },
            .data_channel = {},
            .state = {},
        });
        trickle_remote();
        remote->async_accept_data_channel(
            [this](webcpp::webrtc::detail::error_code ec, webcpp::webrtc::data_channel c) {
                if (!ec) {
                    sender.emplace(std::move(c));
                }
            });
        channel = local->create_data_channel("data");
        require(BOOST_TEST(channel != nullptr));
        channel->set_handlers({
            .open = {},
            .message = [this](backend::bytes frame) { received.push_back(std::move(frame)); },
            .close = [this] { ++closes; },
            .buffered_amount_low = {},
        });
        local->set_local_description(
            [this](backend::error_code ec, std::string_view, std::string_view offer) {
                require(BOOST_TEST(!ec));
                answer(offer);
            });
        require(BOOST_TEST(run_until(io, [this] {
            return channel->is_open() && sender &&
                   sender->ready_state() == webcpp::webrtc::data_channel_state::open;
        })));
    }
};

// A peer may send what is no frame: a text message, which a data channel of webcpp::webrtc
// delivers as text, closes the channel, as 0.27.0's failData closes it; the frame before it
// arrives, and nothing after it. The remote peer is webcpp::webrtc's own, which sends text.
void a_text_message_closes_the_channel() {
    mixed_pair p;
    p.connect();
    const std::vector<std::uint8_t> before{1, 2, 3};
    const std::vector<std::uint8_t> after{4, 5};
    BOOST_TEST(!p.sender->send(asio::buffer(before)));
    BOOST_TEST(!p.sender->send_text("1e300"));
    BOOST_TEST(!p.sender->send(asio::buffer(after)));
    require(BOOST_TEST(run_until(p.io, [&] { return p.closes == 1; })));
    BOOST_TEST(!p.channel->is_open());
    require(BOOST_TEST_EQ(p.received.size(), 1U));
    BOOST_TEST_ALL_EQ(p.received[0].begin(), p.received[0].end(), before.begin(), before.end());
}

}  // namespace

int main() {
    two_connections_open_a_data_channel_and_exchange_bytes();
    closing_one_end_is_seen_by_the_other();
    messages_wait_for_the_receiver_during_handoff();
    a_full_pending_queue_drains_in_order_and_uses_replacement_handlers();
    a_pending_queue_overflow_closes_the_channel();
    a_pending_frame_past_the_room_frame_bound_closes_the_channel();
    max_message_size_is_known_once_connected();
    max_message_size_is_read_from_the_remote_description();
    a_receiver_can_close_and_destroy_its_channel_during_pending_delivery();
    a_malformed_candidate_fails_without_throwing();
    a_candidate_past_its_bound_fails();
    a_candidate_that_is_not_utf8_is_not_written();
    the_backend_makes_at_most_its_budget_in_a_minute();
    the_runtime_holds_at_most_its_cap_at_once();
    a_url_libdatachannel_cannot_parse_is_left_out();
    a_text_message_closes_the_channel();
    pending_frames_and_delivery_use_the_channels_resource();
    return boost::report_errors();
}
