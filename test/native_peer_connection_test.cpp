// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the native peer connection, libdatachannel's, between two connections in one process,
// wired to each other directly: what the signaling would carry over the relays is handed across
// here.

#include <webcpp/trystero/native/peer_connection.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "require.hpp"

namespace asio = boost::asio;
namespace trystero = webcpp::trystero;
namespace backend = trystero::backend;
using namespace std::chrono_literals;

using webcpp::test::require;

namespace {

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
    std::shared_ptr<backend::peer_connection> offerer =
        trystero::native::peer_connection::create(io.get_executor(), {});
    std::shared_ptr<backend::peer_connection> answerer =
        trystero::native::peer_connection::create(io.get_executor(), {});
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
                    answer_end = channel;
                    watch(answer_end, answer_open, at_answerer, &answer_closes);
                },
            .state =
                [this](backend::connection_state s) {
                    on_io_thread();
                    answerer_states.push_back(s);
                },
        });
        offer_end = offerer->create_data_channel("data");
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
    p.answer_end->send(backend::bytes(20'000, 7));
    if (!BOOST_TEST(run_until(
            p.io, [&] { return p.at_answerer.size() == 1 && p.at_offerer.size() == 1; }))) {
        return;
    }
    BOOST_TEST_ALL_EQ(p.at_answerer.front().begin(), p.at_answerer.front().end(), hello.begin(),
                      hello.end());
    BOOST_TEST_EQ(p.at_offerer.front().size(), 20'000U);
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

void a_malformed_candidate_fails_without_throwing() {
    asio::io_context io;
    auto pc = trystero::native::peer_connection::create(io.get_executor(), {});
    std::optional<backend::error_code> result;
    pc->add_ice_candidate("not json", [&](backend::error_code ec) { result = ec; });
    if (!BOOST_TEST(run_until(io, [&] { return result.has_value(); }))) {
        return;
    }
    BOOST_TEST(*result == trystero::error::backend_failure);
}

}  // namespace

int main() {
    two_connections_open_a_data_channel_and_exchange_bytes();
    closing_one_end_is_seen_by_the_other();
    a_malformed_candidate_fails_without_throwing();
    return boost::report_errors();
}
