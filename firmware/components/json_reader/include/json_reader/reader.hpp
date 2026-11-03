// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// A bounded, allocation-free reader for the small JSON objects PrusaLink returns.
//
// WHY NOT cJSON. cJSON ships with ESP-IDF and the provisioning server already uses
// it, so the obvious choice would be to reuse it here. Two properties decided
// against it, and both are requirements rather than preferences.
//
// The first is verification. Architecture Spec §3.4 and §9.2 put the decoder on the
// wrong side of the API boundary: malformed bodies, unknown members, absent optional
// fields and oversized responses are exactly the cases that must be proved by host
// tests, because a real printer will not produce them on demand. The host suite is a
// standalone CMake project with no ESP-IDF in it (firmware/test/host), so a
// cJSON-based decoder could only be tested on the target — which is to say, not
// tested. This reader compiles into both builds, so every host test exercises the
// same code that runs on the device.
//
// The second is memory. cJSON allocates a node per value, and §16.4 requires "every
// network body and queue must be bounded" with a defined behaviour on allocation
// failure. This reader allocates nothing: it validates in place and returns views
// into the caller's buffer. The only bound is the caller's own body buffer.
//
// WHAT IT DELIBERATELY IS NOT. Not a general JSON library. It reads members of an
// object, and objects nested in objects, which is the whole shape of §4.1's version
// response and §4.3's status response. Arrays are validated so a document containing
// them is accepted, but there is no element accessor: nothing in the accepted MVP
// endpoint set (§19) needs one. Adding one later is a small change; adding it now
// would be a speculative abstraction.
//
// STRUCTURE IS VALIDATED BEFORE ANYTHING IS READ. parse_object() walks the whole
// document once and rejects it as a unit. Every later lookup therefore scans text
// already proved well-formed, which is what keeps the accessors free of error paths
// that a caller could forget to check.

namespace prd::json_reader {

// Hard structural limits. Both are refusals, not truncations: a document that
// exceeds either is rejected whole, because a partially read status response could
// present a stale or invented printer state as current (§9.3, §24.18).
//
// The depth limit is generous for the documented shapes — status nests one level
// (`printer`, `job`) and version's `capabilities` is also one — while still bounding
// the recursion that validation performs on this task's stack.
inline constexpr std::size_t kMaxDepth = 8;

enum class ScanStatus : std::uint8_t {
    Ok,

    // Not a single well-formed JSON object: bad syntax, an unterminated string, a
    // trailing comma, a non-object at the top level, or trailing content after it.
    Malformed,

    // Nesting exceeded kMaxDepth.
    TooDeep,
};

[[nodiscard]] std::string_view log_label(ScanStatus status);

// A validated object: the document text plus the half-open range of its body,
// `[begin, end)`, where `begin` indexes the '{' and `end` is one past the '}'.
struct JsonObject {
    std::string_view text;
    std::size_t begin{0};
    std::size_t end{0};
};

// A validated JSON array. Like JsonObject, it borrows the caller's immutable
// input. Arrays were initially validation-only because the PrusaLink schemas did
// not need element access. Release discovery does: GitHub returns one bounded
// `assets` array whose objects must be inspected without allocating a DOM.
struct JsonArray {
    std::string_view text;
    std::size_t begin{0};
    std::size_t end{0};
};

enum class JsonType : std::uint8_t {
    // The member was not present. Distinct from Null, which is a value the printer
    // chose to send — §9.3 requires absent and zero to stay distinguishable, and the
    // same argument applies to absent and null.
    Absent,
    Null,
    Bool,
    Number,
    String,
    Object,
    Array,
};

// A member's value, as a view into the document.
struct JsonField {
    JsonType type{JsonType::Absent};

    // Number: the numeric token exactly as written. String: the raw contents
    // between the quotes, escapes NOT yet resolved — read_string() resolves them.
    // Object and Array: the full text of the value, so it can be re-entered.
    std::string_view raw;

    bool boolean{false};

    [[nodiscard]] bool present() const {
        return type != JsonType::Absent;
    }
};

// Validates `text` as one JSON object and returns its extent.
//
// Leading and trailing whitespace is permitted; anything else after the closing
// brace is Malformed, so a truncated response followed by garbage cannot be read as
// a valid prefix.
[[nodiscard]] ScanStatus parse_object(std::string_view text, JsonObject& out);

// Looks up a direct member by name. Absent when the object has no such member.
//
// Unknown members are ignored by construction: nothing here enumerates members, so
// an additive field the printer starts sending cannot affect the result (§3.2,
// §9.3 "tolerate additive fields").
[[nodiscard]] JsonField find(const JsonObject& object, std::string_view name);

// Looks up an object-valued member and returns it ready to be read from. False when
// the member is absent or is not an object — the caller cannot mistake "no `printer`
// object" for "an empty one".
[[nodiscard]] bool find_object(const JsonObject& object, std::string_view name, JsonObject& out);

// Looks up an array-valued member. False for an absent/non-array member.
[[nodiscard]] bool find_array(const JsonObject& object, std::string_view name, JsonArray& out);

enum class ArrayStep : std::uint8_t {
    Item,
    End,
    Malformed,
};

// Reads the next direct element from a validated array. Set `cursor` to zero for
// the first element; it is advanced after every Item. The returned field borrows
// from the array input. Malformed is defensive: a JsonArray produced by
// find_array cannot reach it unless its backing text was mutated afterwards.
[[nodiscard]] ArrayStep next(const JsonArray& array, std::size_t& cursor, JsonField& out);

// Reads a number. Accepts integer or fractional notation, per §9.3's "accept integer
// or floating-point numeric values where reasonable": PrusaLink reports temperatures
// as either on different firmware. False when the field is absent or not a number,
// or when the token does not fit a double without loss of meaning.
[[nodiscard]] bool read_number(const JsonField& field, double& out);

// Reads a string into a fixed buffer, resolving escapes.
//
// `capacity` counts bytes available for characters; no terminator is written, and
// `length` reports what was produced. False when the field is absent, is not a
// string, contains an invalid escape, or does not fit — a truncated printer state
// string must not be decoded into a state, so this refuses rather than clips.
//
// \uXXXX is accepted and resolved only for code points below 0x80; anything higher
// becomes '?'. Every value this decoder reads is an ASCII enum name or an identifier
// (§4.1, §5.1), so the substitution cannot affect a decision — and refusing valid
// JSON outright would be worse.
[[nodiscard]] bool read_string(const JsonField& field, char* out, std::size_t capacity,
                               std::size_t& length);

} // namespace prd::json_reader

namespace prd::json_reader {
// Opt-in strict accessors. Legacy functions above remain unchanged.
// Printable ASCII only, including escaped ASCII; NUL and substitutions refused.
// On failure length is zero and, when out is non-null, all capacity bytes are
// zeroed. Caller supplies a writable buffer of capacity bytes. Success writes
// exactly length characters, without appending a terminator.
[[nodiscard]] bool read_ascii(const JsonField& field, char* out, std::size_t capacity,
                              std::size_t& length);
// Exact nonnegative decimal integer token; no fractions/exponents/signs or overflow.
[[nodiscard]] bool read_unsigned(const JsonField& field, std::uint64_t& out);
// Required known names are printable ASCII, at most 64 bytes. Decoded aliases
// count as the same name. Missing is Absent; duplicates return false. Unknown
// non-ASCII/long keys cannot alias a known ASCII name and are ignored.
// On every false return out is reset to Absent, with an empty raw view.
// Like legacy lookup, input must describe a validated, immutable object.
[[nodiscard]] bool find_unique(const JsonObject& object, std::string_view name, JsonField& out);
} // namespace prd::json_reader
