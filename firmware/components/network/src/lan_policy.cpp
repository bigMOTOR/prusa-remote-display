// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "network/lan.hpp"

#include <cstdio>

namespace prd::network::lan_policy {
namespace {

[[nodiscard]] bool is_digit(const char chr) {
    return chr >= '0' && chr <= '9';
}

// Parses one decimal octet, leaving `pos` one past it.
//
// Rejects a leading zero on a multi-digit number. `010` is ten to this parser and eight to some
// resolvers, and a value that means two things is a value the policy cannot be stated over.
// One parsed octet, and where the parse stopped.
//
// Returned rather than written through two adjacent out-parameters: a `std::size_t&` cursor and a
// `std::uint8_t&` value side by side are transposable at a call site, and transposing them here would
// corrupt the address the whole §13.5 policy is stated over.
struct Octet {
    bool ok{false};
    std::uint8_t value{0};
    std::size_t next{0};
};

[[nodiscard]] Octet parse_octet(const std::string_view text, const std::size_t from) {
    std::size_t pos = from;
    const std::size_t start = pos;
    unsigned value = 0;

    while (pos < text.size() && is_digit(text[pos])) {
        value = (value * 10U) + static_cast<unsigned>(text[pos] - '0');

        if (value > 255U) {
            return {};
        }

        ++pos;
    }

    const std::size_t digits = pos - start;

    if (digits == 0 || digits > 3) {
        return {};
    }

    if (digits > 1 && text[start] == '0') {
        return {};
    }

    return {true, static_cast<std::uint8_t>(value), pos};
}

} // namespace

std::string_view log_label(const Destination destination) {
    switch (destination) {
    case Destination::Private:
        return "private";
    case Destination::LinkLocal:
        return "link-local";
    case Destination::Loopback:
        return "loopback";
    case Destination::Unspecified:
        return "unspecified";
    case Destination::Multicast:
        return "multicast";
    case Destination::Public:
        return "public";
    }

    return "unknown";
}

bool permits(const Destination destination) {
    switch (destination) {
    case Destination::Private:
    case Destination::LinkLocal:
        return true;

    case Destination::Loopback:
    case Destination::Unspecified:
    case Destination::Multicast:
    case Destination::Public:
        return false;
    }

    return false;
}

Destination classify(const Ipv4& address) {
    const std::uint8_t first = address.octets[0];
    const std::uint8_t second = address.octets[1];

    // Checked before anything else: 0.0.0.0/8 is never a destination, and 0.0.0.0 in particular is
    // what an unset or failed resolution looks like.
    if (first == 0) {
        return Destination::Unspecified;
    }

    if (first == 127) {
        return Destination::Loopback;
    }

    // 255.255.255.255 is the limited broadcast address; the rest of 240/4 is reserved. Both are
    // grouped with multicast because the product's answer to all of them is the same refusal.
    if (first >= 224) {
        return Destination::Multicast;
    }

    if (first == 10) {
        return Destination::Private;
    }

    // 172.16.0.0 – 172.31.255.255. Note the bound: 172.32.x.x is public, and an off-by-one here
    // would admit a globally routable range.
    if (first == 172 && second >= 16 && second <= 31) {
        return Destination::Private;
    }

    if (first == 192 && second == 168) {
        return Destination::Private;
    }

    if (first == 169 && second == 254) {
        return Destination::LinkLocal;
    }

    // Everything else is treated as public, including 100.64/10 carrier NAT and the documentation
    // ranges. The default is refusal: a range this function has not been taught about must not
    // become a destination for a credential by accident.
    return Destination::Public;
}

bool parse_ipv4(const std::string_view text, Ipv4& out) {
    if (text.empty() || text.size() > 15) {
        return false;
    }

    std::size_t pos = 0;

    for (std::size_t index = 0; index < 4; ++index) {
        if (index != 0) {
            if (pos >= text.size() || text[pos] != '.') {
                return false;
            }

            ++pos;
        }

        const Octet octet = parse_octet(text, pos);

        if (!octet.ok) {
            return false;
        }

        out.octets[index] = octet.value;
        pos = octet.next;
    }

    return pos == text.size();
}

std::size_t render_ipv4(const Ipv4& address, char* const out, const std::size_t capacity) {
    if (out == nullptr) {
        return 0;
    }

    const int written = std::snprintf(
        out, capacity, "%u.%u.%u.%u", static_cast<unsigned>(address.octets[0]),
        static_cast<unsigned>(address.octets[1]), static_cast<unsigned>(address.octets[2]),
        static_cast<unsigned>(address.octets[3]));

    if (written <= 0 || static_cast<std::size_t>(written) >= capacity) {
        return 0;
    }

    return static_cast<std::size_t>(written);
}

bool is_resolvable_name(const std::string_view host) {
    if (host.empty() || host.size() > 64) {
        return false;
    }

    // An address literal is not a name; the caller must classify it directly rather than hand it
    // to a resolver.
    Ipv4 ignored;

    if (parse_ipv4(host, ignored)) {
        return false;
    }

    for (const char chr : host) {
        const bool letter = (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z');

        if (!letter && !is_digit(chr) && chr != '-' && chr != '.') {
            return false;
        }
    }

    // No empty label anywhere, and no trailing or leading dot: a resolver's treatment of those
    // varies, and a host the checker and the stack read differently is the whole problem.
    return host.front() != '.' && host.back() != '.' && host.find("..") == std::string_view::npos &&
           host.front() != '-' && host.back() != '-';
}

} // namespace prd::network::lan_policy
