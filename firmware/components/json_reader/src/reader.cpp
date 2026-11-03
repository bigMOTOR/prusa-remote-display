// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "json_reader/reader.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace prd::json_reader {
namespace {

// The bracket pair of a container, passed as one value so the two cannot be swapped at a
// call site — they are both chars, and swapping them would accept `}1{`.
struct Brackets {
    char open;
    char close;
};

constexpr Brackets kObject{'{', '}'};
constexpr Brackets kArray{'[', ']'};

[[nodiscard]] bool is_space(const char chr) {
    return chr == ' ' || chr == '\t' || chr == '\n' || chr == '\r';
}

[[nodiscard]] bool is_digit(const char chr) {
    return chr >= '0' && chr <= '9';
}

[[nodiscard]] bool is_hex(const char chr) {
    return is_digit(chr) || (chr >= 'a' && chr <= 'f') || (chr >= 'A' && chr <= 'F');
}

// Advances past whitespace. Returns false at end of input, so every caller that needs a
// character has one.
[[nodiscard]] bool skip_space(const std::string_view text, std::size_t& pos) {
    while (pos < text.size() && is_space(text[pos])) {
        ++pos;
    }

    return pos < text.size();
}

[[nodiscard]] bool literal(const std::string_view text, std::size_t& pos,
                           const std::string_view word) {
    if (text.size() - pos < word.size() || text.compare(pos, word.size(), word) != 0) {
        return false;
    }

    pos += word.size();
    return true;
}

// Validates one escape sequence, leaving `pos` one past it. `pos` indexes the character
// after the backslash on entry.
[[nodiscard]] bool scan_escape(const std::string_view text, std::size_t& pos) {
    if (pos >= text.size()) {
        return false;
    }

    const char escape = text[pos];

    if (escape == 'u') {
        // Four hex digits must follow the 'u'.
        if (text.size() - pos < 5) {
            return false;
        }

        for (std::size_t digit = 1; digit <= 4; ++digit) {
            if (!is_hex(text[pos + digit])) {
                return false;
            }
        }

        pos += 5;
        return true;
    }

    if (escape != '"' && escape != '\\' && escape != '/' && escape != 'b' && escape != 'f' &&
        escape != 'n' && escape != 'r' && escape != 't') {
        return false;
    }

    ++pos;
    return true;
}

// Scans a quoted string, leaving `pos` one past the closing quote. `contents` receives the
// raw bytes between the quotes.
//
// Escapes are validated here and resolved later by read_string(). Validating them twice
// would be the alternative, and the reason not to is that a document is accepted or
// rejected as a whole: an invalid escape anywhere must fail the document, not just the
// field somebody happens to read.
[[nodiscard]] bool scan_string(const std::string_view text, std::size_t& pos,
                               std::string_view& contents) {
    if (pos >= text.size() || text[pos] != '"') {
        return false;
    }

    ++pos;
    const std::size_t start = pos;

    while (pos < text.size()) {
        const char chr = text[pos];

        if (chr == '"') {
            contents = text.substr(start, pos - start);
            ++pos;
            return true;
        }

        if (chr == '\\') {
            ++pos;

            if (!scan_escape(text, pos)) {
                return false;
            }

            continue;
        }

        // Unescaped control characters are not legal in a JSON string, and a raw newline in
        // the middle of a body is the shape a truncated-and-concatenated response takes.
        if (static_cast<unsigned char>(chr) < 0x20U) {
            return false;
        }

        ++pos;
    }

    return false;
}

// Consumes one or more digits. False when there is not at least one.
[[nodiscard]] bool scan_digits(const std::string_view text, std::size_t& pos) {
    if (pos >= text.size() || !is_digit(text[pos])) {
        return false;
    }

    while (pos < text.size() && is_digit(text[pos])) {
        ++pos;
    }

    return true;
}

// Consumes the integer part, rejecting JSON's forbidden leading zeros: `01` is two tokens,
// not one, and accepting it would let a malformed body scan as well-formed.
[[nodiscard]] bool scan_integer_part(const std::string_view text, std::size_t& pos) {
    if (pos >= text.size() || !is_digit(text[pos])) {
        return false;
    }

    if (text[pos] == '0') {
        ++pos;
        return true;
    }

    return scan_digits(text, pos);
}

// Consumes an optional fraction. False only when a '.' is present with no digits after it.
[[nodiscard]] bool scan_fraction(const std::string_view text, std::size_t& pos) {
    if (pos >= text.size() || text[pos] != '.') {
        return true;
    }

    ++pos;
    return scan_digits(text, pos);
}

// Consumes an optional exponent. False only when an 'e' is present without a valid value.
[[nodiscard]] bool scan_exponent(const std::string_view text, std::size_t& pos) {
    if (pos >= text.size() || (text[pos] != 'e' && text[pos] != 'E')) {
        return true;
    }

    ++pos;

    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        ++pos;
    }

    return scan_digits(text, pos);
}

// Scans a number, leaving `pos` one past its last character.
[[nodiscard]] bool scan_number(const std::string_view text, std::size_t& pos) {
    if (pos < text.size() && text[pos] == '-') {
        ++pos;
    }

    return scan_integer_part(text, pos) && scan_fraction(text, pos) && scan_exponent(text, pos);
}

ScanStatus skip_value(std::string_view text, std::size_t& pos, std::size_t depth);

// Scans an object or array body, `pos` indexing its opening bracket on entry.
ScanStatus skip_container(const std::string_view text, std::size_t& pos, const std::size_t depth,
                          const Brackets brackets) {
    if (depth >= kMaxDepth) {
        return ScanStatus::TooDeep;
    }

    ++pos; // past the opening bracket

    if (!skip_space(text, pos)) {
        return ScanStatus::Malformed;
    }

    if (text[pos] == brackets.close) {
        ++pos;
        return ScanStatus::Ok;
    }

    for (;;) {
        if (!skip_space(text, pos)) {
            return ScanStatus::Malformed;
        }

        if (brackets.open == kObject.open) {
            std::string_view name;

            if (!scan_string(text, pos, name)) {
                return ScanStatus::Malformed;
            }

            if (!skip_space(text, pos) || text[pos] != ':') {
                return ScanStatus::Malformed;
            }

            ++pos;
        }

        const ScanStatus value = skip_value(text, pos, depth + 1);

        if (value != ScanStatus::Ok) {
            return value;
        }

        if (!skip_space(text, pos)) {
            return ScanStatus::Malformed;
        }

        if (text[pos] == ',') {
            ++pos;
            continue;
        }

        if (text[pos] == brackets.close) {
            ++pos;
            return ScanStatus::Ok;
        }

        return ScanStatus::Malformed;
    }
}

ScanStatus skip_value(const std::string_view text, std::size_t& pos, const std::size_t depth) {
    if (!skip_space(text, pos)) {
        return ScanStatus::Malformed;
    }

    const char chr = text[pos];

    if (chr == kObject.open) {
        return skip_container(text, pos, depth, kObject);
    }

    if (chr == kArray.open) {
        return skip_container(text, pos, depth, kArray);
    }

    if (chr == '"') {
        std::string_view ignored;
        return scan_string(text, pos, ignored) ? ScanStatus::Ok : ScanStatus::Malformed;
    }

    if (chr == '-' || is_digit(chr)) {
        return scan_number(text, pos) ? ScanStatus::Ok : ScanStatus::Malformed;
    }

    if (literal(text, pos, "true") || literal(text, pos, "false") || literal(text, pos, "null")) {
        return ScanStatus::Ok;
    }

    return ScanStatus::Malformed;
}

// Reads the value starting at `pos` into a field, assuming validated text.
[[nodiscard]] JsonField value_at(const std::string_view text, std::size_t pos) {
    JsonField field;

    if (!skip_space(text, pos)) {
        return field;
    }

    const std::size_t start = pos;
    const char chr = text[pos];

    if (chr == '"') {
        std::string_view contents;
        (void)scan_string(text, pos, contents);
        field.type = JsonType::String;
        field.raw = contents;
        return field;
    }

    if (chr == '-' || is_digit(chr)) {
        (void)scan_number(text, pos);
        field.type = JsonType::Number;
        field.raw = text.substr(start, pos - start);
        return field;
    }

    if (chr == kObject.open || chr == kArray.open) {
        const bool is_object = chr == kObject.open;
        (void)skip_container(text, pos, 0, is_object ? kObject : kArray);
        field.type = is_object ? JsonType::Object : JsonType::Array;
        field.raw = text.substr(start, pos - start);
        return field;
    }

    if (literal(text, pos, "true")) {
        field.type = JsonType::Bool;
        field.boolean = true;
        return field;
    }

    if (literal(text, pos, "false")) {
        field.type = JsonType::Bool;
        field.boolean = false;
        return field;
    }

    if (literal(text, pos, "null")) {
        field.type = JsonType::Null;
        return field;
    }

    return field;
}

// One resolved escape: the character it produced, or a refusal.
struct Resolved {
    bool ok{false};
    char produced{'\0'};
};

// Resolves one escape sequence, `pos` indexing the character after the backslash on entry
// and advancing past what was consumed.
[[nodiscard]] Resolved resolve_escape(const std::string_view raw, std::size_t& pos) {
    if (pos >= raw.size()) {
        return {};
    }

    const char escape = raw[pos++];

    switch (escape) {
    case '"':
        return {true, '"'};
    case '\\':
        return {true, '\\'};
    case '/':
        return {true, '/'};
    case 'b':
        return {true, '\b'};
    case 'f':
        return {true, '\f'};
    case 'n':
        return {true, '\n'};
    case 'r':
        return {true, '\r'};
    case 't':
        return {true, '\t'};
    default:
        break;
    }

    if (escape != 'u' || raw.size() - pos < 4) {
        return {};
    }

    unsigned code = 0;

    for (std::size_t digit = 0; digit < 4; ++digit) {
        const char hex = raw[pos + digit];
        unsigned value = 0;

        if (is_digit(hex)) {
            value = static_cast<unsigned>(hex - '0');
        } else if (hex >= 'a' && hex <= 'f') {
            value = static_cast<unsigned>(hex - 'a') + 10U;
        } else if (hex >= 'A' && hex <= 'F') {
            value = static_cast<unsigned>(hex - 'A') + 10U;
        } else {
            return {};
        }

        code = (code << 4U) | value;
    }

    pos += 4;

    // Above ASCII becomes '?'. Every value this decoder reads is an ASCII enum name or an
    // identifier (§4.1, §5.1), so the substitution cannot affect a decision — and refusing
    // valid JSON outright would be worse.
    return {true, code < 0x80U ? static_cast<char>(code) : '?'};
}

} // namespace

std::string_view log_label(const ScanStatus status) {
    switch (status) {
    case ScanStatus::Ok:
        return "ok";
    case ScanStatus::Malformed:
        return "malformed";
    case ScanStatus::TooDeep:
        return "too-deep";
    }

    return "unknown";
}

ScanStatus parse_object(const std::string_view text, JsonObject& out) {
    std::size_t pos = 0;

    if (!skip_space(text, pos) || text[pos] != kObject.open) {
        return ScanStatus::Malformed;
    }

    const std::size_t begin = pos;
    const ScanStatus status = skip_container(text, pos, 0, kObject);

    if (status != ScanStatus::Ok) {
        return status;
    }

    const std::size_t end = pos;

    // Trailing content is a refusal. A body that is a valid object followed by anything
    // else is not a response this decoder understands, and reading the prefix would be the
    // "partially read status" §9.3 forbids.
    if (skip_space(text, pos)) {
        return ScanStatus::Malformed;
    }

    out.text = text;
    out.begin = begin;
    out.end = end;
    return ScanStatus::Ok;
}

JsonField find(const JsonObject& object, const std::string_view name) {
    const std::string_view text = object.text;
    std::size_t pos = object.begin + 1;

    if (!skip_space(text, pos) || text[pos] == kObject.close) {
        return {};
    }

    for (;;) {
        if (!skip_space(text, pos)) {
            return {};
        }

        std::string_view member;

        if (!scan_string(text, pos, member)) {
            return {};
        }

        if (!skip_space(text, pos) || text[pos] != ':') {
            return {};
        }

        ++pos;

        // Compared raw. Every name this decoder looks for is a plain ASCII identifier from
        // the published schema (§4.1, §4.3), so an escaped spelling of one would be a
        // different member as far as this comparison goes — which is the safe direction: it
        // reads as absent rather than as a match nobody verified.
        const bool matched = member == name;
        const JsonField field = value_at(text, pos);

        if (matched) {
            return field;
        }

        if (!skip_space(text, pos)) {
            return {};
        }

        if (skip_value(text, pos, 0) != ScanStatus::Ok) {
            return {};
        }

        if (!skip_space(text, pos)) {
            return {};
        }

        if (text[pos] == ',') {
            ++pos;
            continue;
        }

        return {};
    }
}

bool find_object(const JsonObject& object, const std::string_view name, JsonObject& out) {
    const JsonField field = find(object, name);

    if (field.type != JsonType::Object) {
        return false;
    }

    const auto begin = static_cast<std::size_t>(field.raw.data() - object.text.data());

    out.text = object.text;
    out.begin = begin;
    out.end = begin + field.raw.size();
    return true;
}

bool find_array(const JsonObject& object, const std::string_view name, JsonArray& out) {
    out = {};
    const JsonField field = find(object, name);

    if (field.type != JsonType::Array) {
        return false;
    }

    const auto begin = static_cast<std::size_t>(field.raw.data() - object.text.data());
    out.text = object.text;
    out.begin = begin;
    out.end = begin + field.raw.size();
    return true;
}

ArrayStep next(const JsonArray& array, std::size_t& cursor, JsonField& out) {
    out = {};
    if (array.begin >= array.end || array.end > array.text.size() ||
        array.text[array.begin] != kArray.open || array.text[array.end - 1] != kArray.close) {
        return ArrayStep::Malformed;
    }

    std::size_t position = cursor == 0 ? array.begin + 1 : cursor;
    if (!skip_space(array.text, position)) {
        return ArrayStep::Malformed;
    }
    if (position >= array.end) {
        return ArrayStep::Malformed;
    }
    if (array.text[position] == kArray.close) {
        cursor = array.end;
        return ArrayStep::End;
    }

    out = value_at(array.text, position);
    if (!out.present() || skip_value(array.text, position, 0) != ScanStatus::Ok ||
        !skip_space(array.text, position) || position >= array.end) {
        out = {};
        return ArrayStep::Malformed;
    }

    if (array.text[position] == ',') {
        ++position;
        cursor = position;
        return ArrayStep::Item;
    }
    if (array.text[position] == kArray.close) {
        cursor = position;
        return ArrayStep::Item;
    }

    out = {};
    return ArrayStep::Malformed;
}

bool read_number(const JsonField& field, double& out) {
    if (field.type != JsonType::Number || field.raw.empty()) {
        return false;
    }

    // The token is already proved to be a well-formed JSON number, so the only work left is
    // the conversion. It is copied into a bounded buffer because strtod needs a terminator
    // and the view points into the middle of the response body.
    constexpr std::size_t kMaxToken = 32;

    if (field.raw.size() >= kMaxToken) {
        return false;
    }

    std::array<char, kMaxToken> token{};

    for (std::size_t index = 0; index < field.raw.size(); ++index) {
        token.at(index) = field.raw[index];
    }

    char* consumed = nullptr;
    const double value = std::strtod(token.data(), &consumed);

    if (consumed != token.data() + field.raw.size()) {
        return false;
    }

    out = value;
    return true;
}

bool read_string(const JsonField& field, char* const out, const std::size_t capacity,
                 std::size_t& length) {
    length = 0;

    if (field.type != JsonType::String || out == nullptr) {
        return false;
    }

    const std::string_view raw = field.raw;
    std::size_t produced = 0;
    std::size_t pos = 0;

    while (pos < raw.size()) {
        if (produced >= capacity) {
            return false;
        }

        const char chr = raw[pos];

        if (chr != '\\') {
            out[produced++] = chr;
            ++pos;
            continue;
        }

        ++pos;
        const Resolved resolved = resolve_escape(raw, pos);

        if (!resolved.ok) {
            return false;
        }

        out[produced++] = resolved.produced;
    }

    length = produced;
    return true;
}

} // namespace prd::json_reader

namespace prd::json_reader {
bool read_ascii(const JsonField& field, char* out, std::size_t capacity, std::size_t& length) {
    length = 0;
    const auto reject = [&]() {
        if (out != nullptr) {
            std::fill_n(out, capacity, '\0');
        }
        length = 0;
        return false;
    };
    if (field.type != JsonType::String || out == nullptr) {
        return reject();
    }
    std::size_t position = 0;
    while (position < field.raw.size()) {
        char value = field.raw[position++];
        if (value == '\\') {
            if (position >= field.raw.size()) {
                return reject();
            }
            if (field.raw[position] == 'u' &&
                (field.raw.size() - position < 5 || field.raw[position + 1] != '0' ||
                 field.raw[position + 2] != '0' || field.raw[position + 3] > '7')) {
                return reject();
            }
            const auto resolved = resolve_escape(field.raw, position);
            if (!resolved.ok) {
                return reject();
            }
            value = resolved.produced;
        }
        if (value < 0x20 || static_cast<unsigned char>(value) > 0x7e || length >= capacity) {
            return reject();
        }
        out[length++] = value;
    }
    return true;
}

bool read_unsigned(const JsonField& field, std::uint64_t& out) {
    out = 0;
    if (field.type != JsonType::Number || field.raw.empty() ||
        (field.raw.size() > 1 && field.raw.front() == '0')) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char character : field.raw) {
        if (!is_digit(character)) {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = (value * 10) + digit;
    }
    out = value;
    return true;
}

bool find_unique(const JsonObject& object, std::string_view name, JsonField& out) {
    out = {};
    JsonField result;
    if (name.size() > 64) {
        return false;
    }
    std::size_t position = object.begin + 1;
    while (skip_space(object.text, position) && object.text[position] != '}') {
        std::string_view raw;
        if (!scan_string(object.text, position, raw) || !skip_space(object.text, position) ||
            object.text[position] != ':') {
            return false;
        }
        ++position;
        if (!skip_space(object.text, position)) {
            return false;
        }
        std::array<char, 64> key{};
        std::size_t length = 0;
        const JsonField name_field{JsonType::String, raw, false};
        if (read_ascii(name_field, key.data(), key.size(), length) &&
            std::string_view(key.data(), length) == name) {
            if (result.present()) {
                return false;
            }
            result = value_at(object.text, position);
        }
        if (skip_value(object.text, position, 0) != ScanStatus::Ok ||
            !skip_space(object.text, position)) {
            return false;
        }
        if (object.text[position] != ',') {
            break;
        }
        ++position;
    }
    out = result;
    return true;
}
} // namespace prd::json_reader
