// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Tests a room's users and their peers alone, as 0.27.0's room counts them: a user joins with its
// first peer and leaves with its last, and a peer belongs to one user.

#include <webcpp/trystero/detail/user_table.hpp>

#include <boost/core/lightweight_test.hpp>
#include <webcpp/test/exit_status.hpp>

#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "cases.hpp"

namespace trystero = webcpp::trystero;

namespace {

std::vector<std::string> peers_of(const trystero::detail::user_table& t, std::string_view user) {
    const auto peers = t.peers_of(user);
    return {peers.begin(), peers.end()};
}

void first_peer_joins_the_user() {
    trystero::detail::user_table t(std::pmr::new_delete_resource());
    BOOST_TEST(t.add("p1", "u"));
    BOOST_TEST(!t.add("p2", "u"));
    BOOST_TEST(t.add("p3", "v"));
    const std::vector<std::string> both{"p1", "p2"};
    const auto of_u = peers_of(t, "u");
    BOOST_TEST_ALL_EQ(of_u.begin(), of_u.end(), both.begin(), both.end());
    BOOST_TEST_EQ(peers_of(t, "v").size(), 1U);
    BOOST_TEST(peers_of(t, "w").empty());
    BOOST_TEST_EQ(t.user_of("p2").value_or(""), "u");
    BOOST_TEST_EQ(t.user_of("p3").value_or(""), "v");
    BOOST_TEST(!t.user_of("p4"));
    BOOST_TEST_EQ(t.users().size(), 2U);
}

void last_peer_leaves_the_user() {
    trystero::detail::user_table t(std::pmr::new_delete_resource());
    t.add("p1", "u");
    t.add("p2", "u");
    BOOST_TEST(!t.remove("p1"));
    BOOST_TEST(!t.user_of("p1"));
    const std::vector<std::string> left{"p2"};
    const auto of_u = peers_of(t, "u");
    BOOST_TEST_ALL_EQ(of_u.begin(), of_u.end(), left.begin(), left.end());
    const auto gone = t.remove("p2");
    BOOST_TEST_EQ(gone.value_or("none"), "u");
    BOOST_TEST(peers_of(t, "u").empty());
    BOOST_TEST(t.users().empty());
    BOOST_TEST(!t.remove("p2"));   // a peer not in the table makes no user leave
    BOOST_TEST(t.add("p2", "u"));  // and a user that left joins again with its next peer
}

void a_peer_moves_nowhere() {
    trystero::detail::user_table t(std::pmr::new_delete_resource());
    BOOST_TEST(t.add("p1", "u"));
    BOOST_TEST(!t.add("p1", "v"));  // refused: p1 is u's
    BOOST_TEST(!t.add("p1", "u"));
    BOOST_TEST_EQ(t.user_of("p1").value_or(""), "u");
    BOOST_TEST(peers_of(t, "v").empty());
    BOOST_TEST_EQ(peers_of(t, "u").size(), 1U);
    BOOST_TEST_EQ(t.users().size(), 1U);
    t.clear();
    BOOST_TEST(t.users().empty());
    BOOST_TEST(!t.user_of("p1"));
}

}  // namespace

int main() {
    // Each case in a frame of its own, which main's never holds (cases.hpp).
    using webcpp::test::run_case;
    run_case(first_peer_joins_the_user);
    run_case(last_peer_leaves_the_user);
    run_case(a_peer_moves_nowhere);
    return webcpp::test::exit_status(boost::report_errors());
}
