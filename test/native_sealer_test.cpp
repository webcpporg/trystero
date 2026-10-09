// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the native sealer, OpenSSL's AES-GCM, against text Trystero JS sealed with WebCrypto,
// which the original's oracle wrote into fixtures/expected/expected.json. The fixture's path is the
// test's one argument.

#include <webcpp/trystero/native/sealer.hpp>
#include <webcpp/trystero/protocol/signal.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/core/lightweight_test.hpp>
#include <boost/json.hpp>

#include <fstream>
#include <memory_resource>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "require.hpp"

namespace json = boost::json;
namespace tp = webcpp::trystero::protocol;
namespace trystero = webcpp::trystero;

using webcpp::test::require;

namespace {

json::value load(const char* path) {
    const std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return json::parse(text.str());
}

struct outcome {
    boost::system::error_code ec;
    std::string text;
};

/** Runs one sealer operation to its completion, which the sealer posts to the executor. */
template <class Operation>
outcome run(boost::asio::io_context& io, Operation operation) {
    std::optional<outcome> result;
    operation([&](boost::system::error_code ec, std::string_view text) {
        result = outcome{.ec = ec, .text = std::string(text)};
    });
    io.restart();
    io.poll();  // the sealer works synchronously: its completion is already posted
    require(BOOST_TEST(result.has_value()));
    return *result;
}

void opens_what_trystero_js_sealed(const json::object& sealed) {
    boost::asio::io_context io;
    trystero::native::sealer sealer(io.get_executor(), tp::seal_key("secret", "my-app", "room-1"),
                                    std::pmr::get_default_resource());
    const auto opened =
        run(io, [&](auto done) { sealer.open(sealed.at("text").as_string(), done); });
    BOOST_TEST(!opened.ec);
    BOOST_TEST_EQ(opened.text, std::string_view(sealed.at("plaintext").as_string()));
}

void seals_in_trystero_form_and_opens_it_back() {
    boost::asio::io_context io;
    trystero::native::sealer sealer(io.get_executor(), tp::seal_key("", "app", "room"),
                                    std::pmr::get_default_resource());
    const std::string plaintext =
        R"({"candidate":"candidate:1 1 udp 2122260223 192.0.2.1 54321 typ host"})";
    const auto sealed = run(io, [&](auto done) { sealer.seal(plaintext, done); });
    BOOST_TEST(!sealed.ec);
    const auto parts = tp::parse_sealed(sealed.text);
    if (!BOOST_TEST(parts.has_value())) {
        return;
    }
    BOOST_TEST_EQ(parts->iv.size(), tp::seal_iv_size);
    // The GCM tag follows, as WebCrypto writes it.
    BOOST_TEST_EQ(parts->ciphertext.size(), plaintext.size() + 16);
    const auto opened = run(io, [&](auto done) { sealer.open(sealed.text, done); });
    BOOST_TEST(!opened.ec);
    BOOST_TEST_EQ(opened.text, plaintext);
}

void another_key_is_a_wrong_password(const json::object& sealed) {
    boost::asio::io_context io;
    trystero::native::sealer sealer(io.get_executor(), tp::seal_key("secret", "my-app", "room-1"),
                                    std::pmr::get_default_resource());
    trystero::native::sealer intruder(io.get_executor(), tp::seal_key("guess", "my-app", "room-1"),
                                      std::pmr::get_default_resource());
    const auto opened =
        run(io, [&](auto done) { intruder.open(sealed.at("text").as_string(), done); });
    BOOST_TEST(opened.ec == trystero::error::wrong_password);
    const auto garbage = run(io, [&](auto done) { sealer.open("not sealed text", done); });
    BOOST_TEST(garbage.ec == trystero::error::wrong_password);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!BOOST_TEST_EQ(argc, 2)) {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
            << "usage: native_sealer_test <fixtures/expected/expected.json>\n";
        return boost::report_errors();
    }
    const auto loaded = load(argv[1]);
    const auto& sealed = loaded.at("sealed").as_object();
    opens_what_trystero_js_sealed(sealed);
    seals_in_trystero_form_and_opens_it_back();
    another_key_is_a_wrong_password(sealed);
    return boost::report_errors();
}
