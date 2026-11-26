// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace host_json {

struct Field {
    std::string name;
    // A disengaged value denotes a non-string node.
    std::optional<std::string> text;
};

struct Document {
    enum class Kind { Object, Array, Invalid };

    Kind kind{Kind::Object};
    std::vector<Field> fields;
    // Omitted means the parser reached the end of the submitted text.
    std::optional<std::size_t> end;
};

// The HTTP fixture owns the body and its scripted parser result for one request.
void set_document(std::string_view body, const Document& document);
void clear_document();

} // namespace host_json
