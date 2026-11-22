// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests an app's key records and verified signatures, detail::identity_cache, against 0.27.0's
// `getVerifiedPubKeyRecord`, `verifyWithP256`, `verifyPeerIdKey` and `verifyUserIdKey`, with the
// vectors recorded from its own code (fixtures/expected/trystero-0.27.0.json), the test's one
// argument.

#include <webcpp/trystero/detail/identity_cache.hpp>
#include <webcpp/trystero/identity.hpp>
#include <webcpp/trystero/protocol/ecdsa.hpp>
#include <webcpp/trystero/protocol/identity.hpp>
#include <webcpp/trystero/protocol/ids.hpp>
#include <webcpp/trystero/protocol/text.hpp>

#include <webcpp/document/document.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/value.hpp>

#include <boost/core/lightweight_test.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cases.hpp"

namespace wd = webcpp::document;
namespace tp = webcpp::trystero::protocol;
namespace td = webcpp::trystero::detail;

namespace {

/** A value of the fixture: a read-only handle of an owned document. */
using fixture_value = wd::basic_const_value<>;

/** The fixture, which main loads before it runs a case. */
std::optional<wd::document>& fixture_document() {
    static std::optional<wd::document> document;
    return document;
}

/** The named top-level section of the fixture. */
fixture_value section(std::string_view name) {
    return fixture_document()->root()[name];
}

/** The text of a fixture value. */
std::string_view str(const fixture_value& v) {
    return v.value_or(std::string_view());
}

/** The bytes of a fixture's hexadecimal text. */
tp::bytes bytes_of(const fixture_value& v) {
    const auto decoded = tp::from_hex(str(v));
    BOOST_TEST(decoded.has_value());
    return decoded.value_or(tp::bytes());
}

/** The bytes of a text, as a span. */
std::span<const std::uint8_t> bytes_view(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

/** The endpoint key of a scalar, with its compact text, made through trystero's P-256 seam. */
std::optional<td::endpoint_identity> key_of(std::span<const std::uint8_t> scalar) {
    auto key = td::make_endpoint_identity(scalar, std::pmr::get_default_resource());
    BOOST_TEST(key.has_value());
    return key;
}

/** The endpoint key whose scalar is the big-endian `n`, which is in [1, n - 1]. */
std::optional<td::endpoint_identity> key_number(std::uint32_t n) {
    std::array<std::uint8_t, 32> scalar{};
    for (std::size_t i = 0; i < 4; ++i) {
        scalar[31 - i] = static_cast<std::uint8_t>(n >> (8 * i));
    }
    return key_of(scalar);
}

/** The endpoint key of the fixture's `p256_keys` entry labelled `label`. */
std::optional<td::endpoint_identity> fixture_key(std::string_view label) {
    for (const auto entry : section("p256_keys")) {
        if (str(entry["label"]) == label) {
            const auto scalar = tp::from_base64url(str(entry["d"]));
            if (!BOOST_TEST(scalar.has_value())) {
                return std::nullopt;
            }
            return key_of(*scalar);
        }
    }
    BOOST_ERROR("p256_keys has no such label");
    return std::nullopt;
}

/** The bound of the fixture's `identity_limits` named `name`. */
std::size_t limit(std::string_view name) {
    return static_cast<std::size_t>(section("identity_limits")[name].value_or(std::int64_t{-1}));
}

void limits_are_0_27_0s() {
    BOOST_TEST_EQ(td::identity_cache::max_records, limit("pub_key_records"));
    BOOST_TEST_EQ(td::identity_cache::max_signatures, limit("signature_cache_entries"));
    BOOST_TEST_EQ(td::identity_cache::max_signature_bytes, limit("signature_cache_bytes"));
    BOOST_TEST_EQ(td::identity_cache::max_cached_payload_bytes,
                  limit("signature_cache_payload_bytes"));
}

void records_are_evicted_least_recently_used_at_4096() {
    td::identity_cache cache(std::pmr::get_default_resource());
    std::vector<std::pmr::string> compacts;
    for (std::uint32_t n = 1; n <= td::identity_cache::max_records + 1; ++n) {
        const auto key = key_number(n);
        if (!key) {
            return;
        }
        compacts.push_back(key->compact);
    }
    for (std::size_t i = 0; i < td::identity_cache::max_records; ++i) {
        const auto* rec = cache.record(compacts[i]);
        if (!BOOST_TEST(rec != nullptr)) {
            return;
        }
        BOOST_TEST_EQ(rec->compact, compacts[i]);
        BOOST_TEST_EQ(rec->peer_id, tp::derive_peer_id(compacts[i]));
        BOOST_TEST_EQ(rec->user_id, tp::derive_user_id(compacts[i]));
    }
    BOOST_TEST_EQ(cache.records(), td::identity_cache::max_records);
    // A record asked for again is the same, and becomes the most recently used.
    const auto* first = cache.record(compacts[0]);
    BOOST_TEST(first != nullptr && first->compact == compacts[0]);
    BOOST_TEST_EQ(cache.records(), td::identity_cache::max_records);
    // One more evicts the least recently used: the second, not the first.
    BOOST_TEST(cache.record(compacts.back()) != nullptr);
    BOOST_TEST_EQ(cache.records(), td::identity_cache::max_records);
    BOOST_TEST(cache.holds_record(compacts[0]));
    BOOST_TEST(!cache.holds_record(compacts[1]));
    BOOST_TEST(cache.holds_record(compacts[2]));
    BOOST_TEST(cache.holds_record(compacts.back()));
    // An evicted record is made again on demand.
    BOOST_TEST(cache.record(compacts[1]) != nullptr);
    BOOST_TEST(!cache.holds_record(compacts[2]));
}

void an_off_curve_key_has_no_record() {
    td::identity_cache cache(std::pmr::get_default_resource());
    const auto key = fixture_key("endpoint-a");
    if (!key) {
        return;
    }
    // (0, 0) and endpoint-a's x with its y changed: compact texts, off the curve.
    const std::array<std::uint8_t, 32> zeros{};
    const std::pmr::string origin = tp::to_base64url(zeros) + "." + tp::to_base64url(zeros);
    const auto point = tp::parse_compact_public_key(key->compact);
    if (!BOOST_TEST(point.has_value())) {
        return;
    }
    auto moved = *point;
    moved[64] ^= 1U;
    const auto off_curve = tp::compact_public_key(moved);
    for (const std::string_view refused :
         {std::string_view(origin), std::string_view(off_curve), std::string_view("not a key"),
          std::string_view(key->compact).substr(1), std::string_view()}) {
        BOOST_TEST(cache.record(refused) == nullptr);
        BOOST_TEST(!cache.verify_peer_id(tp::derive_peer_id(refused), refused));
        BOOST_TEST(!cache.verify_user_id(tp::derive_user_id(refused), refused));
    }
    const auto signature = tp::sign_p256(key->key, bytes_view("payload"));
    if (BOOST_TEST(signature.has_value())) {
        BOOST_TEST(!cache.verify(off_curve, *signature, bytes_view("payload")));
    }
    // A refused text is not remembered.
    BOOST_TEST_EQ(cache.records(), 0U);
    BOOST_TEST(cache.record(key->compact) != nullptr);
    BOOST_TEST_EQ(cache.records(), 1U);
}

void a_cached_signature_answers_for_its_payload_only() {
    td::identity_cache cache(std::pmr::get_default_resource());
    const auto key = fixture_key("endpoint-a");
    const auto other = fixture_key("endpoint-b");
    if (!key || !other) {
        return;
    }
    const auto signature = tp::sign_p256(key->key, bytes_view("payload"));
    if (!BOOST_TEST(signature.has_value())) {
        return;
    }
    BOOST_TEST(!cache.holds_signature(key->compact, *signature));
    BOOST_TEST(cache.verify(key->compact, *signature, bytes_view("payload")));
    BOOST_TEST(cache.holds_signature(key->compact, *signature));
    BOOST_TEST_EQ(cache.signatures(), 1U);
    BOOST_TEST_EQ(cache.signature_bytes(), 7U);
    // The cached signature answers for its payload, and for no other.
    BOOST_TEST(cache.verify(key->compact, *signature, bytes_view("payload")));
    BOOST_TEST(!cache.verify(key->compact, *signature, bytes_view("payloaD")));
    BOOST_TEST(!cache.verify(key->compact, *signature, bytes_view("payload!")));
    BOOST_TEST(!cache.verify(key->compact, *signature, {}));
    // Nor for another key.
    BOOST_TEST(!cache.verify(other->compact, *signature, bytes_view("payload")));
    BOOST_TEST(!cache.holds_signature(other->compact, *signature));
    BOOST_TEST_EQ(cache.signatures(), 1U);
    BOOST_TEST_EQ(cache.signature_bytes(), 7U);
    // A signature of another length is refused at once.
    BOOST_TEST(!cache.verify(key->compact, std::span(*signature).first(63), bytes_view("payload")));
    // The fixture's signatures, WebCrypto's, verify by their keys and are kept.
    const auto proof = section("user_proof");
    const auto proof_signature = tp::from_base64url(str(proof["proof"]));
    if (BOOST_TEST(proof_signature.has_value())) {
        BOOST_TEST(cache.verify(str(proof["user_compact"]), *proof_signature,
                                bytes_view(str(proof["payload"]))));
        BOOST_TEST(cache.holds_signature(str(proof["user_compact"]), *proof_signature));
        BOOST_TEST_EQ(cache.signatures(), 2U);
    }
}

void payloads_over_4096_bytes_are_not_cached() {
    td::identity_cache cache(std::pmr::get_default_resource());
    const auto key = fixture_key("endpoint-a");
    if (!key) {
        return;
    }
    std::vector<std::uint8_t> payload(td::identity_cache::max_cached_payload_bytes + 1, 0x5a);
    const auto long_signature = tp::sign_p256(key->key, payload);
    if (!BOOST_TEST(long_signature.has_value())) {
        return;
    }
    BOOST_TEST(cache.verify(key->compact, *long_signature, payload));
    BOOST_TEST(!cache.holds_signature(key->compact, *long_signature));
    BOOST_TEST_EQ(cache.signatures(), 0U);
    payload.back() ^= 1U;
    BOOST_TEST(!cache.verify(key->compact, *long_signature, payload));
    // 4096 bytes are kept.
    payload.pop_back();
    const auto signature = tp::sign_p256(key->key, payload);
    if (!BOOST_TEST(signature.has_value())) {
        return;
    }
    BOOST_TEST(cache.verify(key->compact, *signature, payload));
    BOOST_TEST(cache.holds_signature(key->compact, *signature));
    BOOST_TEST_EQ(cache.signature_bytes(), td::identity_cache::max_cached_payload_bytes);
}

void the_cache_holds_at_most_8192_signatures_and_2_mib() {
    const auto key = fixture_key("endpoint-a");
    if (!key) {
        return;
    }
    // By bytes: 512 payloads of 4096 bytes are 2 MiB; the 513th evicts the least recently used.
    {
        td::identity_cache cache(std::pmr::get_default_resource());
        constexpr std::size_t fit =
            td::identity_cache::max_signature_bytes / td::identity_cache::max_cached_payload_bytes;
        std::vector<std::vector<std::uint8_t>> payloads;
        std::vector<tp::p256_signature> signatures;
        for (std::size_t i = 0; i <= fit; ++i) {
            std::vector<std::uint8_t> payload(td::identity_cache::max_cached_payload_bytes,
                                              static_cast<std::uint8_t>(i));
            payload[0] = static_cast<std::uint8_t>(i >> 8U);
            const auto signature = tp::sign_p256(key->key, payload);
            if (!BOOST_TEST(signature.has_value())) {
                return;
            }
            payloads.push_back(std::move(payload));
            signatures.push_back(*signature);
        }
        for (std::size_t i = 0; i < fit; ++i) {
            BOOST_TEST(cache.verify(key->compact, signatures[i], payloads[i]));
        }
        BOOST_TEST_EQ(cache.signatures(), fit);
        BOOST_TEST_EQ(cache.signature_bytes(), td::identity_cache::max_signature_bytes);
        // The first, used again, stays; the second leaves for the last.
        BOOST_TEST(cache.verify(key->compact, signatures[0], payloads[0]));
        BOOST_TEST(cache.verify(key->compact, signatures[fit], payloads[fit]));
        BOOST_TEST_EQ(cache.signatures(), fit);
        BOOST_TEST_EQ(cache.signature_bytes(), td::identity_cache::max_signature_bytes);
        BOOST_TEST(cache.holds_signature(key->compact, signatures[0]));
        BOOST_TEST(!cache.holds_signature(key->compact, signatures[1]));
        BOOST_TEST(cache.holds_signature(key->compact, signatures[2]));
        BOOST_TEST(cache.holds_signature(key->compact, signatures[fit]));
        // A smaller payload evicts the least recently used only when it does not fit.
        const auto small = tp::sign_p256(key->key, bytes_view("small"));
        if (BOOST_TEST(small.has_value())) {
            BOOST_TEST(cache.verify(key->compact, *small, bytes_view("small")));
            BOOST_TEST_EQ(cache.signatures(), fit);
            BOOST_TEST_EQ(cache.signature_bytes(),
                          td::identity_cache::max_signature_bytes -
                              td::identity_cache::max_cached_payload_bytes + 5);
            BOOST_TEST(!cache.holds_signature(key->compact, signatures[2]));
        }
    }
    // By entries: 8192 signatures of short payloads; the 8193rd evicts the least recently used.
    {
        td::identity_cache cache(std::pmr::get_default_resource());
        std::vector<std::string> payloads;
        std::vector<tp::p256_signature> signatures;
        for (std::size_t i = 0; i <= td::identity_cache::max_signatures; ++i) {
            payloads.push_back("payload " + std::to_string(i));
            const auto signature = tp::sign_p256(key->key, bytes_view(payloads.back()));
            if (!BOOST_TEST(signature.has_value())) {
                return;
            }
            signatures.push_back(*signature);
        }
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < td::identity_cache::max_signatures; ++i) {
            BOOST_TEST(cache.verify(key->compact, signatures[i], bytes_view(payloads[i])));
            bytes += payloads[i].size();
        }
        BOOST_TEST_EQ(cache.signatures(), td::identity_cache::max_signatures);
        BOOST_TEST_EQ(cache.signature_bytes(), bytes);
        BOOST_TEST(cache.verify(key->compact, signatures[0], bytes_view(payloads[0])));
        BOOST_TEST(cache.verify(key->compact, signatures.back(), bytes_view(payloads.back())));
        BOOST_TEST_EQ(cache.signatures(), td::identity_cache::max_signatures);
        BOOST_TEST_EQ(cache.signature_bytes(), bytes - payloads[1].size() + payloads.back().size());
        BOOST_TEST(cache.holds_signature(key->compact, signatures[0]));
        BOOST_TEST(!cache.holds_signature(key->compact, signatures[1]));
        BOOST_TEST(cache.holds_signature(key->compact, signatures.back()));
    }
}

void a_high_s_signature_is_refused_before_verification() {
    const auto proof = section("user_proof");
    const auto compact = str(proof["user_compact"]);
    const auto payload = bytes_view(str(proof["payload"]));
    std::optional<tp::bytes> low;
    std::optional<tp::bytes> high;
    for (const auto c : section("low_s")) {
        if (str(c["case"]) == "signed") {
            low = bytes_of(c["signature"]);
        } else if (str(c["case"]) == "its high form") {
            high = bytes_of(c["signature"]);
        }
    }
    if (!BOOST_TEST(low.has_value()) || !BOOST_TEST(high.has_value())) {
        return;
    }
    // The high form of a signature that verifies is refused, and never kept.
    td::identity_cache cache(std::pmr::get_default_resource());
    BOOST_TEST(!cache.verify(compact, *high, payload));
    BOOST_TEST(!cache.holds_signature(compact, *high));
    BOOST_TEST_EQ(cache.signatures(), 0U);
    // Its low form verifies and is kept; the high form is refused still.
    BOOST_TEST(cache.verify(compact, *low, payload));
    BOOST_TEST(!cache.verify(compact, *high, payload));
    BOOST_TEST(!cache.holds_signature(compact, *high));
    BOOST_TEST_EQ(cache.signatures(), 1U);
    // Every case of the fixture verifies as 0.27.0's verifyWithP256 says, by a fresh cache and by
    // this one.
    for (const auto c : section("low_s")) {
        const auto signature = bytes_of(c["signature"]);
        td::identity_cache fresh(std::pmr::get_default_resource());
        BOOST_TEST_EQ(fresh.verify(compact, signature, payload), c["verifies"].value_or(true));
        BOOST_TEST_EQ(cache.verify(compact, signature, payload), c["verifies"].value_or(true));
    }
    BOOST_TEST_EQ(cache.signatures(), 1U);
}

void ids_are_checked_against_their_keys() {
    std::size_t keys = 0;
    for (const auto entry : section("p256_keys")) {
        const auto compact = str(entry["compact"]);
        const auto peer_id = str(entry["peer_id"]);
        const auto user_id = str(entry["user_id"]);
        td::identity_cache cache(std::pmr::get_default_resource());
        // Ids that are not ids are refused before any lookup.
        BOOST_TEST(!cache.verify_peer_id("", compact));
        BOOST_TEST(!cache.verify_user_id(user_id.substr(1), compact));
        BOOST_TEST(!cache.verify_user_id(std::string(user_id) + "0", compact));
        BOOST_TEST(!cache.verify_user_id(std::string(user_id.substr(1)) + "-", compact));
        BOOST_TEST(!cache.verify_user_id(std::string(user_id.substr(1)) + "\xc3", compact));
        BOOST_TEST_EQ(cache.records(), 0U);
        // Each id is its own key's.
        BOOST_TEST(cache.verify_peer_id(peer_id, compact));
        BOOST_TEST(cache.verify_user_id(user_id, compact));
        BOOST_TEST(!cache.verify_peer_id(user_id, compact));
        BOOST_TEST(!cache.verify_user_id(std::string(peer_id) + "00", compact));
        BOOST_TEST(!cache.verify_peer_id(peer_id.substr(1), compact));
        BOOST_TEST_EQ(cache.records(), 1U);
        // And no other key's.
        for (const auto other : section("p256_keys")) {
            if (str(other["compact"]) != compact) {
                BOOST_TEST(!cache.verify_peer_id(peer_id, str(other["compact"])));
                BOOST_TEST(!cache.verify_user_id(user_id, str(other["compact"])));
            }
        }
        ++keys;
    }
    BOOST_TEST_GE(keys, 2U);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!BOOST_TEST_EQ(argc, 2)) {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
            << "usage: identity_cache_test <fixtures/expected/trystero-0.27.0.json>\n";
        return boost::report_errors();
    }
    const std::ifstream file(argv[1]);
    std::stringstream text;
    text << file.rdbuf();
    auto loaded = wd::parse(text.str());
    if (!BOOST_TEST(loaded.has_value())) {
        return boost::report_errors();
    }
    fixture_document().emplace(std::move(*loaded));
    webcpp::test::run_case(limits_are_0_27_0s);
    webcpp::test::run_case(records_are_evicted_least_recently_used_at_4096);
    webcpp::test::run_case(an_off_curve_key_has_no_record);
    webcpp::test::run_case(a_cached_signature_answers_for_its_payload_only);
    webcpp::test::run_case(payloads_over_4096_bytes_are_not_cached);
    webcpp::test::run_case(the_cache_holds_at_most_8192_signatures_and_2_mib);
    webcpp::test::run_case(a_high_s_signature_is_refused_before_verification);
    webcpp::test::run_case(ids_are_checked_against_their_keys);
    return boost::report_errors();
}
