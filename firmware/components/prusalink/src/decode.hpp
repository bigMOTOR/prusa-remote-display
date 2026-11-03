// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "dto.hpp"

// Response body → DTO, or a typed reason why not (Architecture Spec §5.8's
// `prusalink_decoder`, §9.1 step 5).
//
// Pure and host-tested. This is the layer that meets a real printer's output, so
// every way that output can be wrong is a case with a test rather than a surprise on
// the bench: §9.3 requires malformed required fields to be *classified*, not merely
// rejected, because the connection state a user sees differs — an unreadable body is
// `invalidResponse`, an unparseable one is not the same fact as a missing `state`.

namespace prd::prusalink {

enum class DecodeError : std::uint8_t {
    None,

    // The body is not one well-formed JSON object, or nests too deeply.
    Malformed,

    // A required object is absent: `printer` in a status response (§4.3).
    MissingObject,

    // A required field is absent: `state` in a status response (§5.1), or one of
    // §4.1's required version fields.
    MissingField,

    // A required field is present but is the wrong JSON type, or is a string too
    // long for its bounded destination. §9.3: classified, not silently clipped —
    // a truncated state string must never be decoded into a state.
    UnusableField,
};

[[nodiscard]] std::string_view log_label(DecodeError error);

// Decodes `GET /api/v1/status`.
//
// Optional values that are absent, null, or of an unexpected type are left unknown
// rather than failing the response: §3.2 requires the client to tolerate optional
// and additive fields, and a firmware that omits the bed target must still yield a
// usable printer state. Only `printer.state` can fail the decode.
//
// ⬅️ THAT INCLUDES THE WHOLE OF `job`. §4.3 requires only the `printer` object, so an absent job,
// a null one, one that is not an object, and one whose every field is unusable all leave the job
// unknown and still return None. `StatusDto::job_present` records which of those happened, because
// "the printer reports no job" is a fact about the machine and not a gap in the reading.
[[nodiscard]] DecodeError decode_status(std::string_view body, StatusDto& out);

// Decodes `GET /api/version`.
//
// §4.1's required set is api, version, printer, text and firmware, and this requires all five. The
// PrusaLink version may arrive as either `version` or `server`; decode.cpp records the CORE One
// response that made the second spelling necessary.
//
// ⛔ THIS COMMENT USED TO SAY `text` WAS TOLERATED. It has not been since 2026-08-23, and dto.hpp
// records why: requiring only part of the set let `{"api":..,"firmware":..}` from an unrelated
// service validate and be promoted to an active printer configuration. Requiring the whole set is
// what makes the preflight an identification rather than a liveness check.
//
// `offending` is optional. When the decode fails on one of the five it receives that field's name,
// which is one of five compile-time literals — never a byte of the body, a header or an endpoint.
// Without it a field report from real hardware says only that some unnamed part of the set was
// absent, which cannot be acted on: the five are checked in order and the first failure ends the
// decode.
[[nodiscard]] DecodeError decode_version(std::string_view body, VersionDto& out,
                                         std::string_view* offending = nullptr);

} // namespace prd::prusalink
