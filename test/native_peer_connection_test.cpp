// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the native peer connection, libdatachannel's, between two connections in one process,
// wired to each other directly: what the signaling would carry over the relays is handed across
// here. Each connection is bound to 127.0.0.1, so that its one host candidate is the loopback's and
// no other interface is needed. The backend makes at most
// `backend_config::max_peer_connections_per_minute` of them in a minute, whatever became of them.

#include <webcpp/trystero/native/backend.hpp>
#include <webcpp/trystero/native/peer_connection.hpp>
#include <webcpp/trystero/protocol/envelope.hpp>

#include <boost/asio/execution.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <atomic>
#include <chrono>
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

/** Two connected peers: the offerer's channel and the answerer's, with what each received. */
struct pair {
    asio::io_context io;
    std::thread::id io_thread = std::this_thread::get_id();
    std::shared_ptr<backend::peer_connection> offerer = trystero::native::peer_connection::create(
        io.get_executor(), {}, loopback, std::pmr::get_default_resource());
    std::shared_ptr<backend::peer_connection> answerer = trystero::native::peer_connection::create(
        io.get_executor(), {}, loopback, std::pmr::get_default_resource());
    std::shared_ptr<backend::data_channel> offer_end;
    std::shared_ptr<backend::data_channel> answer_end;
    std::vector<backend::bytes> at_offerer, at_answerer;
    bool offer_open = false, answer_open = false, off_thread = false;
    std::vector<backend::connection_state> answerer_states;
    int answer_closes = 0;

    void on_io_thread() {
        if (std::this_thread::get_id() != io_thread) {
            off_thread = true;
        }
    }

    void connect(std::pmr::memory_resource* channel_memory = nullptr,
                 const std::optional<asio::any_io_executor>& channel_executor = {}) {
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
        if (channel_memory != nullptr || channel_executor) {
            const auto native =
                std::static_pointer_cast<trystero::native::peer_connection>(offerer);
            offer_end = trystero::native::data_channel::create(
                channel_executor.value_or(io.get_executor()),
                native->native_handle()->createDataChannel("data"),
                channel_memory != nullptr ? channel_memory : std::pmr::get_default_resource());
        } else {
            offer_end = offerer->create_data_channel("data");
        }
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
               std::vector<backend::bytes>& received, int* closes) {
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

// Observes the native channel's posted callbacks without running them. This lets a test
// install a receiver after a real message is queued and before that message is dispatched.
struct queued_tasks {
    std::mutex mutex;
    std::deque<std::function<void()>> ready;

    std::size_t size() {
        const std::scoped_lock lock(mutex);
        return ready.size();
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

struct queued_pair : pair {
    std::shared_ptr<queued_tasks> tasks = std::make_shared<queued_tasks>();

    void connect(std::pmr::memory_resource* memory = nullptr) {
        pair::connect(memory, asio::any_io_executor(queued_executor(io.get_executor(), tasks)));
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

    void receive(std::size_t count) {
        require(BOOST_TEST(run_until(io, [&] { return tasks->size() >= count; })));
        tasks->run();
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
    // One byte more than any room frame is dropped before it is read; the largest one arrives.
    p.answer_end->send(backend::bytes(protocol::max_room_frame_bytes + 1, 6));
    p.answer_end->send(backend::bytes(protocol::max_room_frame_bytes, 7));
    if (!BOOST_TEST(run_until(
            p.io, [&] { return p.at_answerer.size() == 1 && p.at_offerer.size() == 1; }))) {
        return;
    }
    BOOST_TEST_ALL_EQ(p.at_answerer.front().begin(), p.at_answerer.front().end(), hello.begin(),
                      hello.end());
    BOOST_TEST_EQ(p.at_offerer.size(), 1U);
    BOOST_TEST_EQ(p.at_offerer.front().size(), protocol::max_room_frame_bytes);
    BOOST_TEST_LE(p.offer_end->buffered_amount(), backend::data_channel::low_threshold);
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
    p.connect();
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
    p.connect();
    require(BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; })));
    p.offer_end->set_handlers({});
    for (std::uint8_t i = 0; i < 64; ++i) {
        p.answer_end->send(backend::bytes{i});
    }
    p.receive(64);
    std::vector<int> received;
    // The 65th callback is already queued before set_handlers posts its flush.
    p.answer_end->send(backend::bytes{64});
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
        BOOST_TEST_EQ(received[i], static_cast<int>(i));
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
    BOOST_TEST_EQ(received.back(), 65);
}

void a_pending_queue_overflow_closes_the_channel() {
    queued_pair p;
    p.connect();
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

void pending_messages_expire_without_a_receiver() {
    queued_pair p;
    p.connect();
    require(BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; })));
    bool closed = false;
    p.offer_end->set_handlers({
        .open = {},
        .message = {},
        .close = [&] { closed = true; },
        .buffered_amount_low = {},
    });
    const auto started = std::chrono::steady_clock::now();
    p.answer_end->send(backend::bytes{1});
    require(BOOST_TEST(p.until([&] { return closed; }, 12s)));
    BOOST_TEST(std::chrono::steady_clock::now() - started >= 10s);
}

void a_receiver_can_close_and_destroy_its_channel_during_pending_delivery() {
    queued_pair p;
    p.connect();
    require(BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; })));
    p.offer_end->set_handlers({});
    p.answer_end->send(backend::bytes{1});
    p.answer_end->send(backend::bytes{2});
    p.receive(2);
    int received = 0;
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
    BOOST_TEST_EQ(received, 1);
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
        queued_pair p;
        p.connect(&memory);
        require(BOOST_TEST(run_until(p.io, [&] { return p.offer_open && p.answer_open; })));
        p.offer_end->set_handlers({});
        for (int i = 0; i < 64; ++i) {
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
    const auto pc = trystero::native::peer_connection::create(io.get_executor(), {}, loopback,
                                                              std::pmr::get_default_resource());
    std::optional<backend::error_code> result;
    pc->add_ice_candidate("not json", [&](backend::error_code ec) { result = ec; });
    if (!BOOST_TEST(run_until(io, [&] { return result.has_value(); }))) {
        return;
    }
    BOOST_TEST(*result == trystero::error::backend_failure);
}

void the_backend_makes_at_most_its_budget_in_a_minute() {
    asio::io_context io;
    trystero::native::backend factory(
        io.get_executor(),
        {.bind_address = std::pmr::string(loopback), .max_peer_connections_per_minute = 3});
    std::vector<std::shared_ptr<backend::peer_connection>> made;
    for (int i = 0; i < 3; ++i) {
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

}  // namespace

int main() {
    two_connections_open_a_data_channel_and_exchange_bytes();
    closing_one_end_is_seen_by_the_other();
    messages_wait_for_the_receiver_during_handoff();
    a_full_pending_queue_drains_in_order_and_uses_replacement_handlers();
    a_pending_queue_overflow_closes_the_channel();
    pending_messages_expire_without_a_receiver();
    a_receiver_can_close_and_destroy_its_channel_during_pending_delivery();
    a_malformed_candidate_fails_without_throwing();
    the_backend_makes_at_most_its_budget_in_a_minute();
    pending_frames_and_delivery_use_the_channels_resource();
    return boost::report_errors();
}
