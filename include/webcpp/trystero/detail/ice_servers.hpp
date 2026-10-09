// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

#ifndef WEBCPP_TRYSTERO_DETAIL_ICE_SERVERS_HPP
#define WEBCPP_TRYSTERO_DETAIL_ICE_SERVERS_HPP

#include <webcpp/trystero/config.hpp>

#include <webcpp/trystero/backend.hpp>

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

namespace webcpp::trystero::detail {

/** Whether a URL's scheme is `name`, without regard to case, as a URL parser reads it. */
inline bool same_scheme(std::string_view scheme, std::string_view name) {
    return std::ranges::equal(scheme, name, [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
    });
}

/** Writes the scheme of a URL in lowercase, as a URL parser does, and as the backends read it. */
inline void lowercase_scheme(std::pmr::string& url) {
    const auto colon = url.find(':');
    for (std::size_t i = 0; i < colon && i < url.size(); ++i) {
        if (url[i] >= 'A' && url[i] <= 'Z') {
            url[i] = static_cast<char>(url[i] - 'A' + 'a');
        }
    }
}

/** Why the port of an ICE server URL cannot be used, or an empty view when it can: 1 to 65535. */
inline std::string_view ice_port_fault(std::string_view port) {
    if (port.empty() || port.size() > 5 ||
        port.find_first_not_of("0123456789") != std::string_view::npos) {
        return "has a port that is not a number";
    }
    std::size_t value = 0;
    for (const char c : port) {
        value = (value * 10) + static_cast<std::size_t>(c - '0');
    }
    return value == 0 || value > 65535 ? "has a port outside 1 to 65535" : std::string_view();
}

/** Why the `host[:port]` of an ICE server URL cannot be used, or an empty view when it can. */
inline std::string_view ice_authority_fault(std::string_view authority) {
    std::string_view host = authority;
    std::string_view port;
    bool has_port = false;
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return "has an IPv6 address without its closing bracket";
        }
        host = authority.substr(0, close + 1);
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty() && !after.starts_with(':')) {
            return "has text after its IPv6 address";
        }
        has_port = !after.empty();
        port = has_port ? after.substr(1) : std::string_view();
    } else if (const auto last = authority.rfind(':'); last != std::string_view::npos) {
        host = authority.substr(0, last);
        port = authority.substr(last + 1);
        has_port = true;
    }
    if (host.empty() || host == "[]") {
        return "has no host";
    }
    if (!host.starts_with('[') && host.find_first_of("/?#@:[] ") != std::string_view::npos) {
        return "has a host with a character no host has";
    }
    return has_port ? ice_port_fault(port) : std::string_view();
}

/**
 * Why an ICE server URL cannot be used, or an empty view when it can: `scheme:host[:port]`, the
 * scheme `stun`, `stuns`, `turn` or `turns` (RFC 7064, RFC 7065), the host a name or an address
 * (an IPv6 one in brackets), the port 1 to 65535, and for `turn` and `turns` an optional
 * `?transport=udp` or `?transport=tcp`.
 *
 * The scheme is compared without regard to case, as a URL parser lowercases it.
 *
 * Tip: these are the URLs WebRTC's "validate an ICE server" accepts; `RTCPeerConnection` throws
 * for any other, and a room checks its servers when it joins so that no backend meets one.
 */
inline std::string_view ice_url_fault(std::string_view url) {
    const auto colon = url.find(':');
    if (colon == std::string_view::npos) {
        return "has no scheme";
    }
    const std::string_view scheme = url.substr(0, colon);
    const bool turn = same_scheme(scheme, "turn") || same_scheme(scheme, "turns");
    if (!turn && !same_scheme(scheme, "stun") && !same_scheme(scheme, "stuns")) {
        return "is not stun:, stuns:, turn: or turns:";
    }
    std::string_view authority = url.substr(colon + 1);
    if (const auto question = authority.find('?'); question != std::string_view::npos) {
        const std::string_view query = authority.substr(question + 1);
        if (!turn || (query != "transport=udp" && query != "transport=tcp")) {
            return "has a query other than turn's transport=udp or transport=tcp";
        }
        authority = authority.substr(0, question);
    }
    return ice_authority_fault(authority);
}

/** Why an ICE server cannot be used, or an empty view when it can: a URL it names cannot, or a
 * TURN server has no user name or no credential, which `RTCPeerConnection` requires. */
inline std::string_view ice_server_fault(const backend::ice_server& server) {
    if (server.urls.empty()) {
        return "names no URL";
    }
    bool turn = false;
    for (const auto& url : server.urls) {
        if (const auto fault = ice_url_fault(url); !fault.empty()) {
            return fault;
        }
        turn = turn || same_scheme(std::string_view(url).substr(0, 4), "turn");
    }
    if (turn && (server.username.empty() || server.credential.empty())) {
        return "is a TURN server without a user name and a credential";
    }
    return {};
}

}  // namespace webcpp::trystero::detail

#endif
