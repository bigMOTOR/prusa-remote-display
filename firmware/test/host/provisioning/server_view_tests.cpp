// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "server_view.hpp"

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

// What one request sees — the coherence of a single answer.
//
// WHAT THIS FILE IS FOR. The HTTP task used to read the published flow state out of three
// independently updated globals: a Surfaces whose BoundedStrings are replaced field by field, a
// `volatile SetupState`, and a `std::string` on the heap. Beyond the undefined behaviour, one request
// could be answered half from before a provisioning transition and half from after it — the arrival
// surface read at entry, the origin membership read again inside the gate, the setup state read later
// still. Those are now one immutable value taken once, and this file is where "one instant, not
// three" is asserted rather than reviewed.
//
// ⛔ WHAT IT DOES NOT CLAIM. It does not verify the binding. That the handler really takes exactly one
// snapshot at entry, that the depth-one overwrite queue really copies under its own critical section,
// and that no synchronisation primitive is held across an HTTP send are properties of server.cpp,
// which includes esp_http_server.h and cannot be compiled here. They were established by reading it.

namespace {

using prd::provisioning::admits_printer_stage;
using prd::provisioning::CsrfToken;
using prd::provisioning::kUnavailableBody;
using prd::provisioning::ServerSnapshot;
using prd::provisioning::SetupState;
using prd::provisioning::Stage;
using prd::provisioning::status_body;
using prd::provisioning::SubmissionHandoff;
using prd::provisioning::submit_answer;
using prd::provisioning::SubmitAnswer;
using prd::provisioning::Surface;
using prd::provisioning::Surfaces;

// RFC 5737 documentation addresses and a device-shaped hostname. No captured network values.
constexpr std::string_view kAccessPoint = "192.168.4.1";
constexpr std::string_view kStationA = "192.0.2.10";
constexpr std::string_view kStationB = "198.51.100.7";
constexpr std::string_view kHostnameA = "display-aaaa.local";
constexpr std::string_view kHostnameB = "display-bbbb.local";

constexpr std::string_view kVersion = "0.1.0";
constexpr std::string_view kToken = "0123456789abcdef0123456789abcdef";

[[nodiscard]] bool contains(const std::string& body, const std::string_view fragment) {
    return body.find(std::string{fragment}) != std::string::npos;
}

// One complete published state. Every field differs from the other fixture below, so a body built
// from one cannot accidentally look like a body built from the other.
[[nodiscard]] ServerSnapshot editing_on_station_a() {
    ServerSnapshot snapshot;
    snapshot.surfaces.set_access_point(kAccessPoint);
    snapshot.surfaces.set_station(kStationA);
    snapshot.surfaces.set_hostname(kHostnameA);
    snapshot.setup_state = SetupState::Editing;
    (void)snapshot.setup_reason.assign("");
    return snapshot;
}

[[nodiscard]] ServerSnapshot failed_on_station_b() {
    ServerSnapshot snapshot;

    // The access point has gone away, which is how "no longer active" is expressed (ruling B).
    snapshot.surfaces.set_station(kStationB);
    snapshot.surfaces.set_hostname(kHostnameB);
    snapshot.setup_state = SetupState::Failed;
    (void)snapshot.setup_reason.assign("printer-unusable-response");
    return snapshot;
}

// --- Coherence: old or new, never a mixture -----------------------------------------------------

// ⛔ THE PROPERTY THE WHOLE CHECKPOINT TURNS ON. A response describes one instant. The two bodies
// below share no field value, so any mixing would show as a fragment of one appearing in the other.
TEST(ServerViewCoherenceTests, EveryFieldOfOneAnswerComesFromOneSnapshot) {
    const std::string before =
        status_body(editing_on_station_a(), Surface::SetupAccessPoint, kVersion, CsrfToken{kToken});
    const std::string after =
        status_body(failed_on_station_b(), Surface::SetupAccessPoint, kVersion, CsrfToken{kToken});

    EXPECT_TRUE(contains(before, "\"setupState\":\"editing\""));
    EXPECT_TRUE(contains(before, "\"setupReason\":\"\""));
    EXPECT_FALSE(contains(before, "failed"));
    EXPECT_FALSE(contains(before, "printer-unusable-response"));

    EXPECT_TRUE(contains(after, "\"setupState\":\"failed\""));
    EXPECT_TRUE(contains(after, "\"setupReason\":\"printer-unusable-response\""));
    EXPECT_FALSE(contains(after, "\"setupState\":\"editing\""));
}

// The state and its reason move together. They were two writes in a fixed order, and the ordering
// was the guarantee — a discipline that depended on the reader observing both writes in sequence,
// which nothing provided. Now a snapshot that says Failed cannot carry the previous reason.
TEST(ServerViewCoherenceTests, TheStateAndItsReasonCannotDisagree) {
    // The master copy the application task owns.
    ServerSnapshot published = editing_on_station_a();

    // What a request took before the transition.
    const ServerSnapshot taken = published;

    // The application task moves the flow. Both fields, in one act.
    published.setup_state = SetupState::Failed;
    (void)published.setup_reason.assign("printer-unauthorized");

    // The in-flight request still answers entirely from what it took: the old state AND the old
    // reason, never the new state against the old reason.
    const std::string body =
        status_body(taken, Surface::SetupAccessPoint, kVersion, CsrfToken{kToken});
    EXPECT_TRUE(contains(body, "\"setupState\":\"editing\""));
    EXPECT_FALSE(contains(body, "printer-unauthorized"));

    // And the next request answers entirely from the new one.
    const std::string next =
        status_body(published, Surface::SetupAccessPoint, kVersion, CsrfToken{kToken});
    EXPECT_TRUE(contains(next, "\"setupState\":\"failed\""));
    EXPECT_TRUE(contains(next, "\"setupReason\":\"printer-unauthorized\""));
}

// Every combination the application task can publish. publish() moves the state and the reason;
// publish_addresses() replaces all three addresses; start() seeds the access point; stop() clears
// everything. A snapshot must be complete after each.
TEST(ServerViewCoherenceTests, EveryUpdateCombinationLeavesACompleteSnapshot) {
    struct Case {
        SetupState state;
        std::string_view reason;
        std::string_view expected_label;
    };

    const std::vector<Case> cases{
        {SetupState::Editing, "", "editing"},
        {SetupState::Validating, "", "validating"},
        {SetupState::WifiValidated, "", "wifi-validated"},
        {SetupState::Succeeded, "", "succeeded"},
        {SetupState::Failed, "printer-unreachable", "failed"},
    };

    for (const Case& scenario : cases) {
        for (const bool addresses_published : {false, true}) {
            ServerSnapshot snapshot;
            snapshot.surfaces.set_access_point(kAccessPoint);

            if (addresses_published) {
                snapshot.surfaces.set_station(kStationA);
                snapshot.surfaces.set_hostname(kHostnameA);
            }

            snapshot.setup_state = scenario.state;
            (void)snapshot.setup_reason.assign(scenario.reason);

            const std::string body =
                status_body(snapshot, Surface::SetupAccessPoint, kVersion, CsrfToken{kToken});

            EXPECT_TRUE(contains(body, std::string{"\"setupState\":\""} +
                                           std::string{scenario.expected_label} + "\""))
                << scenario.expected_label;
            EXPECT_TRUE(contains(body, std::string{"\"setupReason\":\""} +
                                           std::string{scenario.reason} + "\""));

            // The address set travels with it, so the surface the answer was built for is the
            // surface the same snapshot classifies. The access point is recognised by its own
            // address; everything else is the LAN, published station address or not.
            EXPECT_EQ(snapshot.surfaces.classify(kAccessPoint), Surface::SetupAccessPoint);
            EXPECT_EQ(snapshot.surfaces.classify(kStationA), Surface::StationLan);
            EXPECT_EQ(
                snapshot.surfaces.allows_origin(std::string{"http://"} + std::string{kStationA}),
                addresses_published);
        }
    }
}

// --- Surfaces and origins, from the same snapshot ------------------------------------------------

// ⛔ THE TEARING THAT WAS NOT UNDEFINED BEHAVIOUR BUT WAS STILL WRONG. handle_submit() read the
// surfaces to classify the arrival, and the gate read them AGAIN for origin membership. A
// publish_addresses() between the two answered one request against two different address sets.
// One snapshot means one set.
TEST(ServerViewSurfaceTests, TheArrivalAndTheOriginAreJudgedAgainstOneAddressSet) {
    ServerSnapshot published = editing_on_station_a();
    const ServerSnapshot taken = published;

    // The application task replaces the station address mid-request.
    published.surfaces.set_station(kStationB);

    // The in-flight request still sees exactly one set: the old station address is still a surface
    // it recognises, and its origin is still accepted.
    EXPECT_EQ(taken.surfaces.classify(kStationA), Surface::StationLan);
    EXPECT_TRUE(taken.surfaces.allows_origin(std::string{"http://"} + std::string{kStationA}));

    // And the NEW address is not half-visible to it.
    EXPECT_FALSE(taken.surfaces.allows_origin(std::string{"http://"} + std::string{kStationB}));

    // The next request sees the other complete set.
    EXPECT_TRUE(published.surfaces.allows_origin(std::string{"http://"} + std::string{kStationB}));
    EXPECT_FALSE(published.surfaces.allows_origin(std::string{"http://"} + std::string{kStationA}));
}

// Owner ruling A: the CSRF token is returned only on the surface that could use it. The decision and
// the body come from the same call, so they cannot disagree.
TEST(ServerViewSurfaceTests, TheTokenIsWithheldFromASurfaceThatCannotMutate) {
    const ServerSnapshot snapshot = editing_on_station_a();

    const std::string on_ap =
        status_body(snapshot, Surface::SetupAccessPoint, kVersion, CsrfToken{kToken});
    EXPECT_TRUE(contains(on_ap, std::string{"\"csrfToken\":\""} + std::string{kToken} + "\""));
    EXPECT_TRUE(contains(on_ap, "\"canConfigure\":true"));

    const std::string on_lan =
        status_body(snapshot, Surface::StationLan, kVersion, CsrfToken{kToken});
    EXPECT_TRUE(contains(on_lan, "\"csrfToken\":\"\""));
    EXPECT_TRUE(contains(on_lan, "\"canConfigure\":false"));
    EXPECT_FALSE(contains(on_lan, kToken));

    // An unidentified surface is the most restrictive one, not the most permissive.
    const std::string unknown =
        status_body(snapshot, Surface::Unknown, kVersion, CsrfToken{kToken});
    EXPECT_TRUE(contains(unknown, "\"canConfigure\":false"));
    EXPECT_FALSE(contains(unknown, kToken));
}

TEST(ServerViewSurfaceTests, InstallActionRequiresAvailabilityAndTheAuthorizedSurface) {
    ServerSnapshot snapshot = editing_on_station_a();
    (void)snapshot.update_version.assign("1.2.3");

    const std::string setup =
        status_body(snapshot, Surface::SetupAccessPoint, kVersion, CsrfToken{kToken});
    EXPECT_TRUE(contains(setup, "\"updateVersion\":\"1.2.3\""));
    EXPECT_TRUE(contains(setup, "\"canInstall\":true"));

    const std::string lan = status_body(snapshot, Surface::StationLan, kVersion, CsrfToken{kToken});
    EXPECT_TRUE(contains(lan, "\"canInstall\":false"));

    snapshot.update_installing = true;
    const std::string installing =
        status_body(snapshot, Surface::SetupAccessPoint, kVersion, CsrfToken{kToken});
    EXPECT_TRUE(contains(installing, "\"canInstall\":false"));
    EXPECT_TRUE(contains(installing, "\"updateInstalling\":true"));
}

// --- Stage admission ----------------------------------------------------------------------------

// The printer stage requires a validated Wi-Fi stage. Refused here as well as in app_core, which is
// authoritative — this one gives the page a proper HTTP answer instead of a silent no-op and stops a
// caller reaching step two by skipping step one.
TEST(ServerViewAdmissionTests, ThePrinterStageRequiresAValidatedWifiStage) {
    EXPECT_TRUE(admits_printer_stage(Stage::Printer, SetupState::WifiValidated));

    EXPECT_FALSE(admits_printer_stage(Stage::Printer, SetupState::Editing));
    EXPECT_FALSE(admits_printer_stage(Stage::Printer, SetupState::Validating));
    EXPECT_FALSE(admits_printer_stage(Stage::Printer, SetupState::Succeeded));
    EXPECT_FALSE(admits_printer_stage(Stage::Printer, SetupState::Failed));
}

// The Wi-Fi stage is step one and is always admissible — D-6 keeps the form editable after a
// recoverable failure so another submission needs no reboot.
TEST(ServerViewAdmissionTests, TheWifiStageIsAlwaysAdmissible) {
    for (const SetupState state :
         {SetupState::Editing, SetupState::Validating, SetupState::WifiValidated,
          SetupState::Succeeded, SetupState::Failed}) {
        EXPECT_TRUE(admits_printer_stage(Stage::Wifi, state));
    }
}

// ⛔ THE ADMISSION AND THE RESPONSE COME FROM ONE INSTANT. A submission admitted against a state
// that has already moved is the failure this replaces: the state was read after the surfaces, so a
// transition between the two reads answered one request from two instants.
TEST(ServerViewAdmissionTests, AdmissionUsesTheSnapshotTheRequestWasAnsweredFrom) {
    ServerSnapshot published;
    published.surfaces.set_access_point(kAccessPoint);
    published.setup_state = SetupState::WifiValidated;

    const ServerSnapshot taken = published;

    // The application task settles the transaction as this request is being handled.
    published.setup_state = SetupState::Failed;
    (void)published.setup_reason.assign("printer-unreachable");

    // The in-flight request is admitted, because the state it took permits it.
    EXPECT_TRUE(admits_printer_stage(Stage::Printer, taken.setup_state));

    // A request arriving after the transition is refused, from the same one value that would build
    // its response body.
    EXPECT_FALSE(admits_printer_stage(Stage::Printer, published.setup_state));
    EXPECT_TRUE(
        contains(status_body(published, Surface::SetupAccessPoint, kVersion, CsrfToken{kToken}),
                 "\"setupState\":\"failed\""));
}

// --- Negative controls: what must not have changed -----------------------------------------------

// §5.11 and §14.4: the status response never returns a stored secret. Not the submitted SSID, not a
// passphrase, not an API key, and not the setup access point's own password.
TEST(ServerViewNegativeControlTests, TheStatusBodyStillCarriesNoStoredSecret) {
    ServerSnapshot snapshot = editing_on_station_a();
    snapshot.setup_state = SetupState::Failed;
    (void)snapshot.setup_reason.assign("wifi-unreachable");

    const std::string body =
        status_body(snapshot, Surface::SetupAccessPoint, kVersion, CsrfToken{kToken});

    // The only fields the page is given, and the whole set of them.
    for (const std::string_view field :
         {"\"product\"", "\"version\"", "\"csrfToken\"", "\"setupActive\"", "\"canConfigure\"",
          "\"setupState\"", "\"setupReason\""}) {
        EXPECT_TRUE(contains(body, field)) << field;
    }

    for (const std::string_view forbidden :
         {"wifiSsid", "wifiPassword", "printerApiKey", "password", "apPassword", "ssid"}) {
        EXPECT_FALSE(contains(body, forbidden)) << forbidden;
    }

    // Nor does it name the interface a caller arrived on: it reports a capability, not a topology.
    EXPECT_FALSE(contains(body, "station-lan"));
    EXPECT_FALSE(contains(body, kStationA));
    EXPECT_FALSE(contains(body, kHostnameA));
    EXPECT_FALSE(contains(body, kAccessPoint));
}

// setupActive is unconditional: the read-only LAN surface is permanent (ruling R6), and the page
// distinguishes what it may do through canConfigure rather than through this flag.
TEST(ServerViewNegativeControlTests, SetupActiveIsStillUnconditional) {
    for (const Surface arrival :
         {Surface::SetupAccessPoint, Surface::StationLan, Surface::Unknown}) {
        const std::string body =
            status_body(editing_on_station_a(), arrival, kVersion, CsrfToken{kToken});
        EXPECT_TRUE(contains(body, "\"setupActive\":true"));
    }
}

// The reason is escaped like every other value that leaves the device (§14.4). It is a fixed label
// today, so this asserts the mechanism rather than a reachable input.
TEST(ServerViewNegativeControlTests, TheReasonIsStillEscaped) {
    ServerSnapshot snapshot = editing_on_station_a();
    snapshot.setup_state = SetupState::Failed;
    (void)snapshot.setup_reason.assign("a\"b\\c");

    const std::string body =
        status_body(snapshot, Surface::SetupAccessPoint, kVersion, CsrfToken{kToken});
    EXPECT_TRUE(contains(body, "\"setupReason\":\"a\\\"b\\\\c\""));
}

// A default snapshot recognises no privileged setup-access-point surface and authorises nothing —
// the state a handler falls back to if it ever finds the channel empty.
//
// ⬅️ AND StationLan BELOW IS THE CORRECT EXPECTATION, NOT A LOOSE ONE. classify() is given an
// address it resolved, so it answers conservatively: anything that is not the setup access point's
// address is Surface::StationLan. Surface::Unknown belongs to the other failure — the socket or
// local-address lookup itself failing — which is why it appears further down as the arrival passed
// to status_body(). permits_mutation() refuses both, so the fallback is fail-closed either way.
TEST(ServerViewNegativeControlTests, ADefaultSnapshotAuthorisesNothing) {
    const ServerSnapshot snapshot;

    EXPECT_EQ(snapshot.setup_state, SetupState::Editing);
    EXPECT_TRUE(snapshot.setup_reason.empty());
    EXPECT_EQ(snapshot.surfaces.classify(kAccessPoint), Surface::StationLan);
    EXPECT_FALSE(
        snapshot.surfaces.allows_origin(std::string{"http://"} + std::string{kAccessPoint}));

    // Nothing can be mutated through it, and the printer stage cannot be reached.
    EXPECT_FALSE(admits_printer_stage(Stage::Printer, snapshot.setup_state));
    EXPECT_TRUE(contains(status_body(snapshot, Surface::Unknown, kVersion, CsrfToken{kToken}),
                         "\"canConfigure\":false"));
}

// The snapshot must be copyable by value with no heap behind it, because that is what lets it cross
// a FreeRTOS queue as one item — the property the std::string setup reason did not have.
TEST(ServerViewNegativeControlTests, TheSnapshotIsATriviallyCopyableValue) {
    static_assert(std::is_trivially_copyable<ServerSnapshot>::value,
                  "a snapshot crosses a queue by value; nothing in it may own heap");

    const ServerSnapshot original = failed_on_station_b();
    const ServerSnapshot copy = original;

    EXPECT_EQ(copy.setup_state, original.setup_state);
    EXPECT_EQ(copy.setup_reason.view(), original.setup_reason.view());
    EXPECT_EQ(copy.surfaces.station(), original.surfaces.station());
    EXPECT_EQ(copy.surfaces.hostname(), original.surfaces.hostname());
}

// --- What POST /api/setup may answer — checkpoint 3d ----------------------
//
// ⛔ THE ONE PROPERTY THAT MUST NEVER REGRESS. Before this checkpoint the handler decided
// `"accepted": true` from the grammar alone and called the sink afterwards, so a refused handoff
// still produced a 200 claiming a validation had begun — and the browser then waited for a result
// that was never coming. These tests make the rule a property rather than a consequence of which
// statement comes first.

TEST(SubmitAnswerTests, AValidatedSubmissionThatWasTakenMayReportAcceptance) {
    EXPECT_EQ(submit_answer(true, SubmissionHandoff::Accepted), SubmitAnswer::Accepted);
}

TEST(SubmitAnswerTests, AValidatedSubmissionThatWasNotTakenIsUnavailable) {
    EXPECT_EQ(submit_answer(true, SubmissionHandoff::Unavailable), SubmitAnswer::Unavailable);
}

TEST(SubmitAnswerTests, AnInvalidBodyIsReportedAndNeverAccepted) {
    // The handoff value is meaningless here because the sink was never called. Neither value may
    // turn a failed validation into an acceptance.
    EXPECT_EQ(submit_answer(false, SubmissionHandoff::Accepted), SubmitAnswer::Report);
    EXPECT_EQ(submit_answer(false, SubmissionHandoff::Unavailable), SubmitAnswer::Report);
}

TEST(SubmitAnswerTests, AcceptanceIsUnreachableWithoutAnAcceptedHandoff) {
    // The exhaustive statement of the invariant: across every input pair, the only route to
    // SubmitAnswer::Accepted passes through SubmissionHandoff::Accepted.
    for (const bool validated : {false, true}) {
        for (const SubmissionHandoff handoff :
             {SubmissionHandoff::Accepted, SubmissionHandoff::Unavailable}) {
            if (submit_answer(validated, handoff) == SubmitAnswer::Accepted) {
                EXPECT_TRUE(validated);
                EXPECT_EQ(handoff, SubmissionHandoff::Accepted);
            }
        }
    }
}

TEST(SubmitAnswerTests, TheUnavailableBodyIsFixedAndDisclosesNothing) {
    EXPECT_EQ(kUnavailableBody, "{\"accepted\":false,\"error\":\"unavailable\"}");

    // It says the request was not accepted, and it names no submitted field at all — so there is no
    // path by which §13.5's forbidden values could appear in a 503.
    EXPECT_TRUE(contains(std::string{kUnavailableBody}, "\"accepted\":false"));
    EXPECT_FALSE(contains(std::string{kUnavailableBody}, "\"accepted\":true"));

    for (const std::string_view field :
         {"wifiSsid", "wifiPassword", "printerHost", "printerApiKey", "printerPort"}) {
        EXPECT_FALSE(contains(std::string{kUnavailableBody}, field)) << field;
    }
}

TEST(ServerViewNegativeControlTests, TheAcceptanceAssertionCanFail) {
    // ⛔ WITHOUT THIS THE FOUR TESTS ABOVE COULD BE VACUOUS. `submit_answer` genuinely returns
    // different answers for different inputs, so a passing suite means the mapping was exercised
    // rather than that every input happened to agree.
    EXPECT_NE(submit_answer(true, SubmissionHandoff::Accepted),
              submit_answer(true, SubmissionHandoff::Unavailable));
    EXPECT_NE(submit_answer(true, SubmissionHandoff::Accepted),
              submit_answer(false, SubmissionHandoff::Accepted));

    // And the body check discriminates: a body that DID claim acceptance would fail it.
    constexpr std::string_view dishonest = "{\"accepted\":true}";
    EXPECT_TRUE(contains(std::string{dishonest}, "\"accepted\":true"));
    EXPECT_FALSE(contains(std::string{dishonest}, "\"accepted\":false"));
}

} // namespace
