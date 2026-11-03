// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "decode.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "json_scan.hpp"

namespace prd::prusalink {
namespace {

// One optional numeric member, read or not.
//
// Returned as a value rather than written through a bool& and a double&: those two are
// adjacent, convertible and therefore swappable at a call site, and swapping them would
// silently mark every temperature known.
struct OptionalNumber {
    bool known{false};
    double value{0.0};
};

// Reads an optional numeric member. Absent, null and wrong-typed all report not-known:
// §3.2's tolerance rule applies to every value the product can do without.
[[nodiscard]] OptionalNumber optional_number(const JsonObject& object,
                                             const std::string_view name) {
    const JsonField field = find(object, name);

    if (field.type != JsonType::Number) {
        return {};
    }

    double read = 0.0;

    if (!read_number(field, read)) {
        return {};
    }

    return {true, read};
}

// Reads an optional numeric member and additionally requires it to be FINITE.
//
// ⛔ WHY `optional_number` IS NOT ENOUGH FOR THE JOB FIELDS. JSON has no NaN or Infinity literal,
// so a well-formed document cannot spell one directly — but `1e400` is a well-formed JSON number
// and `strtod` converts it to infinity while consuming the whole token, which read_number()
// reports as a successful read. An infinite progress or duration then reaches the domain
// conversion, where every comparison against it is false and the value is neither accepted nor
// rejected on its merits. Refusing it here is the truthful answer: the printer sent something this
// build cannot represent, so the value is not known.
//
// ⬅️ SCOPED TO THE JOB FIELDS ON PURPOSE. The four temperatures still go through
// `optional_number`, which has been physically validated as it stands; widening the guard to them
// is a behaviour change to accepted code and belongs to its own checkpoint, not to this one.
[[nodiscard]] OptionalNumber finite_number(const JsonObject& object, const std::string_view name) {
    const OptionalNumber read = optional_number(object, name);

    if (!read.known || !std::isfinite(read.value)) {
        return {};
    }

    return read;
}

// Reads the job identity, which the contract types as an integer.
//
// ⬅️ TWO SPELLINGS, AND NEITHER IS A GUESS ABOUT THE OTHER. The OpenAPI contract and v6.8.1 both
// make `id` a number, so that is the primary form and it is rendered to decimal text because
// `JobObservation::id` is text. A string is accepted as well: §3.2 requires tolerance, the two
// JSON types cannot be confused for one another, and reading only one of them would silently lose
// the identity that `apply_job()`'s job-change detection depends on.
//
// A non-integral or out-of-range number is left unknown rather than truncated. An id is an
// identity: half of one is not a weaker version of it, it is a different job.
template <std::size_t Capacity>
[[nodiscard]] bool optional_identity(const JsonObject& object, const std::string_view name,
                                     app_model::BoundedString<Capacity>& out) {
    const JsonField field = find(object, name);

    if (field.type == JsonType::String) {
        std::array<char, Capacity> buffer{};
        std::size_t length = 0;

        if (!read_string(field, buffer.data(), Capacity, length) || length == 0) {
            return false;
        }

        return out.assign(std::string_view{buffer.data(), length});
    }

    if (field.type != JsonType::Number) {
        return false;
    }

    double value = 0.0;

    if (!read_number(field, value) || !std::isfinite(value)) {
        return false;
    }

    constexpr double kMaxIdentity = 4294967295.0;

    if (value < 0.0 || value > kMaxIdentity || value != std::floor(value)) {
        return false;
    }

    // Decimal, built here rather than with std::to_string: this translation unit is on the target's
    // network path and the identity is at most ten digits, so the bounded buffer is both smaller
    // and free of the allocation §16.4 asks the decoder to do without.
    std::array<char, 10> digits{};
    std::size_t count = 0;
    auto remaining = static_cast<std::uint32_t>(value);

    do {
        digits.at(count) = static_cast<char>('0' + (remaining % 10U));
        ++count;
        remaining /= 10U;
    } while (remaining != 0U && count < digits.size());

    std::array<char, 10> text{};
    for (std::size_t index = 0; index < count; ++index) {
        text.at(index) = digits.at(count - 1 - index);
    }

    return out.assign(std::string_view{text.data(), count});
}

// Reads a required string into a bounded destination.
//
// Returns MissingField when absent and UnusableField when present but unreadable —
// wrong type, bad escape, or longer than the destination. The distinction is what
// lets a log say whether the printer said nothing or said something this build
// cannot represent.
template <std::size_t Capacity>
[[nodiscard]] DecodeError required_string(const JsonObject& object, const std::string_view name,
                                          app_model::BoundedString<Capacity>& out) {
    const JsonField field = find(object, name);

    if (!field.present() || field.type == JsonType::Null) {
        return DecodeError::MissingField;
    }

    if (field.type != JsonType::String) {
        return DecodeError::UnusableField;
    }

    std::array<char, Capacity> buffer{};
    std::size_t length = 0;

    if (!read_string(field, buffer.data(), Capacity, length)) {
        return DecodeError::UnusableField;
    }

    if (length == 0) {
        return DecodeError::UnusableField;
    }

    // assign() cannot truncate here: read_string() already refused anything longer
    // than Capacity. The result is checked anyway, because a silent truncation is
    // exactly the failure this function exists to prevent.
    if (!out.assign(std::string_view{buffer.data(), length})) {
        return DecodeError::UnusableField;
    }

    return DecodeError::None;
}

} // namespace

std::string_view log_label(const DecodeError error) {
    switch (error) {
    case DecodeError::None:
        return "none";
    case DecodeError::Malformed:
        return "malformed";
    case DecodeError::MissingObject:
        return "missing-object";
    case DecodeError::MissingField:
        return "missing-field";
    case DecodeError::UnusableField:
        return "unusable-field";
    }

    return "unknown";
}

DecodeError decode_status(const std::string_view body, StatusDto& out) {
    JsonObject root;

    if (parse_object(body, root) != ScanStatus::Ok) {
        return DecodeError::Malformed;
    }

    // §4.3: "the `printer` object is required and other top-level values are
    // optional". `job`, `storage`, `transfer` and `camera` are not read by this
    // checkpoint and are ignored wherever they appear.
    JsonObject printer;

    if (!find_object(root, "printer", printer)) {
        return DecodeError::MissingObject;
    }

    const DecodeError state = required_string(printer, "state", out.printer_state);

    if (state != DecodeError::None) {
        return state;
    }

    const OptionalNumber nozzle = optional_number(printer, "temp_nozzle");
    out.temp_nozzle_known = nozzle.known;
    out.temp_nozzle = nozzle.value;

    const OptionalNumber nozzle_target = optional_number(printer, "target_nozzle");
    out.target_nozzle_known = nozzle_target.known;
    out.target_nozzle = nozzle_target.value;

    const OptionalNumber bed = optional_number(printer, "temp_bed");
    out.temp_bed_known = bed.known;
    out.temp_bed = bed.value;

    const OptionalNumber bed_target = optional_number(printer, "target_bed");
    out.target_bed_known = bed_target.known;
    out.target_bed = bed_target.value;

    // --- The optional `job` object -------------------------------------------
    //
    // ⛔ NOTHING IN HERE CAN FAIL THE RESPONSE. §4.3 requires only `printer`, and a firmware that
    // omits `job`, or sends one this build cannot use, must still yield a usable printer state —
    // the panel's temperatures and state word do not depend on a job existing. So every path below
    // leaves a value unknown, and `decode_status` still returns None.
    //
    // `storage`, `transfer` and `camera` remain unread and are ignored wherever they appear, as are
    // any additive members of `job` itself: nothing here enumerates members, so a field the
    // firmware starts sending cannot affect the result (§3.2).
    JsonObject job;

    if (!find_object(root, "job", job)) {
        // Absent, null, or present but not an object. All three mean the same thing to this
        // product — no usable job record arrived — and none of them is a protocol fault.
        out.job_present = false;
        return DecodeError::None;
    }

    out.job_present = true;
    out.job_id_known = optional_identity(job, "id", out.job_id);

    const OptionalNumber progress = finite_number(job, "progress");
    out.progress_known = progress.known;
    out.progress_percent = progress.value;

    const OptionalNumber remaining = finite_number(job, "time_remaining");
    out.time_remaining_known = remaining.known;
    out.time_remaining_seconds = remaining.value;

    const OptionalNumber printing = finite_number(job, "time_printing");
    out.time_printing_known = printing.known;
    out.time_printing_seconds = printing.value;

    return DecodeError::None;
}

DecodeError decode_version(const std::string_view body, VersionDto& out,
                           std::string_view* const offending) {
    JsonObject root;

    if (parse_object(body, root) != ScanStatus::Ok) {
        return DecodeError::Malformed;
    }

    // §4.1's required set, all of it. Checked in the document's own order so a service missing
    // several fields reports the first one, which is the most useful thing to log.
    //
    // ⬅️ AND IT NAMES THAT FIELD. Because the first failure ends the decode, "missing-field" alone
    // does not identify which of the five a real printer omitted — the question a physical
    // validation actually has to answer.
    //
    // ⛔ THE NAME IS RECORDED LOCALLY AND PUBLISHED ONLY ON THE WAY OUT OF A FAILURE. Writing it
    // through the pointer as each field was checked meant the version alias left "version" behind
    // after falling back to `server` and succeeding, so a decode that worked reported a field that
    // had not failed. A caller logging that name would have printed a fault on a success.
    std::string_view failed_on;

    const auto check = [&root, &failed_on](const std::string_view name, auto& destination) {
        const DecodeError error = required_string(root, name, destination);

        if (error != DecodeError::None) {
            failed_on = name;
        }

        return error;
    };

    const auto report = [offending, &failed_on](const DecodeError error) {
        if (offending != nullptr) {
            *offending = failed_on;
        }

        return error;
    };

    const DecodeError api = check("api", out.api);

    if (api != DecodeError::None) {
        return report(api);
    }

    // ⬅️ `version` OR `server`, AND THIS IS NOT A RELAXATION. CORE One firmware 6.8.1+16182 answers
    // /api/version with `server` and no `version`. Captured from the owner's printer on 2026-08-29:
    // 183 bytes, byte-for-byte the body this decoder had already rejected on the device, so the
    // alias is fitted to a measured response and not to an assumption about Buddy firmware.
    //
    // `server` is the OctoPrint-lineage spelling of the same value — the PrusaLink server version,
    // which is what §4.1's `version` describes and what VersionDto::version holds. All five values
    // are still required; one of them may arrive under either of two names, so a service answering
    // with fewer than five still fails identification exactly as before.
    //
    // ⛔ THE FALLBACK IS FOR ABSENCE ONLY. A `version` that is present but unusable — wrong type, or
    // longer than its destination — is a protocol fault under §9.3 and is reported as one rather
    // than masked by trying the other spelling.
    DecodeError version = check("version", out.version);

    if (version == DecodeError::MissingField) {
        version = check("server", out.version);

        if (version == DecodeError::MissingField) {
            failed_on = "version|server";
        }
    }

    if (version != DecodeError::None) {
        return report(version);
    }

    const DecodeError printer = check("printer", out.printer);

    if (printer != DecodeError::None) {
        return report(printer);
    }

    const DecodeError text = check("text", out.text);

    if (text != DecodeError::None) {
        return report(text);
    }

    const DecodeError firmware = check("firmware", out.firmware);

    if (firmware != DecodeError::None) {
        return report(firmware);
    }

    return DecodeError::None;
}

} // namespace prd::prusalink
