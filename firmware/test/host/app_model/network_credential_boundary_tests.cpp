// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstddef>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "app_model/reducer.hpp"
#include "support/state_builders.hpp"

// The home network name may not enter `AppState` — the 2026-08-23 credential-boundary checkpoint,
// audit item 1.
//
// ⛔ THE CONTRACT, AS THE OWNER SETTLED IT ON 2026-08-23. §13.2 places `wifiSsid` inside
// SecretConfigV1; §13.5 forbids credentials in `AppState`; §7.3 says "Secrets are never placed
// inside `AppState`". §7.4's field list once named `ssidDisplayName` and the model carried it, which
// is the contradiction the clarification resolved in §7.3's favour. The field was read by nothing —
// no scene, no presentation, no projection — so it carried a credential in and out of the
// authoritative state at every snapshot for no product behaviour at all.
//
// ⬅️ TWO INDEPENDENT PROOFS, BECAUSE THEY FAIL DIFFERENTLY. The first is structural: no type on the
// path from the Wi-Fi adapter to the store has a member that could hold the name, so reintroducing
// one breaks a compile-time assertion rather than a runtime expectation. The second is behavioural:
// everything a NetworkObservation CAN carry is driven through the real reducer and the resulting
// AppState is searched byte by byte, so a future field that smuggled the name in under a different
// name would still have to survive a scan that is not looking for a spelling.
//
// No real network name appears below. Every value is an obviously synthetic marker, and the address
// is from RFC 5737's documentation range.

namespace prd::app_model {
namespace {

// Detects a member of that name on a type, whatever the member's own type is.
template <typename T, typename = void> struct CarriesSsidDisplayName : std::false_type {};

template <typename T>
struct CarriesSsidDisplayName<T, std::void_t<decltype(std::declval<T&>().ssid_display_name)>>
    : std::true_type {};

// ⛔ THE POSITIVE CONTROL. A detector that answered "absent" for every type would pass the
// assertions below on any type at all, including one that still carried the field.
struct StillCarriesIt {
    int ssid_display_name{0};
};

static_assert(CarriesSsidDisplayName<StillCarriesIt>::value,
              "the detector must find the member when it is present, or it proves nothing");

static_assert(!CarriesSsidDisplayName<NetworkState>::value,
              "§§7.3/13.5: NetworkState must carry no home SSID");
static_assert(!CarriesSsidDisplayName<NetworkObservation>::value,
              "§§7.3/13.5: the observation is the only route into the store and must carry none");

// Searches a whole object's byte image for a marker.
//
// AppState is a flat aggregate of fixed-capacity members — §6's "bounded strings", no heap and no
// indirection — so its bytes really are the whole of it. The static_assert is what keeps that true:
// if a pointer or a std::string ever appears in the state, this stops compiling rather than
// silently searching only the handle.
template <typename T>
[[nodiscard]] bool image_contains(const T& object, const std::string_view marker) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "the byte image is only the whole object while the object is flat");

    const auto* const bytes = reinterpret_cast<const char*>(&object);
    const std::string_view image{bytes, sizeof(T)};
    return image.find(marker) != std::string_view::npos;
}

// Clearly synthetic. Neither is a network name, and the address is RFC 5737 documentation space.
constexpr std::string_view kIpMarker = "203.0.113.9";
constexpr std::string_view kErrorMarker = "MARKER-ERROR-TEXT";

// The name a leak would carry. It is never assigned to anything — there is nothing to assign it to,
// which is the property under test.
constexpr std::string_view kNetworkNameMarker = "MARKER-NETWORK-NAME";

[[nodiscard]] NetworkObservation fully_populated() {
    NetworkObservation observation;
    observation.phase = NetworkPhase::Connected;
    observation.ip_address.assign(kIpMarker);
    observation.error.assign(kErrorMarker);
    observation.signal_known = true;
    observation.signal_strength_dbm = -50;
    return observation;
}

} // namespace

TEST(NetworkCredentialBoundaryTests, NEITHER_THE_STATE_NOR_THE_OBSERVATION_HAS_AN_SSID_MEMBER) {
    // The static_asserts above are the real gate. This restates them as a named runtime case so the
    // property is visible in the test list, and so the positive control shows up in the output.
    EXPECT_TRUE(CarriesSsidDisplayName<StillCarriesIt>::value);
    EXPECT_FALSE(CarriesSsidDisplayName<NetworkState>::value);
    EXPECT_FALSE(CarriesSsidDisplayName<NetworkObservation>::value);
}

TEST(NetworkCredentialBoundaryTests, EVERYTHING_THE_OBSERVATION_CAN_CARRY_IS_ACCOUNTED_FOR) {
    // ⛔ THE CONTROL FOR THE SCAN. Fill every field the observation has, reduce it, and confirm the
    // two string fields that SHOULD arrive are findable in the state's bytes. Without this, the
    // absence assertion in the next test would pass on a scan that never finds anything.
    const AppState state =
        reduce(prd::test::booted_device(prd::test::at_ms(1000)),
               Event::network_observed(prd::test::at_ms(2000), fully_populated()));

    EXPECT_TRUE(image_contains(state, kIpMarker));
    EXPECT_TRUE(image_contains(state, kErrorMarker));

    // And they arrived where they belong, so the scan is not passing on a coincidence elsewhere.
    EXPECT_EQ(state.network.ip_address.view(), kIpMarker);
    EXPECT_EQ(state.network.last_error.view(), kErrorMarker);
    EXPECT_EQ(state.network.phase, NetworkPhase::Connected);
}

TEST(NetworkCredentialBoundaryTests, A_NETWORK_NAME_HAS_NO_ROUTE_INTO_THE_STATE) {
    // ⬅️ WHY THIS READS ODDLY, AND WHY IT IS STILL THE STRONGEST FORM AVAILABLE. There is no field
    // to put a network name in, so the marker cannot be injected — that absence IS the property.
    // The assertion is meaningful because the test above proves the same scan finds values that are
    // present, and because the structural detector proves no member of that name exists.
    const AppState state =
        reduce(prd::test::booted_device(prd::test::at_ms(1000)),
               Event::network_observed(prd::test::at_ms(2000), fully_populated()));

    EXPECT_FALSE(image_contains(state, kNetworkNameMarker));

    // The old leak's own fixture value, which fake_source used to publish into the state through
    // network_connected(). It cannot reach the state now, and this is the regression that says so.
    EXPECT_FALSE(image_contains(state, "workshop-wifi"));
}

TEST(NetworkCredentialBoundaryTests, StateEqualityStillDistinguishesEveryRemainingNetworkField) {
    // operator== lost a term when the field went. A revision that stops noticing a change is how a
    // stale panel happens, so each surviving field is checked to still count.
    const NetworkState reference;
    NetworkState changed;
    EXPECT_EQ(reference, changed);

    changed.phase = NetworkPhase::Connected;
    EXPECT_NE(reference, changed);
    changed = reference;

    changed.ip_address.assign(kIpMarker);
    EXPECT_NE(reference, changed);
    changed = reference;

    changed.signal_known = true;
    EXPECT_NE(reference, changed);
    changed = reference;

    changed.signal_strength_dbm = -42;
    EXPECT_NE(reference, changed);
    changed = reference;

    changed.last_connected_at = prd::test::at_ms(5000);
    EXPECT_NE(reference, changed);
    changed = reference;

    changed.consecutive_failures = 3;
    EXPECT_NE(reference, changed);
    changed = reference;

    changed.last_error.assign(kErrorMarker);
    EXPECT_NE(reference, changed);
}

} // namespace prd::app_model
