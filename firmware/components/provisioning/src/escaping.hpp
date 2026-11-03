// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <string_view>

// Escaping for values that leave the device — Architecture Spec §14.4's "escape all
// displayed values".
//
// PURE, AND HOST-TESTED.
//
// ⬅️ THERE IS NO HTML ESCAPER HERE, AND THAT IS THE DESIGN. The served page interpolates
// nothing: index.html is a fixed embedded asset, and app.js renders every value it fetches
// through `textContent`, never `innerHTML`. So there is no path from a value into markup
// to escape. §14.4's rule is met by removing the injection point rather than by defending
// it — a defence has to be remembered at every future call site, and the absence of a path
// does not.
//
// What remains is JSON, because the status endpoint carries values into a response body.

namespace prd::provisioning {

// Escapes a value for use inside a JSON string, per RFC 8259: the two mandatory escapes
// and every control character below 0x20.
//
// Bytes above 0x7E are passed through unchanged. A value that is already UTF-8 stays
// UTF-8; a value that is not was never valid JSON input and cannot be repaired here.
[[nodiscard]] std::string json_escape(std::string_view value);

// A JSON object key.
//
// A DISTINCT TYPE BECAUSE THE KEY AND THE VALUE ARE BOTH STRING VIEWS, and only their order
// tells them apart — the same swappable-parameter defect the backoff ladder's `Jitter` type
// was introduced for. Swapping them here would put a field's value where its name belongs
// and escape the name instead of the value.
//
// The key is always a compile-time literal from this component and is never escaped,
// because it is never external.
struct FieldName {
    std::string_view text;
};

// Appends `"name":"value"` with the value escaped.
void append_json_string(std::string& out, FieldName name, std::string_view value);

// Appends `"name":true` / `"name":false`.
void append_json_bool(std::string& out, FieldName name, bool value);

} // namespace prd::provisioning
