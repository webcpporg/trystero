// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the signaling and the Nostr strategy alone, over the memory backend, on both targets. As
// an answerer, the signaling publishes the bytes Trystero JS 0.26.0's own signal handler published
// for the same offers, which the original's oracle wrote into fixtures/expected/expected.json, the
// test's one argument, but for each sealed text: the oracle's offers are sealed again as 0.27.0
// seals them, under the room's key with their AAD, and each sealed text the signaling publishes
// opens, with its own AAD, to the plaintext Trystero sealed there; its peer connections give the
// oracle's answer and candidate. Each text a strategy or a signaling reads is bounded: a relay's
// text or a signal at its bound is read, one byte longer is dropped.

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

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <optional>
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

/** The text Trystero JS 0.26.0 sealed `plaintext` to for the oracle; the case fails without one. */
std::string old_sealed(const fixture_value& oracle, std::string_view plaintext) {
    for (const auto entry : oracle["sealed"]) {
        if (str(entry["plaintext"]) == plaintext) {
            return std::string(str(entry["text"]));
        }
    }
    BOOST_ERROR("the oracle sealed no such plaintext");
    return {};
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

/** The oracle's offer `text`, its offer sealed again as 0.27.0 seals it: under the room key, with
 * the AAD of the offer, its sender and its offer id. */
std::string resealed_offer(const fixture_value& oracle, std::string_view text) {
    const read_signal_text read(text);
    const auto plaintext = str(oracle["sealed"][0]["plaintext"]);
    const std::array<std::uint8_t, tp::gcm_iv_size> iv{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const auto aad = tp::signal_aad("offer", read.message->peer_id, read.message->offer_id);
    const auto sealed = tp::seal(room_key(oracle), iv, plaintext, std::string_view(aad));
    require(BOOST_TEST(sealed.has_value()));
    return replaced(text, read.message->offer, *sealed);
}

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
    std::shared_ptr<detail::signaling> signaling;

    explicit answerer(const fixture_value& oracle)
        : net(io, str(oracle["answer_sdp"]), str(oracle["candidate"])) {
        signaling = std::make_shared<detail::signaling>(
            net, str(oracle["app_id"]), str(oracle["room_id"]), str(oracle["self_id"]),
            std::span<const backend::ice_server>(),
            detail::signaling::callbacks{
                .claim = {},
                .publish =
                    [this](std::size_t relay, std::string_view topic, std::string_view content) {
                        published.push_back({
                            .relay = relay,
                            .topic = std::string(topic),
                            .text = std::string(content),
                        });
                    },
                .connected = {},
                .failed = [this](std::string_view, boost::system::error_code ec,
                                 std::string_view) { failures.push_back(ec); },
            },
            detail::logger(), memory(), memory());
        signaling->set_key(room_key(oracle));
    }

    /** Hands `text` to the signaling, as a relay's on the room's self topic, then runs what is
     * ready, the virtual clock standing still. */
    void receive(std::string_view text) {
        signaling->handle(0, text);
        require(BOOST_TEST(net.poll_ready()));
    }
};

// For each offer, the signaling opens it, applies it to an answering connection, and publishes the
// answer, then the candidate it held, on the offerer's topic, each written as Trystero's answerer
// writes it: {peerId, answer, offerId} and {peerId, candidate, offerId}, its offerId last, or none
// for an offer that had none. The bytes are Trystero's, member for member, but for each sealed
// text, which opens with the AAD of its type, the answerer and the offer id to what Trystero
// sealed there.
void an_answerers_signals_are_trysteros(const fixture_value& oracle) {
    const auto cases = oracle["cases"];
    BOOST_TEST_EQ(cases.size(), 2U);
    for (const auto c : cases) {
        answerer a(oracle);
        a.receive(resealed_offer(oracle, str(c["offer"])));
        BOOST_TEST_EQ(a.net.connections.size(), 1U);
        if (!a.net.connections.empty()) {
            BOOST_TEST_EQ(a.net.connections[0]->remote_type, "offer");
            BOOST_TEST_EQ(a.net.connections[0]->remote_sdp, str(oracle["sealed"][0]["plaintext"]));
        }
        const auto expected = c["published"];
        BOOST_TEST_EQ(a.published.size(), expected.size());
        // The fixture's topics are 0.26's; the signals go to the offerer's topic as 0.27.0 names
        // it.
        const auto offer = str(c["offer"]);
        constexpr std::string_view key = R"("peerId":")";
        const auto found = offer.find(key);
        BOOST_TEST(found != std::string_view::npos);
        const auto from = found + key.size();
        const auto closing = offer.find('"', from);
        BOOST_TEST(closing != std::string_view::npos);
        const auto offerer = offer.substr(from, closing - from);
        const std::string topic = std::string(
            trystero::protocol::peer_topic(str(oracle["app_id"]), str(oracle["room_id"]), offerer));
        std::size_t i = 0;
        for (const auto signal : expected) {
            if (i < a.published.size()) {
                BOOST_TEST_EQ(a.published[i].relay, 0U);
                BOOST_TEST_EQ(a.published[i].topic, topic);
                const read_signal_text read(a.published[i].text);
                const auto& m = *read.message;
                const bool answer = !m.answer.empty();
                const auto sealed = answer ? m.answer : m.candidate;
                const auto aad = tp::signal_aad(answer ? "answer" : "candidate",
                                                str(oracle["self_id"]), m.offer_id);
                const auto plain = tp::open(room_key(oracle), sealed, std::string_view(aad));
                if (BOOST_TEST(plain.has_value())) {
                    BOOST_TEST_EQ(*plain, str(answer ? oracle["answer_sdp"] : oracle["candidate"]));
                    BOOST_TEST_EQ(
                        a.published[i].text,
                        replaced(str(signal["text"]), old_sealed(oracle, *plain), sealed));
                }
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
    const auto offer = resealed_offer(oracle, str(oracle["cases"][0]["offer"]));
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

/** A relay's connection that keeps what the strategy sends it. */
class recording_socket final : public backend::websocket {
public:
    void send(std::string_view text) override { sent.emplace_back(text); }

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
        return socket;
    }

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
        [] { return std::pmr::string(R"({"peerId":"p"})", memory()); },
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
        [] { return std::pmr::string(R"({"peerId":"p"})", memory()); },
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

}  // namespace

int main(int argc, char* argv[]) {
    if (!BOOST_TEST_EQ(argc, 2)) {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
            << "usage: signaling_test <fixtures/expected/expected.json>\n";
        return webcpp::test::exit_status(boost::report_errors());
    }
    const auto fixture = load(argv[1]);
    if (!BOOST_TEST(fixture.has_value() && (*fixture)["answerer"].is_object())) {
        return webcpp::test::exit_status(boost::report_errors());
    }
    const auto oracle = fixture->root()["answerer"];
    an_answerers_signals_are_trysteros(oracle);
    a_signal_past_its_bound_is_dropped(oracle);
    a_relay_text_past_its_bound_is_dropped();
    a_relay_text_past_its_depth_is_dropped();
    return webcpp::test::exit_status(boost::report_errors());
}
