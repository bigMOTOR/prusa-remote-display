// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "discovery_identity.hpp"

#include <string>

namespace prd::network::discovery_identity {
namespace {

// A-F to a-f, and nothing else touched.
//
// NOT std::tolower. That one takes the current locale into account, and there is a real
// locale — Turkish — in which the case mapping of an ASCII letter is not the ASCII one. A
// hostname must not depend on which locale the firmware was built or run under, and the input
// here is four hex digits, so the mapping is written out.
char to_lower_ascii(const char character) {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character - 'A' + 'a');
    }

    return character;
}

std::string lowercased(const std::string_view text) {
    std::string result;
    result.reserve(text.size());

    for (const char character : text) {
        result.push_back(to_lower_ascii(character));
    }

    return result;
}

} // namespace

app_model::BoundedString<24> hostname_for(const std::string_view suffix) {
    std::string text{kHostnamePrefix};
    text += lowercased(suffix);

    app_model::BoundedString<24> hostname;
    hostname.assign(text);
    return hostname;
}

app_model::BoundedString<32> local_name_for(const std::string_view suffix) {
    std::string text{hostname_for(suffix).view()};
    text += kLocalDomain;

    app_model::BoundedString<32> name;
    name.assign(text);
    return name;
}

app_model::BoundedString<48> service_instance_for(const std::string_view suffix) {
    std::string text{kServiceInstancePrefix};
    text += suffix;

    app_model::BoundedString<48> instance;
    instance.assign(text);
    return instance;
}

} // namespace prd::network::discovery_identity
