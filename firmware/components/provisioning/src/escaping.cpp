// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "escaping.hpp"

#include <array>
#include <cstddef>

namespace prd::provisioning {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

void append_escaped_control(std::string& out, const unsigned char byte) {
    // The short forms RFC 8259 names, then \u00XX for everything else below 0x20. A raw
    // control byte inside a JSON string is a parse error, not merely ugly.
    switch (byte) {
    case '\b':
        out += "\\b";
        return;
    case '\f':
        out += "\\f";
        return;
    case '\n':
        out += "\\n";
        return;
    case '\r':
        out += "\\r";
        return;
    case '\t':
        out += "\\t";
        return;
    default:
        break;
    }

    out += "\\u00";
    out += kHexDigits[(byte >> 4U) & 0x0FU];
    out += kHexDigits[byte & 0x0FU];
}

} // namespace

std::string json_escape(const std::string_view value) {
    std::string out;
    out.reserve(value.size());

    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);

        if (character == '"') {
            out += "\\\"";
            continue;
        }

        if (character == '\\') {
            out += "\\\\";
            continue;
        }

        if (byte < 0x20U) {
            append_escaped_control(out, byte);
            continue;
        }

        out += character;
    }

    return out;
}

void append_json_string(std::string& out, const FieldName name, const std::string_view value) {
    out += '"';
    out += name.text;
    out += "\":\"";
    out += json_escape(value);
    out += '"';
}

void append_json_bool(std::string& out, const FieldName name, const bool value) {
    out += '"';
    out += name.text;
    out += "\":";
    out += value ? "true" : "false";
}

} // namespace prd::provisioning
