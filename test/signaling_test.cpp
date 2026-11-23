// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the signaling and the Nostr strategy alone, over the memory backend, on both targets. As
// an answerer, the signaling publishes the bytes Trystero JS 0.27.0's own signal handler published
// for the same offers, which the original's oracle wrote into fixtures/oracle/expected.json, the
// test's one argument, but for each sealed text, sealed under an IV of its own: it opens, with its
// AAD, to the plaintext 0.27.0 sealed there; its peer connections give the oracle's answer and
// candidate. Each text a strategy or a signaling reads is bounded: a relay's text or a signal at
// its bound is read, one byte longer is dropped.

#include <webcpp/trystero/backend.hpp>
#include <webcpp/trystero/detail/log.hpp>
#include <webcpp/trystero/detail/nostr_strategy.hpp>
#include <webcpp/trystero/detail/signaling.hpp>
#include <webcpp/trystero/error.hpp>
#include <webcpp/trystero/handler.hpp>
#include <webcpp/trystero/protocol/json.hpp>
#include <webcpp/trystero/protocol/nostr.hpp>
#include <webcpp/trystero/protocol/room_keys.hpp>
#include <webcpp/trystero/protocol/seal.hpp>
#include <webcpp/trystero/protocol/signal.hpp>

#include <webcpp/crypto/secret.hpp>

#include "support/memory_backend.hpp"

#include <webcpp/asio/io_context.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/quote.hpp>
#include <webcpp/document/value.hpp>

#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "require.hpp"

namespace asio = webcpp::asio;
namespace trystero = webcpp::trystero;
namespace backend = trystero::backend;
namespace detail = trystero::detail;
namespace tp = trystero::protocol;
using trystero::test::memory_network;
using trystero::test::post_in;
using webcpp::test::require;

namespace {

/** A value of the fixture: a read-only handle of an owned document. */
using fixture_value = webcpp::document::basic_const_value<>;

/** The fixture at `path`, read with webcpp::document; nothing when it does not parse. */
std::optional<webcpp::document::document> load(const char* path) {
    const std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    auto parsed = webcpp::document::parse(text.str());
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return std::move(parsed).unsafe_value();
}

std::string_view str(const fixture_value& v) {
    return v.value_or(std::string_view());
}

/** The resource of everything the test makes. */
std::pmr::memory_resource* memory() {
    return std::pmr::get_default_resource();
}

/**
 * An answering peer connection that gives the oracle's answer, then its one candidate, each
 * posted, as a browser's arrive after the call; it opens no channel.
 */
class answering_connection final : public backend::peer_connection,
                                   public std::enable_shared_from_this<answering_connection> {
public:
    answering_connection(asio::io_context& io, std::string_view answer, std::string_view candidate)
        : io_(io), answer_(answer), candidate_(candidate) {}

    void set_handlers(handlers h) override { h_ = std::move(h); }

    std::shared_ptr<backend::data_channel> create_data_channel(
        std::string_view /*label*/) override {
        return nullptr;
    }

    void set_local_description(
        trystero::handler<void(backend::error_code, std::string_view, std::string_view)> done)
        override {
        post_in(io_, memory(), [self = shared_from_this(), done = std::move(done)]() mutable {
            done({}, "answer", self->answer_);
            post_in(self->io_, memory(), [self] {
                if (self->h_.ice_candidate) {
                    self->h_.ice_candidate(self->candidate_);
                }
            });
        });
    }

    void set_remote_description(std::string_view type, std::string_view sdp,
                                trystero::handler<void(backend::error_code)> done) override {
        remote_type = type;
        remote_sdp = sdp;
        post_in(io_, memory(), [done = std::move(done)]() mutable { done({}); });
    }

    void add_ice_candidate(std::string_view /*candidate_json*/,
                           trystero::handler<void(backend::error_code)> done) override {
        post_in(io_, memory(), [done = std::move(done)]() mutable { done({}); });
    }

    [[nodiscard]] bool has_remote_description() const override { return !remote_sdp.empty(); }

    /** No SCTP transport: the signaling never asks. */
    [[nodiscard]] std::optional<std::size_t> max_message_size() const override {
        return std::nullopt;
    }

    void close() override {}

    /** The description the signaling applied: the offer it opened. */
    std::string remote_type;
    /** Its text. */
    std::string remote_sdp;

private:
    asio::io_context& io_;
    std::string answer_;
    std::string candidate_;
    handlers h_;
};

/** The memory network, whose peer connections answer as the oracle's answering peer does. */
class answering_network final : public memory_network {
public:
    answering_network(asio::io_context& io, std::string_view answer, std::string_view candidate)
        : memory_network(io), answer_(answer), candidate_(candidate) {
        virtual_clock = true;
    }

    std::shared_ptr<backend::peer_connection> make_peer_connection(
        std::span<const backend::ice_server> /*servers*/,
        std::pmr::memory_resource* /*memory*/) override {
        auto made = std::make_shared<answering_connection>(io(), answer_, candidate_);
        connections.push_back(made);
        return made;
    }

    /** Every connection made, in order. */
    std::vector<std::shared_ptr<answering_connection>> connections;

private:
    std::string answer_;
    std::string candidate_;
};

/** The key of the oracle's room, which has no password. */
webcpp::crypto::secret<32> room_key(const fixture_value& oracle) {
    return tp::room_aes_key_without_password(str(oracle["app_id"]), str(oracle["room_id"]));
}

/** `text` with its one `from` replaced by `to`; the case fails when it holds none. */
std::string replaced(std::string_view text, std::string_view from, std::string_view to) {
    std::string out(text);
    const auto at = out.find(from);
    require(BOOST_TEST(!from.empty() && at != std::string::npos));
    out.replace(at, from.size(), to);
    return out;
}

/** A signal's text, read as a room reads it; the case fails when it is not one. */
struct read_signal_text {
    explicit read_signal_text(std::string_view text)
        : document(webcpp::document::parse(text,
                                           tp::json::protocol_limits(tp::max_parsed_signal_bytes),
                                           std::pmr::polymorphic_allocator<std::byte>(memory()))) {
        require(BOOST_TEST(document.has_value()));
        message = tp::read_signal(document->root());
        require(BOOST_TEST(message.has_value()));
    }

    decltype(webcpp::document::parse(std::string_view(), tp::json::protocol_limits(0),
                                     std::pmr::polymorphic_allocator<std::byte>())) document;
    std::optional<tp::signal_message> message;
};

/** A signal the signaling published. */
struct published_signal {
    std::size_t relay;
    std::string topic;
    std::string text;
};

/** A signaling of the oracle's room and self id, under its room key, over an answering network. */
struct answerer {
    asio::io_context io;
    answering_network net;
    std::vector<published_signal> published;
    std::vector<boost::system::error_code> failures;
    /** What the signaling's should_connect answers. */
    bool wanted = true;
    std::shared_ptr<detail::signaling> signaling;

    explicit answerer(const fixture_value& oracle)
        : net(io, str(oracle["answer_sdp"]), str(oracle["candidate"])) {
        signaling = std::make_shared<detail::signaling>(
            net, str(oracle["app_id"]), str(oracle["room_id"]), str(oracle["self_id"]),
            std::span<const backend::ice_server>(),
            detail::signaling::callbacks{
                .publish =
                    [this](std::size_t relay, std::string_view topic, std::string_view content) {
                        published.push_back({
                            .relay = relay,
                            .topic = std::string(topic),
                            .text = std::string(content),
                        });
                    },
                .connected = {},
                .failed = [this](const trystero::room_error& e) { failures.push_back(e.code); },
                .should_connect = [this](std::string_view) { return wanted; },
            },
            detail::logger(), memory());
        signaling->set_key(room_key(oracle));
    }

    /** Hands `text` to the signaling, as a relay's on the room's self topic, then runs what is
     * ready, the virtual clock standing still. */
    void receive(std::string_view text) {
        signaling->handle(0, text);
        require(BOOST_TEST(net.poll_ready()));
    }
};

/** Checks a published signal against the one 0.27.0 published: its relay, its topic, what its
 * sealed text opens to, and its bytes but for that text. */
void check_published(const fixture_value& oracle, const published_signal& published,
                     const fixture_value& signal) {
    BOOST_TEST_EQ(published.relay, 0U);
    BOOST_TEST_EQ(published.topic, str(signal["topic"]));
    const read_signal_text read(published.text);
    const auto& m = *read.message;
    const bool answer = !m.answer.empty();
    const auto sealed = answer ? m.answer : m.candidate;
    const auto aad =
        tp::signal_aad(answer ? "answer" : "candidate", str(oracle["self_id"]), m.offer_id);
    const auto plain = tp::open(room_key(oracle), sealed, std::string_view(aad));
    if (BOOST_TEST(plain.has_value())) {
        BOOST_TEST_EQ(*plain, str(signal["plaintext"]));
        BOOST_TEST_EQ(published.text, replaced(str(signal["text"]), str(signal["sealed"]), sealed));
    }
}

// For each offer, the signaling opens it, applies it to an answering connection, and publishes the
// answer, then the candidate it held, on the offerer's topic, each written as 0.27.0's answerer
// writes it: {peerId, offerId, answer} and {peerId, offerId, candidate}. The bytes are 0.27.0's,
// member for member, but for each sealed text, which opens with the AAD of its type, the answerer
// and the offer id to what 0.27.0 sealed there.
void an_answerers_signals_are_trysteros(const fixture_value& oracle) {
    const auto cases = oracle["cases"];
    BOOST_TEST_EQ(cases.size(), 2U);
    for (const auto c : cases) {
        answerer a(oracle);
        a.receive(str(c["offer"]));
        BOOST_TEST_EQ(a.net.connections.size(), 1U);
        if (!a.net.connections.empty()) {
            BOOST_TEST_EQ(a.net.connections[0]->remote_type, "offer");
            BOOST_TEST_EQ(a.net.connections[0]->remote_sdp, str(oracle["offer_sdp"]));
        }
        const auto expected = c["published"];
        BOOST_TEST_EQ(a.published.size(), expected.size());
        std::size_t i = 0;
        for (const auto signal : expected) {
            if (i < a.published.size()) {
                check_published(oracle, a.published[i], signal);
            }
            ++i;
        }
        BOOST_TEST(a.failures.empty());
    }
}

/** `text`, an object's JSON, with spaces before its closing brace, to `size` bytes. */
std::string padded(std::string_view text, std::size_t size) {
    require(BOOST_TEST(text.ends_with('}') && text.size() <= size));
    std::string out(text.substr(0, text.size() - 1));
    out.append(size - text.size(), ' ');
    out += '}';
    return out;
}

// A signal of protocol::max_parsed_signal_bytes, the oracle's first offer with spaces, is read and
// answered; one byte longer is dropped before it is read: nothing is made or published.
void a_signal_past_its_bound_is_dropped(const fixture_value& oracle) {
    const std::string offer(str(oracle["cases"][0]["offer"]));
    {
        answerer a(oracle);
        a.receive(padded(offer, tp::max_parsed_signal_bytes));
        BOOST_TEST_EQ(a.net.connections.size(), 1U);
        BOOST_TEST_EQ(a.published.size(), 2U);
    }
    {
        answerer a(oracle);
        a.receive(padded(offer, tp::max_parsed_signal_bytes + 1));
        BOOST_TEST(a.net.connections.empty());
        BOOST_TEST(a.published.empty());
        BOOST_TEST(a.failures.empty());
    }
}

// The offer of a peer its room does not want, as 0.27.0's signal handler asks shouldConnectPeer
// (a full room's refused user), is ignored: no connection, no answer, no report. The same offer
// once the peer is wanted is answered.
void an_unwanted_peers_offer_is_ignored(const fixture_value& oracle) {
    const std::string offer(str(oracle["cases"][0]["offer"]));
    answerer a(oracle);
    a.wanted = false;
    a.receive(offer);
    BOOST_TEST(a.net.connections.empty());
    BOOST_TEST(a.published.empty());
    BOOST_TEST(a.failures.empty());
    a.wanted = true;
    a.receive(offer);
    BOOST_TEST_EQ(a.net.connections.size(), 1U);
    BOOST_TEST_EQ(a.published.size(), 2U);
}

/** A relay's connection that keeps what the strategy sends it. */
class recording_socket final : public backend::websocket {
public:
    bool send(std::string_view text) override {
        sent.emplace_back(text);
        return true;
    }

    void close() override { closed = true; }

    /** Every text sent, in order. */
    std::vector<std::string> sent;
    /** Whether the strategy closed it. */
    bool closed = false;
};

/** The memory network, whose relay connections are the test's: it keeps the handlers of the last
 * one made, to hand the strategy texts as a relay's. */
class relaying_network final : public memory_network {
public:
    explicit relaying_network(asio::io_context& io) : memory_network(io) { virtual_clock = true; }

    std::shared_ptr<backend::websocket> connect(std::string_view /*url*/,
                                                backend::websocket::handlers h,
                                                std::pmr::memory_resource* /*memory*/) override {
        handlers = std::move(h);
        socket = std::make_shared<recording_socket>();
        ++connects;
        return socket;
    }

    /** How many connections were made. */
    std::size_t connects = 0;
    /** The handlers of the last connection made. */
    backend::websocket::handlers handlers;
    /** That connection. */
    std::shared_ptr<recording_socket> socket;
};

/** A relay's `["EVENT", <subscription>, <event>]` of `topic` whose content is `size` bytes in
 * all, padded with letters, under the id `id`. */
std::string relay_event(std::string_view subscription, std::string_view topic, std::string_view id,
                        std::size_t size) {
    std::pmr::string head(R"(["EVENT",)", memory());
    require(BOOST_TEST(webcpp::document::quote(subscription, head).has_value()));
    head += R"(,{"id":")";
    head += id;
    head += R"(","tags":[["x",)";
    require(BOOST_TEST(webcpp::document::quote(topic, head).has_value()));
    head += R"(]],"content":")";
    constexpr std::string_view tail = R"("}])";
    require(BOOST_TEST(head.size() + tail.size() <= size));
    std::string out(head);
    out.append(size - head.size() - tail.size(), 'a');
    out += tail;
    return out;
}

// A relay's text of protocol::nostr::max_parsed_relay_text_bytes, an event of the room's
// subscription and topic, reaches the room; one byte longer is dropped before it is read.
void a_relay_text_past_its_bound_is_dropped() {
    asio::io_context io;
    relaying_network net(io);
    const std::pmr::string url("wss://relay.test", memory());
    const std::string_view topic = "the-room-topic";
    std::vector<std::size_t> contents;
    const auto strategy = std::make_shared<detail::nostr_strategy>(
        net, std::span<const std::pmr::string>(&url, 1),
        std::pmr::vector<std::pmr::string>({std::pmr::string(topic, memory())}, memory()), topic,
        [](std::string_view /*nonce*/) { return std::pmr::string(R"({"peerId":"p"})", memory()); },
        [&contents](std::size_t, std::string_view, std::string_view content) {
            contents.push_back(content.size());
        },
        detail::logger(), memory());
    strategy->start();
    require(BOOST_TEST(net.socket != nullptr && static_cast<bool>(net.handlers.open)));
    net.handlers.open();
    require(BOOST_TEST(net.poll_ready()));
    // The subscription the strategy asked for, ["REQ", <id>, <filter>].
    require(BOOST_TEST(!net.socket->sent.empty()));
    const auto request = webcpp::document::parse(net.socket->sent.front());
    require(BOOST_TEST(request.has_value() && str(request->root()[0]) == "REQ"));
    const auto subscription = str(request->root()[1]);
    const std::size_t bound = tp::nostr::max_parsed_relay_text_bytes;
    const auto at_bound = relay_event(subscription, topic, std::string(64, '1'), bound);
    const auto past_bound = relay_event(subscription, topic, std::string(64, '2'), bound + 1);
    BOOST_TEST_EQ(at_bound.size(), bound);
    BOOST_TEST_EQ(past_bound.size(), bound + 1);
    net.handlers.message(at_bound);
    BOOST_TEST_EQ(contents.size(), 1U);
    net.handlers.message(past_bound);
    BOOST_TEST_EQ(contents.size(), 1U);
    BOOST_TEST(!net.socket->closed);
    strategy->stop();
}

/** A relay's `["EVENT", <subscription>, <event>]` of `topic` under the id `id`, its event holding
 * a member nested so that the whole text nests `depth` deep. */
std::string nested_event(std::string_view subscription, std::string_view topic, std::string_view id,
                         std::size_t depth) {
    std::pmr::string text(R"(["EVENT",)", memory());
    require(BOOST_TEST(webcpp::document::quote(subscription, text).has_value()));
    text += R"(,{"id":")";
    text += id;
    text += R"(","tags":[["x",)";
    require(BOOST_TEST(webcpp::document::quote(topic, text).has_value()));
    // The text's array and its event are two levels; the member's arrays are the rest.
    text += R"(]],"content":"c","deep":)";
    text.append(depth - 2, '[');
    text.append(depth - 2, ']');
    text += "}]";
    return std::string(text);
}

// A relay's text nested as deep as a protocol text may, protocol::json::max_protocol_depth, reaches
// the room; one level deeper is dropped as a malformed one is, before the parse recurses further.
void a_relay_text_past_its_depth_is_dropped() {
    asio::io_context io;
    relaying_network net(io);
    const std::pmr::string url("wss://relay.test", memory());
    const std::string_view topic = "the-room-topic";
    std::vector<std::string> contents;
    const auto strategy = std::make_shared<detail::nostr_strategy>(
        net, std::span<const std::pmr::string>(&url, 1),
        std::pmr::vector<std::pmr::string>({std::pmr::string(topic, memory())}, memory()), topic,
        [](std::string_view /*nonce*/) { return std::pmr::string(R"({"peerId":"p"})", memory()); },
        [&contents](std::size_t, std::string_view, std::string_view content) {
            contents.emplace_back(content);
        },
        detail::logger(), memory());
    strategy->start();
    require(BOOST_TEST(net.socket != nullptr && static_cast<bool>(net.handlers.open)));
    net.handlers.open();
    require(BOOST_TEST(net.poll_ready()));
    require(BOOST_TEST(!net.socket->sent.empty()));
    const auto request = webcpp::document::parse(net.socket->sent.front());
    require(BOOST_TEST(request.has_value() && str(request->root()[0]) == "REQ"));
    const auto subscription = str(request->root()[1]);
    const std::size_t depth = tp::json::max_protocol_depth;
    BOOST_TEST_EQ(depth, 16U);
    net.handlers.message(nested_event(subscription, topic, std::string(64, '1'), depth));
    BOOST_TEST_EQ(contents.size(), 1U);
    net.handlers.message(nested_event(subscription, topic, std::string(64, '2'), depth + 1));
    BOOST_TEST_EQ(contents.size(), 1U);
    BOOST_TEST(!net.socket->closed);
    strategy->stop();
}

/**
 * A strategy over one relay of the test's, on the virtual clock, open unless asked otherwise: it
 * subscribed and made its first announcement. It announces `{"peerId":"p","nonce":<nonce>}`, as a
 * room does, and keeps what it logs and the messages it hands on.
 */
struct relay_harness {
    explicit relay_harness(std::pmr::vector<std::pmr::string> topics, bool open = true) {
        strategy = std::make_shared<detail::nostr_strategy>(
            net, std::span<const std::pmr::string>(&url, 1), std::move(topics), announce_topic,
            [](std::string_view nonce) {
                return tp::write_signal({.peer_id = "p"}, nonce, memory());
            },
            [this](std::size_t, std::string_view topic, std::string_view) {
                heard.emplace_back(topic);
            },
            detail::logger(
                [this](std::string_view, std::string_view text) { logged.emplace_back(text); },
                memory()),
            memory());
        strategy->start();
        if (open) {
            opens();
        }
    }

    /** A strategy of one other topic and the root topic, open unless `open` is false. */
    explicit relay_harness(bool open = true)
        : relay_harness(
              std::pmr::vector<std::pmr::string>(
                  {std::pmr::string("self", memory()), std::pmr::string(announce_topic, memory())},
                  memory()),
              open) {}

    relay_harness(const relay_harness&) = delete;
    relay_harness& operator=(const relay_harness&) = delete;

    // The strategy's stop allocates the CLOSE messages it sends; a failure to allocate there ends
    // the test, as in any destructor.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~relay_harness() { strategy->stop(); }

    /** The texts of `type` the strategy sent, parsed, in order. */
    [[nodiscard]] std::vector<webcpp::document::document> sent(std::string_view type) const {
        std::vector<webcpp::document::document> out;
        for (const auto& text : net.socket->sent) {
            auto parsed = webcpp::document::parse(text);
            if (parsed.has_value() && str(parsed->root()[0]) == type) {
                out.push_back(std::move(*parsed));
            }
        }
        return out;
    }

    /** The id of the strategy's first subscription. */
    [[nodiscard]] std::string subscription() const {
        const auto requests = sent("REQ");
        require(BOOST_TEST(!requests.empty()));
        return std::string(str(requests.front().root()[1]));
    }

    /** How many events the strategy sent. */
    [[nodiscard]] std::size_t events() const { return sent("EVENT").size(); }

    /** Whether a signal published now goes out: it is the last text sent. */
    [[nodiscard]] bool publishes() const {
        const auto before = net.socket->sent.size();
        strategy->publish(0, "a-peer-topic", "probe");
        return net.socket->sent.size() == before + 1;
    }

    /** Opens the strategy's last connection, as the relay accepts it. */
    void opens() {
        require(BOOST_TEST(net.socket != nullptr && static_cast<bool>(net.handlers.open)));
        net.handlers.open();
        require(BOOST_TEST(net.poll_ready()));
    }

    /** Closes the strategy's connection, as the relay goes away, and opens the one it makes
     * next, which comes within the first reconnection bound, 3333 ms. */
    void reopens() {
        net.handlers.close();
        wait(std::chrono::milliseconds(3333));
        require(BOOST_TEST_EQ(net.connects, 2U));
        opens();
    }

    /** How many REQs the strategy sent on its connection. */
    [[nodiscard]] std::size_t requests() const { return sent("REQ").size(); }

    /** Hands the strategy `text` as the relay's. */
    void relay_says(std::string_view text) { net.handlers.message(text); }

    /** Moves the virtual clock by `by`. */
    void wait(std::chrono::milliseconds by) { require(BOOST_TEST(net.advance(by))); }

    /** Whether a logged sentence holds `part`. */
    [[nodiscard]] bool logged_with(std::string_view part) const {
        return std::ranges::any_of(logged, [part](const std::string& line) {
            return line.find(part) != std::string::npos;
        });
    }

    static constexpr std::string_view announce_topic = "the-room-topic";
    asio::io_context io;
    relaying_network net{io};
    const std::pmr::string url{"wss://relay.test", memory()};
    std::vector<std::string> logged;
    std::vector<std::string> heard;
    std::shared_ptr<detail::nostr_strategy> strategy;
};

/** A relay's OK of an event, refused with `reason`, or accepted. */
std::string ok_text(bool accepted, std::string_view reason) {
    std::pmr::string out(R"(["OK",")", memory());
    out.append(64, 'a');
    out += accepted ? R"(",true,)" : R"(",false,)";
    require(BOOST_TEST(webcpp::document::quote(reason, out).has_value()));
    out += ']';
    return std::string(out);
}

/** A relay's `[type, subscription, reason]`, or `[type, subscription]` with no reason. */
std::string relay_text(std::string_view type, std::string_view subscription,
                       std::optional<std::string_view> reason = std::nullopt) {
    std::pmr::string out("[", memory());
    require(BOOST_TEST(webcpp::document::quote(type, out).has_value()));
    out += ',';
    require(BOOST_TEST(webcpp::document::quote(subscription, out).has_value()));
    if (reason) {
        out += ',';
        require(BOOST_TEST(webcpp::document::quote(*reason, out).has_value()));
    }
    out += ']';
    return std::string(out);
}

// A strategy of 600 topics subscribes on a relay in three REQs, of 250, 250 and 100 topics, each
// under an id of its own of 64 characters, since a minute before now. Each lists the kinds of its
// topics once, in the order of their first topic, which are fewer than its topics where two share
// a kind; an event of a topic of the third reaches the room under the third's id. Stopping closes
// all three; a time before a minute after the epoch subscribes since the epoch.
void subscriptions_hold_250_topics_and_distinct_kinds() {
    std::pmr::vector<std::pmr::string> topics(memory());
    for (std::size_t i = 0; i < 600; ++i) {
        topics.emplace_back("topic-" + std::to_string(i));
    }
    const std::vector<std::string> names(topics.begin(), topics.end());
    relay_harness h(std::move(topics));
    const auto requests = h.sent("REQ");
    require(BOOST_TEST_EQ(requests.size(), 3U));
    std::set<std::string> ids;
    bool shared_kind = false;
    const auto since = h.net.unix_now() - std::chrono::seconds(60);
    for (std::size_t r = 0; r < requests.size(); ++r) {
        const auto req = requests[r].root();
        const auto id = str(req[1]);
        BOOST_TEST_EQ(id.size(), 64U);
        ids.emplace(id);
        const auto filter = req[2];
        BOOST_TEST_EQ(filter["since"].value_or(std::int64_t{-1}), since.time_since_epoch().count());
        const std::size_t first = r * 250;
        const std::size_t count = std::min<std::size_t>(250, names.size() - first);
        BOOST_TEST_EQ(filter["#x"].size(), count);
        std::vector<std::int64_t> expected;
        for (std::size_t i = 0; i < count; ++i) {
            BOOST_TEST_EQ(str(filter["#x"][i]), names[first + i]);
            const std::int64_t k = tp::nostr::topic_kind(names[first + i]);
            if (std::ranges::find(expected, k) == expected.end()) {
                expected.push_back(k);
            }
        }
        shared_kind = shared_kind || expected.size() < count;
        std::vector<std::int64_t> kinds;
        for (const auto k : filter["kinds"]) {
            kinds.push_back(k.value_or(std::int64_t{-1}));
        }
        BOOST_TEST_ALL_EQ(kinds.begin(), kinds.end(), expected.begin(), expected.end());
    }
    BOOST_TEST_EQ(ids.size(), 3U);
    BOOST_TEST(shared_kind);
    const auto third = std::string(str(requests[2].root()[1]));
    h.relay_says(relay_event(third, names[599], std::string(64, '3'), 300));
    h.relay_says(relay_event("not-ours", names[599], std::string(64, '4'), 300));
    const std::vector<std::string> heard{names[599]};
    BOOST_TEST_ALL_EQ(h.heard.begin(), h.heard.end(), heard.begin(), heard.end());
    h.strategy->stop();
    const auto closes = h.sent("CLOSE");
    BOOST_TEST_EQ(closes.size(), 3U);
    for (const auto& c : closes) {
        BOOST_TEST(ids.contains(std::string(str(c.root()[1]))));
    }
    BOOST_TEST(tp::nostr::subscribe_since(std::chrono::sys_seconds(std::chrono::seconds(30))) ==
               std::chrono::sys_seconds());
}

// Each announcement is `{"peerId":<id>,"nonce":<nonce>}`, its nonce 8 base62 characters drawn when
// it is published, so that no two of them are one event, as Trystero adds a nonce at each publish.
void announcements_carry_a_fresh_nonce() {
    relay_harness h;
    h.wait(std::chrono::milliseconds(233 + 533 + 1333));
    const auto events = h.sent("EVENT");
    require(BOOST_TEST_EQ(events.size(), 4U));
    std::set<std::string> nonces;
    for (const auto& e : events) {
        const auto content = str(e.root()[1]["content"]);
        constexpr std::string_view head = R"({"peerId":"p","nonce":")";
        BOOST_TEST(content.starts_with(head) && content.ends_with(R"("})"));
        BOOST_TEST_EQ(content.size(), head.size() + 8 + 2);
        const auto nonce = content.substr(head.size(), 8);
        BOOST_TEST(std::ranges::all_of(nonce, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }));
        nonces.emplace(nonce);
    }
    BOOST_TEST_EQ(nonces.size(), events.size());
}

// Once told to stop announcing, as a room is when it starts to leave, the strategy publishes no
// announcement more: neither at its cadence, nor when the relay asks it to replay, nor when a peer
// leaves; it still publishes a signal, and its subscription stays.
void a_strategy_that_stops_announcing_announces_nothing() {
    relay_harness h;
    h.wait(std::chrono::milliseconds(233));
    const auto events = h.events();
    BOOST_TEST_EQ(events, 2U);  // at once, and after 233 ms
    h.strategy->stop_announcing();
    h.wait(std::chrono::milliseconds(60'000));
    h.relay_says(relay_text("EOSE", h.subscription()));
    h.strategy->restart_announcements();
    BOOST_TEST_EQ(h.events(), events);
    BOOST_TEST(h.publishes());
    BOOST_TEST_EQ(h.requests(), 1U);
}

// A refused event or a closed subscription whose reason starts with "blocked:", "restricted:",
// "auth-required:" or "pow:" retires the relay: the strategy closes it, publishes nothing on it
// and never connects to it again.
void terminal_rejections_retire_a_relay() {
    for (const std::string_view prefix : {"blocked:", "restricted:", "auth-required:", "pow:"}) {
        for (const bool closed : {false, true}) {
            relay_harness h;
            const std::string reason = std::string(prefix) + " not here";
            h.relay_says(closed ? relay_text("CLOSED", h.subscription(), reason)
                                : ok_text(false, reason));
            BOOST_TEST(h.net.socket->closed);
            BOOST_TEST(h.logged_with("retiring wss://relay.test"));
            BOOST_TEST(!h.publishes());
            const auto sent = h.net.socket->sent.size();
            h.net.handlers.close();
            h.wait(std::chrono::minutes(20));
            BOOST_TEST_EQ(h.net.connects, 1U);
            BOOST_TEST_EQ(h.net.socket->sent.size(), sent);
        }
    }
    // Accepted, an event is no rejection, whatever its text.
    relay_harness h;
    h.relay_says(ok_text(true, "blocked: nothing"));
    BOOST_TEST(!h.net.socket->closed);
    BOOST_TEST(h.publishes());
}

// A rate limit cools a relay down for 60 s, in which it publishes nothing; then it is on probation,
// where another rate limit doubles the cooldown, up to 15 min, and an accepted event makes it
// healthy, so that the next rate limit cools it for 60 s again. An accepted event during a
// cooldown does not end it, and a "duplicate:" rejection neither cools it nor is logged.
void rate_limits_cool_a_relay_down_and_probation_ends_on_ok() {
    using std::chrono::milliseconds;
    relay_harness h;
    const auto cooled_for = [&h](milliseconds length) {
        h.relay_says(ok_text(false, "rate-limited: slow down"));
        BOOST_TEST(!h.publishes());
        h.wait(length - milliseconds(1));
        BOOST_TEST(!h.publishes());
        h.wait(milliseconds(1));
        BOOST_TEST(h.publishes());
    };
    cooled_for(milliseconds(60'000));
    for (const auto length : {120'000, 240'000, 480'000, 900'000, 900'000}) {
        cooled_for(milliseconds(length));
    }
    h.relay_says(ok_text(true, ""));
    cooled_for(milliseconds(60'000));
    // Healthy again, then cooled: an accepted event during the cooldown leaves it.
    h.relay_says(ok_text(true, ""));
    h.relay_says(ok_text(false, "rate-limited: slow down"));
    h.relay_says(ok_text(true, ""));
    BOOST_TEST(!h.publishes());
    h.wait(milliseconds(60'000));
    BOOST_TEST(h.publishes());
    BOOST_TEST(h.logged_with("rate-limits us"));
    h.relay_says(ok_text(true, ""));
    h.relay_says(ok_text(false, "duplicate: already have it"));
    BOOST_TEST(h.publishes());
    BOOST_TEST(!h.logged_with("duplicate"));
    h.relay_says(ok_text(false, "invalid: bad"));
    BOOST_TEST(h.logged_with("refused an event: invalid: bad"));
}

// A CLOSED of a subscription that is not the strategy's is ignored, whatever its reason: the relay
// is neither retired nor cooled, and nothing is asked again. A CLOSED of its own is asked again
// under the same id after 5333 ms, with the room's announcement.
void closed_of_another_subscription_is_ignored() {
    relay_harness h;
    const auto subscription = h.subscription();
    h.relay_says(relay_text("CLOSED", "not-ours", "blocked: you"));
    h.relay_says(relay_text("CLOSED", "not-ours", "rate-limited: you"));
    h.relay_says(relay_text("CLOSED", "not-ours", "error: gone"));
    BOOST_TEST(!h.net.socket->closed);
    BOOST_TEST(h.publishes());
    BOOST_TEST(!h.logged_with("closed our subscription"));
    h.wait(std::chrono::milliseconds(10'000));
    BOOST_TEST_EQ(h.sent("REQ").size(), 1U);

    h.relay_says(relay_text("CLOSED", subscription, "error: shutting down"));
    BOOST_TEST(h.logged_with("closed our subscription: error: shutting down"));
    h.wait(std::chrono::milliseconds(5332));
    BOOST_TEST_EQ(h.sent("REQ").size(), 1U);
    const auto events = h.events();
    h.wait(std::chrono::milliseconds(1));
    const auto requests = h.sent("REQ");
    require(BOOST_TEST_EQ(requests.size(), 2U));
    BOOST_TEST_EQ(str(requests[1].root()[1]), subscription);
    BOOST_TEST_EQ(h.events(), events + 1);
}

// An EOSE that ends the last pending subscription 1 s or more after its REQ replays the room's
// announcement at once, outside its cadence; one sooner, a second one, or one of an id not the
// strategy's replays nothing.
void slow_eose_replays_announcements() {
    {
        relay_harness h;
        h.wait(std::chrono::milliseconds(100));
        const auto events = h.events();
        h.relay_says(relay_text("EOSE", h.subscription()));
        BOOST_TEST_EQ(h.events(), events);
    }
    relay_harness h;
    h.wait(std::chrono::milliseconds(1000));
    const auto events = h.events();
    BOOST_TEST_EQ(events, 3U);  // at once, after 233 ms and after 766 ms
    h.relay_says(relay_text("EOSE", "not-ours"));
    BOOST_TEST_EQ(h.events(), events);
    h.relay_says(relay_text("EOSE", h.subscription()));
    BOOST_TEST_EQ(h.events(), events + 1);
    h.relay_says(relay_text("EOSE", h.subscription()));
    BOOST_TEST_EQ(h.events(), events + 1);
}

// A CLOSED of the strategy's own subscription is asked again after 5333 ms, then after twice the
// last delay each time, up to 60 s; the EOSE of the subscription asked again brings the delay back
// to 5333 ms.
void closed_subscriptions_are_asked_again_less_and_less_often() {
    using std::chrono::milliseconds;
    relay_harness h;
    const auto subscription = h.subscription();
    const auto asked_again_after = [&h, &subscription](milliseconds delay) {
        const auto before = h.requests();
        h.relay_says(relay_text("CLOSED", subscription, "error: try later"));
        h.wait(delay - milliseconds(1));
        BOOST_TEST_EQ(h.requests(), before);
        h.wait(milliseconds(1));
        BOOST_TEST_EQ(h.requests(), before + 1);
    };
    for (const auto delay : {5333, 10666, 21332, 42664, 60'000, 60'000}) {
        asked_again_after(milliseconds(delay));
    }
    h.relay_says(relay_text("EOSE", subscription));
    asked_again_after(milliseconds(5333));
}

// A rate-limited CLOSED cools the relay down and asks for the subscription again only once the
// 60 s cooldown ends, with the room's announcement.
void a_rate_limited_closed_waits_out_the_cooldown() {
    using std::chrono::milliseconds;
    relay_harness h;
    h.relay_says(relay_text("CLOSED", h.subscription(), "rate-limited: slow down"));
    BOOST_TEST(h.logged_with("rate-limits us, pausing it for 60 s"));
    BOOST_TEST(!h.publishes());
    h.wait(milliseconds(59'999));
    BOOST_TEST_EQ(h.requests(), 1U);
    const auto events = h.events();
    h.wait(milliseconds(1));
    BOOST_TEST_EQ(h.requests(), 2U);
    BOOST_TEST_EQ(h.events(), events + 1);
    BOOST_TEST(h.publishes());
}

// A relay that comes back is asked for the subscription again, under its id, and given the room's
// announcement once; the cadence goes on from where it was, rather than starting its warm-up over.
void a_reopened_relay_subscribes_again_and_announces_once() {
    using std::chrono::milliseconds;
    relay_harness h;
    const auto subscription = h.subscription();
    // The warm-up: at once, then after 233, 533, 1333 and 5333 ms; the next is 60 s after it.
    h.wait(milliseconds(233 + 533 + 1333 + 5333));
    BOOST_TEST_EQ(h.events(), 5U);
    h.reopens();
    const auto requests = h.sent("REQ");
    require(BOOST_TEST_EQ(requests.size(), 1U));
    BOOST_TEST_EQ(str(requests[0].root()[1]), subscription);
    BOOST_TEST_EQ(h.events(), 1U);
    h.wait(milliseconds(60'000 - 3333 - 1));
    BOOST_TEST_EQ(h.events(), 1U);
    h.wait(milliseconds(1));
    BOOST_TEST_EQ(h.events(), 2U);
}

// A relay that comes back while it cools down is asked for nothing, and given nothing, until its
// cooldown ends.
void a_relay_that_reopens_cooling_down_waits_for_the_cooldown() {
    using std::chrono::milliseconds;
    relay_harness h;
    h.relay_says(ok_text(false, "rate-limited: slow down"));
    h.reopens();
    BOOST_TEST_EQ(h.requests(), 0U);
    h.wait(milliseconds(60'000 - 3333 - 1));
    BOOST_TEST_EQ(h.requests(), 0U);
    BOOST_TEST_EQ(h.events(), 0U);
    h.wait(milliseconds(1));
    BOOST_TEST_EQ(h.requests(), 1U);
    BOOST_TEST_EQ(h.events(), 1U);
}

// Over several subscriptions, an EOSE of some of them neither ends the wait nor replays the
// announcement, and leaves the delay of the next retry where it was; the EOSE of the last one
// does both and brings the delay back to 5333 ms.
void an_eose_of_some_subscriptions_neither_syncs_nor_replays() {
    using std::chrono::milliseconds;
    std::pmr::vector<std::pmr::string> topics(memory());
    for (std::size_t i = 0; i < 600; ++i) {
        topics.emplace_back("topic-" + std::to_string(i));
    }
    relay_harness h(std::move(topics));
    std::vector<std::string> ids;
    for (const auto& req : h.sent("REQ")) {
        ids.emplace_back(str(req.root()[1]));
    }
    require(BOOST_TEST_EQ(ids.size(), 3U));
    const auto eose = [&h](std::string_view id) { h.relay_says(relay_text("EOSE", id)); };
    h.wait(milliseconds(1000));
    auto events = h.events();
    eose(ids[0]);
    eose(ids[1]);
    eose(ids[0]);
    BOOST_TEST_EQ(h.events(), events);
    // Closed, the subscriptions are asked again after 5333 ms; the next delay is 10666 ms.
    h.relay_says(relay_text("CLOSED", ids[0], "error: try later"));
    h.wait(milliseconds(5333));
    BOOST_TEST_EQ(h.requests(), 6U);
    h.wait(milliseconds(1000));
    events = h.events();
    eose(ids[0]);
    eose(ids[1]);
    BOOST_TEST_EQ(h.events(), events);
    h.relay_says(relay_text("CLOSED", ids[1], "error: try later"));
    h.wait(milliseconds(10'665));
    BOOST_TEST_EQ(h.requests(), 6U);
    h.wait(milliseconds(1));
    BOOST_TEST_EQ(h.requests(), 9U);
    // All three end, 1 s after their REQs: the announcement once, and the delay back to 5333 ms.
    h.wait(milliseconds(1000));
    events = h.events();
    for (const auto& id : ids) {
        eose(id);
    }
    BOOST_TEST_EQ(h.events(), events + 1);
    h.relay_says(relay_text("CLOSED", ids[2], "error: try later"));
    h.wait(milliseconds(5332));
    BOOST_TEST_EQ(h.requests(), 9U);
    h.wait(milliseconds(1));
    BOOST_TEST_EQ(h.requests(), 12U);
}

// The room's key is made at its first event, not with the strategy, and signs it; the strategy
// drops it when it stops.
void the_room_key_is_made_at_the_first_event_and_dropped_on_stop() {
    relay_harness h(false);
    BOOST_TEST(h.strategy->current_signer() == nullptr);
    h.opens();
    const auto events = h.sent("EVENT");
    require(BOOST_TEST_EQ(events.size(), 1U));
    const auto* key = h.strategy->current_signer();
    require(BOOST_TEST(key != nullptr));
    BOOST_TEST_EQ(str(events[0].root()[1]["pubkey"]), key->pubkey_hex());
    h.strategy->stop();
    BOOST_TEST(h.strategy->current_signer() == nullptr);
}

/** Two signalings of one room over the memory network, A (`ids[0]`, whose id is lower, so it
 * offers) and B; each publishes on the other's self topic, through a queue `deliver` empties,
 * `tamper` first rewriting what goes to A. */
struct signaling_pair {
    static constexpr std::array<std::string_view, 2> ids{
        "aPeer000000000000000",
        "bPeer000000000000000",
    };
    static constexpr std::string_view app_id = "pair-app";
    static constexpr std::string_view room_id = "pair-room";

    /** A negotiation that ended connected: its connection and the descriptions it was given. */
    struct connection {
        std::shared_ptr<backend::peer_connection> pc;
        std::shared_ptr<backend::data_channel> channel;
        std::string local_sdp;
        std::string remote_sdp;
    };

    signaling_pair() {
        net.virtual_clock = true;
        for (std::size_t i = 0; i < 2; ++i) {
            sides[i] = std::make_shared<detail::signaling>(
                net, app_id, room_id, ids[i], std::span<const backend::ice_server>(),
                detail::signaling::callbacks{
                    .publish =
                        [this, i](std::size_t, std::string_view topic, std::string_view content) {
                            const std::size_t to = 1 - i;
                            BOOST_TEST_EQ(topic, tp::peer_topic(app_id, room_id, ids[to]));
                            queue.emplace_back(to, std::string(content));
                        },
                    .connected =
                        [this, i](std::string_view, std::shared_ptr<backend::peer_connection> pc,
                                  std::shared_ptr<backend::data_channel> channel,
                                  std::string_view local_sdp, std::string_view remote_sdp) {
                            connected[i].push_back({
                                .pc = std::move(pc),
                                .channel = std::move(channel),
                                .local_sdp = std::string(local_sdp),
                                .remote_sdp = std::string(remote_sdp),
                            });
                        },
                    .failed = [](const trystero::room_error&) {},
                    .should_connect = [this, i](std::string_view) { return wanted[i]; },
                },
                detail::logger(), memory());
            sides[i]->set_key(key);
        }
    }

    /** Delivers what was published, and what that publishes, until nothing is left. */
    void deliver() {
        require(BOOST_TEST(net.poll_ready()));
        while (!queue.empty()) {
            auto [to, text] = std::move(queue.front());
            queue.pop_front();
            if (to == 0 && tamper) {
                text = tamper(text);
            }
            sides[to]->handle(0, text);
            require(BOOST_TEST(net.poll_ready()));
        }
    }

    /** A hears B's announcement, and they negotiate. */
    void announce_b() {
        sides[0]->handle(0, tp::write_signal({.peer_id = ids[1]}, {}, memory()));
        deliver();
    }

    asio::io_context io;
    memory_network net{io};
    webcpp::crypto::secret<32> key = tp::room_aes_key_without_password(app_id, room_id);
    std::array<std::shared_ptr<detail::signaling>, 2> sides;
    std::deque<std::pair<std::size_t, std::string>> queue;
    std::function<std::string(std::string)> tamper;
    std::array<std::vector<connection>, 2> connected;
    /** What each side's should_connect answers. */
    std::array<bool, 2> wanted{true, true};
};

// An offerer whose connection refuses the answer it is sent (its first line names no connection)
// starts over with a new offer once its peer's answer has expired; when that negotiation opens,
// each side is given the descriptions of the connection that opened: the one it set itself and
// the one it applied, its peer's, never the answer it refused.
void connected_carries_the_descriptions_applied() {
    signaling_pair p;
    std::string refused;
    p.tamper = [&](std::string text) {
        const read_signal_text read(text);
        const auto& m = *read.message;
        if (m.answer.empty() || !refused.empty()) {
            return text;
        }
        const auto aad = tp::signal_aad("answer", signaling_pair::ids[1], m.offer_id);
        const auto sdp = tp::open(p.key, m.answer, std::string_view(aad));
        require(BOOST_TEST(sdp.has_value()));
        refused = replaced(*sdp, "mem:", "nil:");
        const std::array<std::uint8_t, tp::gcm_iv_size> iv{9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2};
        const auto sealed = tp::seal(p.key, iv, refused, std::string_view(aad));
        require(BOOST_TEST(sealed.has_value()));
        return replaced(text, m.answer, *sealed);
    };
    std::vector<std::size_t> made;
    p.net.on_made = [&](std::size_t pc, std::span<const backend::ice_server>) {
        made.push_back(pc);
    };
    p.announce_b();
    require(BOOST_TEST(!refused.empty()));
    BOOST_TEST(p.connected[0].empty() && p.connected[1].empty());
    require(BOOST_TEST_EQ(made.size(), 2U));  // A's first offer, B's answer to it
    require(BOOST_TEST(p.net.advance(std::chrono::milliseconds(24'000))));
    p.deliver();
    p.announce_b();
    require(BOOST_TEST_EQ(p.connected[0].size(), 1U));
    require(BOOST_TEST_EQ(p.connected[1].size(), 1U));
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& c = p.connected[i].front();
        const auto pc = std::dynamic_pointer_cast<trystero::test::memory_peer_connection>(c.pc);
        require(BOOST_TEST(pc != nullptr && pc->remote_id().has_value()));
        BOOST_TEST_EQ(c.local_sdp,
                      std::string_view(trystero::test::memory_description(pc->id(), memory())));
        BOOST_TEST_EQ(c.remote_sdp, std::string_view(trystero::test::memory_description(
                                        *pc->remote_id(), memory())));
        BOOST_TEST_NE(c.remote_sdp, refused);
        // The connection that opened is a new one, not one of the first negotiation's.
        BOOST_TEST(std::ranges::find(made.begin(), made.begin() + 2, pc->id()) == made.begin() + 2);
    }
    const auto a_pc =
        std::dynamic_pointer_cast<trystero::test::memory_peer_connection>(p.connected[0][0].pc);
    const auto b_pc =
        std::dynamic_pointer_cast<trystero::test::memory_peer_connection>(p.connected[1][0].pc);
    BOOST_TEST_EQ(*a_pc->remote_id(), b_pc->id());
    BOOST_TEST_EQ(*b_pc->remote_id(), a_pc->id());
}

// An offerer whose room stops wanting its peer between its offer and the peer's answer (a full
// room's refusal of that peer's user) ends the offer the answer is for, as 0.27.0 tears down the
// offer an unwanted answer answers: its connection closes at once, and no connection opens. Once
// the peer is wanted again, a new announcement negotiates normally.
void a_refused_answer_ends_the_offer() {
    signaling_pair p;
    p.tamper = [&](std::string text) {
        const read_signal_text read(text);
        if (!read.message->answer.empty()) {
            p.wanted[0] = false;  // the room is full by the time the answer arrives
        }
        return text;
    };
    p.announce_b();
    BOOST_TEST(p.connected[0].empty());
    BOOST_TEST(p.connected[1].empty());
    // A's offer is gone; B's connection that answered it stays until its answer expires.
    BOOST_TEST_EQ(p.net.live_connections(), 1U);
    p.tamper = {};
    p.wanted[0] = true;
    require(BOOST_TEST(p.net.advance(std::chrono::milliseconds(24'000))));
    p.deliver();
    p.announce_b();
    BOOST_TEST_EQ(p.connected[0].size(), 1U);
    BOOST_TEST_EQ(p.connected[1].size(), 1U);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!BOOST_TEST_EQ(argc, 2)) {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM << "usage: signaling_test <fixtures/oracle/expected.json>\n";
        return webcpp::test::exit_status(boost::report_errors());
    }
    const auto fixture = load(argv[1]);
    if (!BOOST_TEST(fixture.has_value() && (*fixture)["answerer"].is_object())) {
        return webcpp::test::exit_status(boost::report_errors());
    }
    const auto oracle = fixture->root()["answerer"];
    an_answerers_signals_are_trysteros(oracle);
    a_signal_past_its_bound_is_dropped(oracle);
    an_unwanted_peers_offer_is_ignored(oracle);
    a_relay_text_past_its_bound_is_dropped();
    a_relay_text_past_its_depth_is_dropped();
    subscriptions_hold_250_topics_and_distinct_kinds();
    announcements_carry_a_fresh_nonce();
    a_strategy_that_stops_announcing_announces_nothing();
    terminal_rejections_retire_a_relay();
    rate_limits_cool_a_relay_down_and_probation_ends_on_ok();
    closed_of_another_subscription_is_ignored();
    slow_eose_replays_announcements();
    closed_subscriptions_are_asked_again_less_and_less_often();
    a_rate_limited_closed_waits_out_the_cooldown();
    a_reopened_relay_subscribes_again_and_announces_once();
    a_relay_that_reopens_cooling_down_waits_for_the_cooldown();
    an_eose_of_some_subscriptions_neither_syncs_nor_replays();
    the_room_key_is_made_at_the_first_event_and_dropped_on_stop();
    connected_carries_the_descriptions_applied();
    a_refused_answer_ends_the_offer();
    return webcpp::test::exit_status(boost::report_errors());
}
