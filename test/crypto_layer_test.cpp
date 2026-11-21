// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests the crypto layer of the port to Trystero 0.27.0 against the vectors recorded from
// 0.27.0's own code (fixtures/expected/trystero-0.27.0.json, written by oracle/record_0_27.mjs).
// The fixture's path is the test's one argument, as webcpp.run passes its input file. The
// fixture is read with webcpp::document; section(name) gives a case the named section of it.

#include <webcpp/document/document.hpp>
#include <webcpp/document/errors.hpp>
#include <webcpp/document/parse.hpp>
#include <webcpp/document/value.hpp>

#include <boost/core/lightweight_test.hpp>

#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

#include "cases.hpp"

namespace wd = webcpp::document;

namespace {

/** A value of the fixture: a read-only handle of an owned document. */
using fixture_value = wd::basic_const_value<>;

/** The fixture, which main loads before it runs a case. */
std::optional<wd::document>& fixture_document() {
    static std::optional<wd::document> document;
    return document;
}

/** The fixture, read as JSON.parse reads it. */
wd::result<wd::document> load(const char* path) {
    const std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return wd::parse(text.str());
}

/** The named top-level section of the fixture: an array of vectors, or the value itself. */
fixture_value section(std::string_view name) {
    return fixture_document()->root()[name];
}

void fixture_is_trystero_0_27_0() {
    BOOST_TEST_EQ(section("trystero").value_or(std::string_view()), std::string_view("0.27.0"));
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!BOOST_TEST_EQ(argc, 2)) {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
            << "usage: crypto_layer_test <fixtures/expected/trystero-0.27.0.json>\n";
        return boost::report_errors();
    }
    auto loaded = load(argv[1]);
    if (!BOOST_TEST(loaded.has_value())) {
        return boost::report_errors();
    }
    fixture_document().emplace(std::move(*loaded));
    webcpp::test::run_case(fixture_is_trystero_0_27_0);
    return boost::report_errors();
}
