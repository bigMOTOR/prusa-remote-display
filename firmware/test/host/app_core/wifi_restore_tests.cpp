// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "provisioning_flow.hpp"
#include "support/fake_key_value.hpp"
#include "wifi_restore.hpp"

// Which pair the radio must be put back to when a provisioning candidate is abandoned — §13.3
// step 7's "retain or restore previous valid configuration on failure", for the radio.
//
// ⛔ WHY THIS UNIT EXISTS AND WHY IT IS TESTED HERE RATHER THAN READ. §13.3's staged pattern already
// obtains the "retain" half by construction: no failing transaction moves an activation pointer, so
// flash keeps the previous pair whatever happens. What no code did was put the RADIO back —
// begin_transaction() hands the candidate's credential to the driver before anything proves it — so a
// failed or abandoned session left the station joined to a network the device had not persisted and
// would silently leave on the next boot. app_core.cpp cannot be compiled by this suite, so the
// DECISION lives here where every branch is reachable, and the binding is read.
//
// ⛔ AND THE ONE MISTAKE THIS MUST NOT REPEAT. candidate.hpp records what the previous design cost:
// it reconstructed a missing generation as `active + 1` and moved a pointer to a generation no
// record occupied. Nothing here derives a generation. The controls below assert that directly rather
// than trusting the absence of arithmetic to be visible in review.

namespace prd::app_core {
namespace {

using wifi_restore::Action;
using wifi_restore::Credential;
using wifi_restore::Target;

constexpr const char* kConfigPointer = "cfg.gen";
constexpr const char* kSecretPointer = "sec.gen";

[[nodiscard]] const char* config_slot(const std::uint32_t generation) {
    return generation % 2 == 0 ? "cfg.0" : "cfg.1";
}

[[nodiscard]] const char* secret_slot(const std::uint32_t generation) {
    return generation % 2 == 0 ? "sec.0" : "sec.1";
}

[[nodiscard]] provisioning_flow::Submission submission_for(const char* ssid, const char* password) {
    provisioning_flow::Submission submission;
    submission.stage = provisioning_flow::Stage::Printer;
    (void)submission.wifi_ssid.assign(ssid);
    (void)submission.wifi_password.assign(password);
    (void)submission.printer_host.assign("192.168.1.10");
    (void)submission.printer_api_key.assign("an-api-key");
    submission.printer_port = 80;
    return submission;
}

[[nodiscard]] provisioning_flow::Records records_for(const char* ssid, const char* password,
                                                     const std::uint32_t profile) {
    provisioning_flow::Records records =
        provisioning_flow::compose(submission_for(ssid, password), config::defaults(), profile);
    records.configuration.printer_validated = true;
    return records;
}

// A device that already works: one whole activated pair. Every case below is about what happens to
// the radio of THIS device when a later candidate is abandoned.
provisioning_flow::StagedPair seed_working_device(test::FakeKeyValue& store, const char* ssid,
                                                  const char* password,
                                                  const std::uint32_t profile = 1) {
    const provisioning_flow::StagedPair staged =
        provisioning_flow::stage_pair(store, records_for(ssid, password, profile));

    EXPECT_TRUE(staged.staged);
    EXPECT_TRUE(provisioning_flow::activate_pair(store, staged).activated);
    return staged;
}

// The candidate a user submitted and that is about to be abandoned. Staged only: no pointer moves,
// which is exactly the state every restoration path starts from.
provisioning_flow::StagedPair stage_candidate(test::FakeKeyValue& store) {
    const provisioning_flow::StagedPair staged =
        provisioning_flow::stage_pair(store, records_for("Candidate", "candidatepass", 2));

    EXPECT_TRUE(staged.staged);
    return staged;
}

// --- The target ------------------------------------------------------------

TEST(WifiRestoreTargetTests, AWholeActivePairIsTheRestoreTargetAtItsExactGenerations) {
    test::FakeKeyValue store;
    const provisioning_flow::StagedPair working =
        seed_working_device(store, "HomeNetwork", "homepassword");

    const Target target = wifi_restore::target_for(store);

    EXPECT_EQ(target.action, Action::Restore);
    EXPECT_EQ(target.profile, 1U);

    // ⛔ THE EXACT GENERATIONS THE WORKING PAIR WAS WRITTEN AT, WHICH IS THE POINT. They are the same
    // numbers stage_pair() reports and activate_pair() writes into the recovery record, so the
    // firmware has ONE notion of "the pair that was active" rather than two that can drift.
    EXPECT_EQ(target.configuration_generation, working.configuration_generation);
    EXPECT_EQ(target.secret_generation, working.secret_generation);

    const Credential credential = wifi_restore::credential_for(store, target);

    EXPECT_TRUE(credential.loaded);
    EXPECT_EQ(credential.ssid.view(), "HomeNetwork");
    EXPECT_EQ(credential.password.view(), "homepassword");
}

TEST(WifiRestoreTargetTests, AStagedCandidateDoesNotBecomeTheRestoreTarget) {
    // ⛔ THE CASE THE WHOLE FEATURE IS FOR. A candidate is in flash and inactive; the radio is
    // holding its credential. The restore target must be the pair the device was RUNNING, and the
    // two are distinguishable only by which generation the pointers name — never by which number is
    // larger, which is the arithmetic this replaces.
    test::FakeKeyValue store;
    const provisioning_flow::StagedPair working =
        seed_working_device(store, "HomeNetwork", "homepassword");
    const provisioning_flow::StagedPair candidate = stage_candidate(store);

    ASSERT_NE(candidate.configuration_generation, working.configuration_generation);
    ASSERT_NE(candidate.secret_generation, working.secret_generation);

    const Target target = wifi_restore::target_for(store);

    EXPECT_EQ(target.action, Action::Restore);
    EXPECT_EQ(target.profile, 1U) << "the previous profile, not the candidate's";
    EXPECT_EQ(target.configuration_generation, working.configuration_generation);
    EXPECT_EQ(target.secret_generation, working.secret_generation);

    const Credential credential = wifi_restore::credential_for(store, target);

    EXPECT_TRUE(credential.loaded);
    EXPECT_EQ(credential.ssid.view(), "HomeNetwork") << "not the candidate's network";
    EXPECT_EQ(credential.password.view(), "homepassword");
}

TEST(WifiRestoreTargetTests, TheGenerationsAreReadFromTheStoreAndNeverDerived) {
    // ⛔ THE NO-ARITHMETIC CONTROL, AND IT IS BUILT SO THAT `active + 1` AND `active - 1` BOTH NAME A
    // REAL RECORD. Two activations plus a staged third mean three generations exist, so an
    // implementation that guessed a neighbour would still find bytes to load and would still report
    // a whole pair — which is precisely how the previous design passed review.
    test::FakeKeyValue store;
    seed_working_device(store, "First", "firstpass", 1);
    const provisioning_flow::StagedPair second =
        seed_working_device(store, "Second", "secondpass", 2);
    const provisioning_flow::StagedPair third = stage_candidate(store);

    const Target target = wifi_restore::target_for(store);

    ASSERT_EQ(target.action, Action::Restore);
    EXPECT_EQ(target.configuration_generation, second.configuration_generation);
    EXPECT_EQ(target.secret_generation, second.secret_generation);

    EXPECT_NE(target.configuration_generation, second.configuration_generation + 1);
    EXPECT_NE(target.configuration_generation, second.configuration_generation - 1);
    EXPECT_NE(target.configuration_generation, third.configuration_generation);
    EXPECT_NE(target.secret_generation, third.secret_generation);

    const Credential credential = wifi_restore::credential_for(store, target);
    EXPECT_TRUE(credential.loaded);
    EXPECT_EQ(credential.ssid.view(), "Second");
}

TEST(WifiRestoreTargetTests, AFirstProvisioningHasNoPairAndIsWithdrawnRatherThanFailed) {
    // PM ruling 1 of checkpoint 3e. There is nothing to go back to and nothing at risk, so taking
    // the station down is the prescribed OUTCOME — not a failure to report.
    test::FakeKeyValue virgin;

    const Target target = wifi_restore::target_for(virgin);

    EXPECT_EQ(target.action, Action::Withdraw);
    EXPECT_EQ(target.profile, 0U);
    EXPECT_EQ(target.configuration_generation, config::kNoGeneration);
    EXPECT_EQ(target.secret_generation, secrets::kNoGeneration);

    // And even a virgin store's candidate does not create one: staging moves no pointer.
    test::FakeKeyValue staged;
    (void)stage_candidate(staged);
    EXPECT_EQ(wifi_restore::target_for(staged).action, Action::Withdraw);
}

TEST(WifiRestoreTargetTests, AMissingHalfIsNoPairRatherThanAFailure) {
    // Half a pair is not a pair, and the profile-id binding is what says so —
    // configuration_observation.cpp already presents such a device as un-provisioned. Both
    // directions, because a rule that held for one half would be a rule about that half.
    {
        test::FakeKeyValue store;
        const provisioning_flow::StagedPair working =
            seed_working_device(store, "HomeNetwork", "homepassword");

        store.forget(storage::Namespace::Secrets, secret_slot(working.secret_generation));
        store.forget(storage::Namespace::System, kSecretPointer);

        const Target target = wifi_restore::target_for(store);
        EXPECT_EQ(target.action, Action::Withdraw);
        EXPECT_EQ(target.profile, 0U);
    }

    {
        test::FakeKeyValue store;
        const provisioning_flow::StagedPair working =
            seed_working_device(store, "HomeNetwork", "homepassword");

        store.forget(storage::Namespace::Config, config_slot(working.configuration_generation));
        store.forget(storage::Namespace::System, kConfigPointer);

        const Target target = wifi_restore::target_for(store);
        EXPECT_EQ(target.action, Action::Withdraw);
    }
}

TEST(WifiRestoreTargetTests, AnOrphanCredentialFromAnotherProfileIsNoPair) {
    // A record left behind by a previous provisioning carries the old handle. Counting it would make
    // a device restore onto a network its configuration does not name — the same reason
    // configuration_observation.cpp refuses to call it provisioned.
    test::FakeKeyValue store;
    const provisioning_flow::StagedPair working =
        seed_working_device(store, "HomeNetwork", "homepassword", 1);

    secrets::SecretConfigV1 orphan;
    orphan.profile_id = 7;
    (void)orphan.wifi_ssid.assign("OtherNetwork");
    (void)orphan.wifi_password.assign("otherpassword");

    const secrets::StageReport restaged = secrets::stage(store, orphan);
    ASSERT_EQ(restaged.outcome, secrets::SaveOutcome::Committed);
    ASSERT_EQ(secrets::activate(store, restaged.generation), secrets::SaveOutcome::Committed);

    ASSERT_EQ(config::load(store).value.wifi_profile_id, 1U);

    const Target target = wifi_restore::target_for(store);

    EXPECT_EQ(target.action, Action::Withdraw) << "two records that disagree name no pair";
    EXPECT_EQ(target.profile, 0U);
    EXPECT_NE(target.secret_generation, restaged.generation);
    EXPECT_NE(target.configuration_generation, working.configuration_generation);
}

TEST(WifiRestoreTargetTests, APairThatNamesNoNetworkIsUNREPRESENTABLEWhichIsWhyTheGuardIsThere) {
    // ⛔ THIS TEST EXISTS TO PIN AN ASSUMPTION, NOT A BRANCH. target_for() requires the credential
    // record to actually name a network before it will call a pair restorable — the same predicate
    // configuration_observation.cpp reads before it will call a device provisioned. That check is
    // currently unreachable, and this is why: the credential schema refuses the record outright.
    //
    // ⬅️ SO WHY KEEP THE CHECK. Because the thing making it unreachable is a rule in a DIFFERENT
    // component, and if that rule is ever relaxed the restore path must not be the place that finds
    // out. The assertions below are what will tell whoever relaxes it that this guard has become
    // load-bearing.
    secrets::SecretConfigV1 nameless;
    nameless.profile_id = 1;
    (void)nameless.printer_api_key.assign("an-api-key");

    test::FakeKeyValue store;
    (void)seed_working_device(store, "HomeNetwork", "homepassword", 1);

    const secrets::StageReport refused = secrets::stage(store, nameless);

    EXPECT_EQ(refused.outcome, secrets::SaveOutcome::Rejected)
        << "a record that claims a profile must name a network; §13.3 step 1 refuses it";
    EXPECT_EQ(refused.generation, secrets::kNoGeneration) << "and nothing was written";

    // The working pair is therefore still the target, unchanged by the refused write.
    const Target target = wifi_restore::target_for(store);
    EXPECT_EQ(target.action, Action::Restore);
    EXPECT_EQ(target.profile, 1U);

    // NEGATIVE CONTROL: the mirror record — a network name and no profile — is refused too, so
    // neither half of the pairing rule can be satisfied alone. Without this, the assertion above
    // would also pass an implementation that rejected every credential record.
    secrets::SecretConfigV1 unassociated;
    (void)unassociated.wifi_ssid.assign("HomeNetwork");
    EXPECT_EQ(secrets::stage(store, unassociated).outcome, secrets::SaveOutcome::Rejected);

    // And a well-formed one is accepted, which is what makes both refusals meaningful.
    secrets::SecretConfigV1 wellformed;
    wellformed.profile_id = 3;
    (void)wellformed.wifi_ssid.assign("Another");
    EXPECT_EQ(secrets::stage(store, wellformed).outcome, secrets::SaveOutcome::Committed);
}

TEST(WifiRestoreTargetTests, AStoreThatWillNotAnswerIsUnavailableRatherThanEmpty) {
    // ⛔ THE DISCRIMINATOR THAT MAKES THREE ANSWERS NECESSARY. "Nothing is stored" and "the store
    // would not say" are different facts, and conflating them would report a first provisioning on a
    // device whose configuration is merely unreadable — then withdraw its station and call that the
    // prescribed outcome. Both halves, and both pointer and record reads.
    for (int victim = 0; victim < 4; ++victim) {
        test::FakeKeyValue store;
        const provisioning_flow::StagedPair working =
            seed_working_device(store, "HomeNetwork", "homepassword");

        switch (victim) {
        case 0:
            store.fail_read(storage::Namespace::System, kConfigPointer);
            break;
        case 1:
            store.fail_read(storage::Namespace::System, kSecretPointer);
            break;
        case 2:
            store.fail_read(storage::Namespace::Config,
                            config_slot(working.configuration_generation));
            break;
        default:
            store.fail_read(storage::Namespace::Secrets, secret_slot(working.secret_generation));
            break;
        }

        const Target target = wifi_restore::target_for(store);

        EXPECT_EQ(target.action, Action::Unavailable) << "victim " << victim;
        EXPECT_EQ(target.profile, 0U) << "victim " << victim;
        EXPECT_EQ(target.configuration_generation, config::kNoGeneration) << "victim " << victim;
    }
}

TEST(WifiRestoreTargetTests, ACorruptHalfIsUnavailableRatherThanAWithdrawal) {
    // Bit rot in an active record. Whether the loader reports it as invalid outright or falls back
    // to the previous known-good generation, the answer is the same: this firmware cannot name the
    // pair that was active, so a restore was required and cannot be performed.
    {
        test::FakeKeyValue store;
        const provisioning_flow::StagedPair working =
            seed_working_device(store, "HomeNetwork", "homepassword");

        std::vector<std::uint8_t> bytes =
            store.peek(storage::Namespace::Config, config_slot(working.configuration_generation));
        ASSERT_FALSE(bytes.empty());
        bytes[bytes.size() / 2] = static_cast<std::uint8_t>(bytes[bytes.size() / 2] ^ 0xFFU);
        store.poke(storage::Namespace::Config, config_slot(working.configuration_generation),
                   bytes);

        EXPECT_EQ(wifi_restore::target_for(store).action, Action::Unavailable);
    }

    {
        test::FakeKeyValue store;
        const provisioning_flow::StagedPair working =
            seed_working_device(store, "HomeNetwork", "homepassword");

        std::vector<std::uint8_t> bytes =
            store.peek(storage::Namespace::Secrets, secret_slot(working.secret_generation));
        ASSERT_FALSE(bytes.empty());
        bytes[bytes.size() / 2] = static_cast<std::uint8_t>(bytes[bytes.size() / 2] ^ 0xFFU);
        store.poke(storage::Namespace::Secrets, secret_slot(working.secret_generation), bytes);

        EXPECT_EQ(wifi_restore::target_for(store).action, Action::Unavailable);
    }
}

// --- Loading the credential ------------------------------------------------

TEST(WifiRestoreCredentialTests, NothingIsLoadedForAnActionOtherThanRestore) {
    test::FakeKeyValue store;
    (void)seed_working_device(store, "HomeNetwork", "homepassword");

    for (const Action action : {Action::Withdraw, Action::Unavailable}) {
        Target target;
        target.action = action;
        target.profile = 1;
        target.configuration_generation = 1;
        target.secret_generation = 1;

        const Credential credential = wifi_restore::credential_for(store, target);

        EXPECT_FALSE(credential.loaded);
        EXPECT_TRUE(credential.ssid.view().empty());
        EXPECT_TRUE(credential.password.view().empty());
    }
}

TEST(WifiRestoreCredentialTests, AGenerationThatNoLongerHoldsThePairIsRefused) {
    // The target is decided from the pointers and the load happens afterwards, so the two reads can
    // in principle disagree — a record that has become unreadable in between, or one that no longer
    // names the profile. The load refuses rather than handing back whatever it found.
    {
        test::FakeKeyValue store;
        const provisioning_flow::StagedPair working =
            seed_working_device(store, "HomeNetwork", "homepassword");

        const Target target = wifi_restore::target_for(store);
        ASSERT_EQ(target.action, Action::Restore);

        store.forget(storage::Namespace::Secrets, secret_slot(working.secret_generation));

        const Credential credential = wifi_restore::credential_for(store, target);
        EXPECT_FALSE(credential.loaded);
        EXPECT_TRUE(credential.ssid.view().empty());
    }

    {
        test::FakeKeyValue store;
        const provisioning_flow::StagedPair working =
            seed_working_device(store, "HomeNetwork", "homepassword");

        const Target target = wifi_restore::target_for(store);
        ASSERT_EQ(target.action, Action::Restore);

        store.forget(storage::Namespace::Config, config_slot(working.configuration_generation));

        const Credential credential = wifi_restore::credential_for(store, target);
        EXPECT_FALSE(credential.loaded)
            << "the configuration half is what makes the answer a PAIR rather than a credential";
    }

    {
        // A target naming a profile the records do not carry. Reachable only by a caller that built
        // a Target by hand, which is why the check is in the loader rather than left to the caller.
        test::FakeKeyValue store;
        (void)seed_working_device(store, "HomeNetwork", "homepassword");

        Target target = wifi_restore::target_for(store);
        ASSERT_EQ(target.action, Action::Restore);
        target.profile = 99;

        EXPECT_FALSE(wifi_restore::credential_for(store, target).loaded);
    }
}

// --- Credential handling ---------------------------------------------------
//
// ⛔ THE ALL-ZERO ORACLE IS A BYTE ARRAY, NEVER A SECOND `Credential`. `Credential` carries padding,
// and value-initialisation is not required to write a padding byte — so `Credential{}` has an
// UNSPECIFIED object representation and `memcmp` against it does not mean "every byte is zero". The
// two tests below asserted exactly that: they passed on the development machine, whose stack slot
// happened to hold zero, and failed on a CI runner whose did not, with `wifi_restore.hpp`,
// `wifi_restore.cpp` and this file all untouched by the commit that surfaced it. What `wipe()`
// actually provides is that the WHOLE object representation is overwritten, and only an array of
// `unsigned char` — a type with no padding of its own — can state that.
//
// AValueInitialisedCredentialIsNotAnAllZeroOracle reproduces the failure deterministically rather
// than leaving it to whatever the stack held.

using CredentialImage = std::array<unsigned char, sizeof(Credential)>;

[[nodiscard]] CredentialImage image_of(const Credential& credential) {
    CredentialImage image{};
    std::memcpy(image.data(), &credential, sizeof(credential));
    return image;
}

// The first byte that is not zero, or the image size when every byte is. Reported in the failure
// message, so residue is located rather than merely announced.
[[nodiscard]] std::size_t first_non_zero(const CredentialImage& image) {
    for (std::size_t index = 0; index < image.size(); ++index) {
        if (image[index] != 0) {
            return index;
        }
    }
    return image.size();
}

// Whether the object representation still carries `text` anywhere in it. A search over the BYTES,
// because "we did not copy it" is otherwise a claim about intentions — the same reason
// TheLoadedRecordsPrinterSecretsNeverLeaveTheStore searches rather than inspects.
[[nodiscard]] bool image_contains(const CredentialImage& image, const std::string_view text) {
    const std::string_view haystack{reinterpret_cast<const char*>(image.data()), image.size()};
    return haystack.find(text) != std::string_view::npos;
}

TEST(WifiRestoreCredentialTests, AWipedCredentialLeavesNoResidueAtAll) {
    // ⛔ CLEARING IS NOT WIPING, AND THAT IS THE WHOLE REASON THIS FUNCTION EXISTS.
    // BoundedString::clear() writes one terminator and leaves the rest of the buffer intact, so a
    // passphrase survives a "cleared" string. §13.5's "embedded in crash dumps where avoidable" is
    // avoidable here.
    test::FakeKeyValue store;
    (void)seed_working_device(store, "HomeNetwork", "homepassword");

    const Target target = wifi_restore::target_for(store);
    Credential credential = wifi_restore::credential_for(store, target);
    ASSERT_TRUE(credential.loaded);

    const CredentialImage zero{};
    const CredentialImage loaded = image_of(credential);

    // NEGATIVE CONTROL: the comparison must be able to fail, or the assertion after the wipe proves
    // nothing at all.
    ASSERT_NE(std::memcmp(loaded.data(), zero.data(), zero.size()), 0)
        << "a loaded credential must differ from an all-zero image";

    // ⬅️ AND IT MUST FAIL FOR THE RIGHT REASON. Against the retired `Credential{}` oracle a single
    // poisoned padding byte satisfied this control on its own, so it could hold with the passphrase
    // already absent: it proved the two objects differed, not that the residue this test exists to
    // remove was there to be removed.
    ASSERT_TRUE(image_contains(loaded, "homepassword"));

    wifi_restore::wipe(credential);

    const CredentialImage wiped = image_of(credential);

    EXPECT_EQ(std::memcmp(wiped.data(), zero.data(), zero.size()), 0)
        << "first non-zero byte at offset " << first_non_zero(wiped) << " of " << wiped.size();
    EXPECT_FALSE(credential.loaded);
    EXPECT_TRUE(credential.ssid.view().empty());
    EXPECT_TRUE(credential.password.view().empty());
}

TEST(WifiRestoreCredentialTests, AValueInitialisedCredentialIsNotAnAllZeroOracle) {
    // ⛔ THE DIAGNOSTIC FOR THE CI FAILURE THAT PRODUCED THE ORACLE ABOVE, MADE DETERMINISTIC.
    // `Credential` has padding; value-initialisation need not write it; the retired form of these
    // tests compared against `Credential{}`. Two objects whose every FIELD is equal could therefore
    // differ under `memcmp` — which is how the comparison passed on one machine and failed on
    // another with the production code identical on both.
    //
    // ⛔ IT NAMES NO OFFSET AND NO LAYOUT. The unwritten bytes are DISCOVERED, by poisoning the
    // storage before the object is placed in it, so this keeps its meaning if the layout changes.
    // The lesson is padding in general, not one byte in one ABI, and nothing here is a licence to
    // depend on that byte.
    static_assert(std::is_trivially_destructible_v<Credential>,
                  "the placement-new below would otherwise owe a destructor call");

    alignas(Credential) CredentialImage poisoned;
    poisoned.fill(0xA5U);

    const Credential* const oracle = new (poisoned.data()) Credential{};
    const auto* const oracle_bytes = reinterpret_cast<const unsigned char*>(oracle);

    std::size_t unwritten = 0;
    for (std::size_t index = 0; index < sizeof(Credential); ++index) {
        if (oracle_bytes[index] == 0xA5U) {
            ++unwritten;
        }
    }

    // The subject the retired comparison was applied to: a wiped credential, all-zero by
    // construction because wipe() overwrites the whole representation.
    test::FakeKeyValue store;
    (void)seed_working_device(store, "HomeNetwork", "homepassword");
    Credential subject = wifi_restore::credential_for(store, wifi_restore::target_for(store));
    ASSERT_TRUE(subject.loaded);
    wifi_restore::wipe(subject);

    // Every FIELD of the two agrees...
    ASSERT_EQ(subject.loaded, oracle->loaded);
    ASSERT_TRUE(subject.ssid == oracle->ssid);
    ASSERT_TRUE(subject.password == oracle->password);

    if (unwritten == 0) {
        // A toolchain that writes every byte makes the retired comparison accidentally correct.
        // Nothing to demonstrate here — and nothing to change either, because the array oracle is
        // right on both kinds of ABI. Recorded rather than skipped in silence.
        GTEST_LOG_(INFO) << "value-initialisation wrote all " << sizeof(Credential)
                         << " bytes on this toolchain";
    } else {
        // ...and the bytes do not. THIS is the CI failure, with the padding made determinate
        // instead of left to the stack.
        EXPECT_NE(std::memcmp(&subject, oracle, sizeof(Credential)), 0)
            << unwritten << " byte(s) were left unwritten by value-initialisation, and the retired "
            << "comparison could not tell they were not part of the value";
    }

    // The repaired oracle is indifferent to all of it.
    const CredentialImage zero{};
    EXPECT_EQ(std::memcmp(&subject, zero.data(), zero.size()), 0);
}

TEST(WifiRestoreCredentialTests, ClearingWouldHaveLeftResidueWhichIsWhyWipingIsUsed) {
    // The positive control for the sentence above: it asserts that the residue this unit avoids is
    // real, so nobody later "simplifies" wipe() into two clear() calls.
    Credential credential;
    (void)credential.password.assign("homepassword");

    const char* const raw = credential.password.c_str();
    ASSERT_EQ(std::string{raw}, "homepassword");

    credential.password.clear();
    EXPECT_TRUE(credential.password.view().empty());
    EXPECT_EQ(raw[1], 'o') << "clear() writes a terminator and leaves the rest of the buffer";

    wifi_restore::wipe(credential);
    EXPECT_EQ(raw[1], '\0');
}

TEST(WifiRestoreCredentialTests, ARefusedLoadCarriesNoPartialCredential) {
    // Every exit from credential_for() must leave nothing behind, including the ones that decided
    // the pair was not whole after all.
    //
    // ⛔ THE REFUSAL IS REACHED WITH THE SECRET RECORD ACTUALLY LOADED, WHICH IS THE ONLY VERSION OF
    // THIS THAT MEANS ANYTHING. The retired setup forgot the secret slot: the read failed, no
    // passphrase ever entered the process, and "the refusal carries no secret" was true because
    // there was never a secret to carry. Here the target names the CANDIDATE's secret generation —
    // a record that loads cleanly and carries its own profile id and its own passphrase — so the
    // refusal is the profile-mismatch branch, taken after `secret.value` held `candidatepass` in
    // this process. That is the path a residue could actually escape through.
    //
    // ⛔ AND IT DOES NOT REQUIRE BYTE IDENTITY WITH `Credential{}`, whose padding is unspecified.
    // The properties that matter are stated directly instead.
    test::FakeKeyValue store;
    (void)seed_working_device(store, "HomeNetwork", "homepassword");
    const provisioning_flow::StagedPair candidate = stage_candidate(store);

    Target target = wifi_restore::target_for(store);
    ASSERT_EQ(target.action, Action::Restore);
    ASSERT_EQ(target.profile, 1U);
    ASSERT_NE(target.secret_generation, candidate.secret_generation);

    // ⬅️ THAT THE RECORD REALLY IS LOADABLE AND REALLY DOES CARRY THE PASSPHRASE. Without this the
    // assertions below would be vacuous again for a new reason: an unreadable record refuses just
    // as quietly as a mismatched one.
    Target whole_candidate;
    whole_candidate.action = Action::Restore;
    whole_candidate.configuration_generation = candidate.configuration_generation;
    whole_candidate.secret_generation = candidate.secret_generation;
    whole_candidate.profile = 2;

    Credential loadable = wifi_restore::credential_for(store, whole_candidate);
    ASSERT_TRUE(loadable.loaded);
    ASSERT_EQ(loadable.ssid.view(), "Candidate");
    ASSERT_EQ(loadable.password.view(), "candidatepass");

    // FIRING CONTROL for the byte search below. A credential that DOES retain the seeded bytes is
    // detected — without it the four EXPECT_FALSEs could hold because the search never matches
    // anything.
    const CredentialImage retained = image_of(loadable);
    ASSERT_TRUE(image_contains(retained, "Candidate"));
    ASSERT_TRUE(image_contains(retained, "candidatepass"));

    wifi_restore::wipe(loadable);

    // The refusal: the same secret generation, under a target whose configuration half names
    // profile 1, so the loaded record's own profile id does not match it.
    target.secret_generation = candidate.secret_generation;

    const Credential refused = wifi_restore::credential_for(store, target);

    EXPECT_FALSE(refused.loaded);

    EXPECT_TRUE(refused.ssid.view().empty());
    EXPECT_FALSE(refused.ssid.truncated());
    EXPECT_TRUE(refused.password.view().empty());
    EXPECT_FALSE(refused.password.truncated());

    const CredentialImage image = image_of(refused);

    // Neither the pair the target named, nor the pair whose record was actually read.
    EXPECT_FALSE(image_contains(image, "HomeNetwork"));
    EXPECT_FALSE(image_contains(image, "homepassword"));
    EXPECT_FALSE(image_contains(image, "Candidate"));
    EXPECT_FALSE(image_contains(image, "candidatepass"));
}

TEST(WifiRestoreCredentialTests, TheLoadedRecordsPrinterSecretsNeverLeaveTheStore) {
    // The stored record also carries a printer API key and password. A restore has no use for
    // either, so neither is copied out — the fewer copies of a secret exist, the fewer there are to
    // wipe. Asserted on the bytes, because "we did not copy it" is otherwise a claim about
    // intentions.
    test::FakeKeyValue store;
    (void)seed_working_device(store, "HomeNetwork", "homepassword");

    const Target target = wifi_restore::target_for(store);
    Credential credential = wifi_restore::credential_for(store, target);
    ASSERT_TRUE(credential.loaded);

    const auto* const bytes = reinterpret_cast<const unsigned char*>(&credential);
    const std::string haystack{bytes, bytes + sizeof(credential)};

    EXPECT_EQ(haystack.find("an-api-key"), std::string::npos);

    wifi_restore::wipe(credential);
}

// --- Routing: the seam that used to be a hand-written condition ---------------
//
// ⛔ WHY THIS IS TESTED AT ALL. Until the repair, app_core.cpp discarded every station evidence
// record whenever no CANDIDATE transaction was running — three tokens of policy in a translation
// unit the host suite cannot compile — and that silently included every record an accepted
// restoration was waiting for. The whole of the defect is the missing RestoreSupervision branch, and
// this is where its absence now fails.

TEST(WifiRestoreRoutingTests, EveryCombinationHasOneDeterministicConsumer) {
    using wifi_restore::Route;
    using wifi_restore::Routing;

    // No record: nothing to route, whoever is listening.
    for (const bool transaction : {false, true}) {
        for (const bool restore : {false, true}) {
            EXPECT_EQ(wifi_restore::route_for(Routing{false, transaction, restore}),
                      Route::Discard);
        }
    }

    // A record and nobody listening: the Discard that keeps a stale answer out of the slot — R3's
    // original purpose, preserved.
    EXPECT_EQ(wifi_restore::route_for(Routing{true, false, false}), Route::Discard);

    // ⬅️ THE BRANCH WHOSE ABSENCE WAS THE DEFECT.
    EXPECT_EQ(wifi_restore::route_for(Routing{true, false, true}), Route::RestoreSupervision);

    EXPECT_EQ(wifi_restore::route_for(Routing{true, true, false}), Route::CandidateValidation);

    // Both active cannot happen in production — begin_transaction() abandons the supervisor before
    // it submits — but the router still answers deterministically, and the newer instruction wins.
    EXPECT_EQ(wifi_restore::route_for(Routing{true, true, true}), Route::CandidateValidation);
}

TEST(WifiRestoreRoutingNegativeControlTests, RestoreEvidenceCannotBeDiscarded) {
    // ⛔ THE CONTROL FOR FINDING 2. An implementation that dropped records while a restoration was
    // armed — which is exactly what the first version of this checkpoint did — cannot satisfy both of
    // these at once: the same inputs that route to the supervisor must NOT route to Discard.
    using wifi_restore::Route;
    using wifi_restore::Routing;

    const Routing armed{true, false, true};

    ASSERT_EQ(wifi_restore::route_for(armed), Route::RestoreSupervision);
    ASSERT_NE(wifi_restore::route_for(armed), Route::Discard);

    // And the discriminator is genuinely `restore_armed`: with it false the same record IS discarded,
    // so the assertion above is not vacuously true of every input.
    ASSERT_EQ(wifi_restore::route_for(Routing{true, false, false}), Route::Discard);
}

// --- The supervisor: acceptance is not completion ----------------------------

namespace {

using Result = wifi_restore::Supervisor::Result;
using SupervisorPhase = wifi_restore::Supervisor::Phase;
using prd::app_model::NetworkFault;
using prd::app_model::NetworkPhase;
using prd::network::station::kNoTag;
using prd::network::station::Outcome;
using prd::network::station::Tag;

// One step with no evidence at all, at a chosen published level.
Result step(wifi_restore::Supervisor& supervisor, const NetworkPhase phase,
            const NetworkFault fault = NetworkFault::None) {
    return supervisor.observe(phase, fault, false, kNoTag, Outcome::Leased);
}

// One step carrying an evidence record.
Result step_evidence(wifi_restore::Supervisor& supervisor, const NetworkPhase phase, const Tag tag,
                     const Outcome outcome, const NetworkFault fault = NetworkFault::None) {
    return supervisor.observe(phase, fault, true, tag, outcome);
}

} // namespace

TEST(WifiRestoreSupervisorTests, ArmingIsNeverCompletion) {
    // ⛔ FINDING 1, AS ONE ASSERTION. network::station::submit() reports the ACCEPTANCE of an
    // asynchronous intent; on the retained path not a single driver call has been issued at that
    // point. The first version of this checkpoint logged "rejoining the previously active pair" and
    // returned there.
    wifi_restore::Supervisor supervisor;

    EXPECT_FALSE(supervisor.armed());
    EXPECT_EQ(supervisor.phase(), SupervisorPhase::Idle);
    EXPECT_EQ(step(supervisor, NetworkPhase::Connected), Result::Pending)
        << "an unarmed supervisor settles nothing";

    supervisor.arm_restore(7);

    EXPECT_TRUE(supervisor.armed());
    EXPECT_EQ(supervisor.phase(), SupervisorPhase::Restoring);
    EXPECT_EQ(supervisor.tag(), Tag{7});

    // Every level the station could be at, with no terminal evidence: still Pending.
    for (const NetworkPhase phase :
         {NetworkPhase::Disabled, NetworkPhase::Starting, NetworkPhase::Connecting,
          NetworkPhase::Connected, NetworkPhase::Reconnecting, NetworkPhase::Unavailable}) {
        EXPECT_EQ(step(supervisor, phase), Result::Pending)
            << "phase " << static_cast<int>(phase) << " is not a restore verdict";
        EXPECT_TRUE(supervisor.armed());
    }

    // ⛔ AND NOT EVEN Connected IS ENOUGH. §7.4 carries an address; a station with a link and no
    // lease can reach nothing, so it has not been restored.
    EXPECT_EQ(step(supervisor, NetworkPhase::Connected), Result::Pending);
}

TEST(WifiRestoreSupervisorTests, ARetainedRestoreSucceedsOnlyOnItsOwnMatchingLease) {
    // The full lifecycle of the case the product actually takes: the submission is retained behind
    // the abandoned candidate's owed completion, the station then retires it, reconnects, associates
    // and leases. Only the last step is a success.
    wifi_restore::Supervisor supervisor;
    supervisor.arm_restore(9);

    // Retained: nothing issued yet, the old association still up.
    EXPECT_EQ(step(supervisor, NetworkPhase::Connected), Result::Pending);

    // The abandoned candidate's failure is the step that PERMITS the restore to be issued. It is
    // charged to the old identity and decides nothing here.
    EXPECT_EQ(step_evidence(supervisor, NetworkPhase::Connecting, 8, Outcome::Unreachable),
              Result::Pending)
        << "the superseded candidate's own failure is not the restore's verdict";
    EXPECT_TRUE(supervisor.armed());

    // Association without a lease.
    EXPECT_EQ(step(supervisor, NetworkPhase::Connecting), Result::Pending);

    // The lease, charged to this identity.
    EXPECT_EQ(step_evidence(supervisor, NetworkPhase::Connected, 9, Outcome::Leased),
              Result::Rejoined);

    EXPECT_FALSE(supervisor.armed()) << "a terminal result disarms";
    EXPECT_EQ(supervisor.tag(), kNoTag);
    EXPECT_EQ(step(supervisor, NetworkPhase::Connected), Result::Pending)
        << "and it settles exactly once";
}

TEST(WifiRestoreSupervisorTests, AnAcceptedRestoreThatEndsUnreachableIsTheRestoreFailure) {
    // ⛔ THE ONE ROUTE TO Failure::WifiRestoreFailed, per the PM's narrowing. §16.2 has already given
    // up inside the station, so the previous network is not coming back on its own.
    wifi_restore::Supervisor supervisor;
    supervisor.arm_restore(3);

    // A link that flaps is not a verdict — §11.3's ladder and §16.2's give-up bound how long this
    // may continue, which is why nothing here needs a deadline of its own.
    EXPECT_EQ(step_evidence(supervisor, NetworkPhase::Connecting, 3, Outcome::LeaseInterrupted),
              Result::Pending);
    EXPECT_TRUE(supervisor.armed());

    EXPECT_EQ(step_evidence(supervisor, NetworkPhase::Unavailable, 3, Outcome::Unreachable),
              Result::RestoreFailed);
    EXPECT_FALSE(supervisor.armed());
}

TEST(WifiRestoreSupervisorTests, EvidenceForAnotherOrStaleIdentityDecidesNothing) {
    // §8.1's "rejects stale responses using request generation or sequence IDs". Every outcome, under
    // every wrong identity, must leave the supervision exactly where it was.
    for (const Outcome outcome : {Outcome::Leased, Outcome::LeaseInterrupted, Outcome::Unreachable,
                                  Outcome::DriverStalled, Outcome::EvidenceLost}) {
        for (const Tag wrong : {kNoTag, Tag{1}, Tag{4}, Tag{99}}) {
            wifi_restore::Supervisor supervisor;
            supervisor.arm_restore(5);

            const Result result =
                step_evidence(supervisor, NetworkPhase::Connecting, wrong, outcome);

            EXPECT_EQ(result, Result::Pending)
                << "outcome " << static_cast<int>(outcome) << " under tag " << wrong;
            EXPECT_TRUE(supervisor.armed());
            EXPECT_EQ(supervisor.tag(), Tag{5});
        }
    }

    // NEGATIVE CONTROL: the SAME outcomes under the right identity do decide, so the assertions above
    // are about the identity and not about the outcomes being inert.
    {
        wifi_restore::Supervisor supervisor;
        supervisor.arm_restore(5);
        EXPECT_EQ(step_evidence(supervisor, NetworkPhase::Connected, 5, Outcome::Leased),
                  Result::Rejoined);
    }
    {
        wifi_restore::Supervisor supervisor;
        supervisor.arm_restore(5);
        EXPECT_EQ(step_evidence(supervisor, NetworkPhase::Unavailable, 5, Outcome::Unreachable),
                  Result::RestoreFailed);
    }
}

TEST(WifiRestoreSupervisorTests, AWithdrawalRemainsPendingUntilTheStationIsActuallyDisabled) {
    // ⛔ PM RULING: network::station::withdraw() returning true means accepted OR RETAINED, never
    // completed. The no-PreviousPair outcome is a success only once the station says Disabled.
    wifi_restore::Supervisor supervisor;
    supervisor.arm_withdrawal(provisioning_flow::Failure::None, false);

    EXPECT_EQ(supervisor.phase(), SupervisorPhase::Withdrawing);
    EXPECT_EQ(supervisor.tag(), kNoTag) << "a withdrawal produces no candidate evidence to match";

    for (const NetworkPhase phase :
         {NetworkPhase::Connected, NetworkPhase::Connecting, NetworkPhase::Reconnecting,
          NetworkPhase::Starting, NetworkPhase::Unavailable}) {
        EXPECT_EQ(step(supervisor, phase), Result::Pending)
            << "phase " << static_cast<int>(phase) << " is not down";
        EXPECT_TRUE(supervisor.armed());
    }

    // Candidate evidence cannot complete it either — a withdrawal is judged by the phase alone.
    EXPECT_EQ(step_evidence(supervisor, NetworkPhase::Connected, 2, Outcome::Leased),
              Result::Pending);

    EXPECT_EQ(step(supervisor, NetworkPhase::Disabled), Result::Withdrawn);
    EXPECT_FALSE(supervisor.armed());
}

TEST(WifiRestoreSupervisorTests, AWithdrawalIsJudgedAsALevelSoAnAlreadyDisabledStationSettles) {
    // ⛔ WHY THE PHASE IS A LEVEL AND NOT AN EDGE. A station with no intent is already Disabled when
    // the withdrawal is accepted, so station_intent publishes NO change at all and an
    // edge-triggered supervisor would wait for ever.
    wifi_restore::Supervisor supervisor;
    supervisor.arm_withdrawal(provisioning_flow::Failure::None, false);

    EXPECT_EQ(step(supervisor, NetworkPhase::Disabled), Result::Withdrawn);
}

TEST(WifiRestoreSupervisorTests, AWithdrawalCarryingRollbackUnavailableReportsThatInstead) {
    // Requirement 10: the retained exact record would not load after the radio had already been
    // disturbed. The candidate still has to come down, but the outcome is NOT a connectivity verdict
    // — nothing was ever attempted against the network.
    wifi_restore::Supervisor supervisor;
    supervisor.arm_withdrawal(provisioning_flow::Failure::WifiRollbackUnavailable, false);

    EXPECT_EQ(step(supervisor, NetworkPhase::Connected), Result::Pending);
    EXPECT_EQ(step(supervisor, NetworkPhase::Disabled), Result::RollbackUnavailable);

    // NEGATIVE CONTROL: the same sequence with no carry is the prescribed SUCCESS, so the two are
    // genuinely distinguished by the carried failure and not by the sequence.
    wifi_restore::Supervisor plain;
    plain.arm_withdrawal(provisioning_flow::Failure::None, false);
    EXPECT_EQ(step(plain, NetworkPhase::Disabled), Result::Withdrawn);
}

TEST(WifiRestoreSupervisorTests, ADriverFaultOrLostEvidenceEndsSupervisionSilently) {
    // ⛔ PM RULING 3. These are the conditions that already ARE the typed WifiUnavailable Device
    // Fault; a restore verdict on top would be a second, weaker statement about the same radio.
    for (const Outcome outcome : {Outcome::DriverStalled, Outcome::EvidenceLost}) {
        wifi_restore::Supervisor supervisor;
        supervisor.arm_restore(6);

        EXPECT_EQ(step_evidence(supervisor, NetworkPhase::Unavailable, 6, outcome),
                  Result::Abandoned);
        EXPECT_FALSE(supervisor.armed());
    }

    // The published fault is the same fact as a level, and it outranks everything — including a
    // lease that arrives in the same step, and including a withdrawal waiting for Disabled.
    {
        wifi_restore::Supervisor supervisor;
        supervisor.arm_restore(6);
        EXPECT_EQ(supervisor.observe(NetworkPhase::Connected, NetworkFault::DriverUnresponsive,
                                     true, 6, Outcome::Leased),
                  Result::Abandoned);
    }
    {
        wifi_restore::Supervisor supervisor;
        supervisor.arm_withdrawal(provisioning_flow::Failure::WifiRollbackUnavailable, false);
        EXPECT_EQ(step(supervisor, NetworkPhase::Disabled, NetworkFault::DriverUnresponsive),
                  Result::Abandoned)
            << "a quarantined station yields no restore vocabulary, not even a withdrawal's";
    }

    // NEGATIVE CONTROL: the ordinary faults do NOT abandon, so the assertion above is about
    // DriverUnresponsive rather than about any fault at all.
    for (const NetworkFault fault :
         {NetworkFault::None, NetworkFault::LinkLost, NetworkFault::CredentialsRejected}) {
        wifi_restore::Supervisor supervisor;
        supervisor.arm_restore(6);
        EXPECT_EQ(supervisor.observe(NetworkPhase::Connected, fault, true, 6, Outcome::Leased),
                  Result::Rejoined);
    }
}

TEST(WifiRestoreSupervisorTests, SupersessionAndExclusiveOperationBothEndItWithoutAVerdict) {
    // Requirement 8. A user submission is the newer instruction; a confirmed factory reset owns the
    // product state. Neither may publish a setup failure on the way out.
    for (const SupervisorPhase armed_as :
         {SupervisorPhase::Restoring, SupervisorPhase::Withdrawing}) {
        wifi_restore::Supervisor supervisor;

        if (armed_as == SupervisorPhase::Restoring) {
            supervisor.arm_restore(11);
        } else {
            supervisor.arm_withdrawal(provisioning_flow::Failure::WifiRollbackUnavailable, false);
        }

        ASSERT_TRUE(supervisor.armed());

        EXPECT_EQ(supervisor.abandon(), Result::Abandoned);
        EXPECT_FALSE(supervisor.armed());
        EXPECT_EQ(supervisor.tag(), kNoTag);

        // ⛔ AND THE CARRIED FAILURE GOES WITH IT. An abandoned withdrawal must not later report the
        // rollback failure it was holding: the reset, not the store, is why the station came down.
        EXPECT_EQ(step(supervisor, NetworkPhase::Disabled), Result::Pending);
    }

    // Abandoning an unarmed supervisor is a no-op rather than a spurious result.
    wifi_restore::Supervisor idle;
    EXPECT_EQ(idle.abandon(), Result::Pending);
}

// --- Publication: who may be told what -------------------------------------

TEST(WifiRestorePublicationTests, OnlyTheTwoFailuresAreEverPublishedAndOnlyToALiveSurface) {
    using wifi_restore::Circumstances;
    using wifi_restore::Publication;
    using wifi_restore::report_for;

    // The successes and the silence: nothing recorded, nothing published, whatever the surface.
    for (const Result quiet :
         {Result::Pending, Result::Rejoined, Result::Withdrawn, Result::Abandoned}) {
        for (const bool live : {false, true}) {
            const wifi_restore::Report report =
                report_for(quiet, Circumstances{live, false, false});
            EXPECT_EQ(report.publication, Publication::None);
            EXPECT_EQ(report.failure, provisioning_flow::Failure::None);
        }
    }

    // ⛔ THE TYPED OUTCOME IS RECORDED EITHER WAY; ONLY DELIVERY DEPENDS ON THE SURFACE.
    using wifi_restore::Circumstances;

    const wifi_restore::Report live_restore =
        report_for(Result::RestoreFailed, Circumstances{true, false, false});
    EXPECT_EQ(live_restore.publication, Publication::SetupFailed);
    EXPECT_EQ(live_restore.failure, provisioning_flow::Failure::WifiRestoreFailed);

    const wifi_restore::Report expired_restore =
        report_for(Result::RestoreFailed, Circumstances{false, false, false});
    EXPECT_EQ(expired_restore.publication, Publication::None)
        << "an expired surface must never be pretended into existence";
    EXPECT_EQ(expired_restore.failure, provisioning_flow::Failure::WifiRestoreFailed)
        << "but the device still records what happened";

    const wifi_restore::Report live_rollback =
        report_for(Result::RollbackUnavailable, Circumstances{true, false, false});
    EXPECT_EQ(live_rollback.publication, Publication::SetupFailed);
    EXPECT_EQ(live_rollback.failure, provisioning_flow::Failure::WifiRollbackUnavailable);

    const wifi_restore::Report expired_rollback =
        report_for(Result::RollbackUnavailable, Circumstances{false, false, false});
    EXPECT_EQ(expired_rollback.publication, Publication::None);
    EXPECT_EQ(expired_rollback.failure, provisioning_flow::Failure::WifiRollbackUnavailable);
}

TEST(WifiRestorePublicationTests, AQuarantinedStationYieldsNoRestoreVocabularyAtAll) {
    // ⛔ PM RULING 3, AT THE PUBLICATION BOUNDARY TOO. The typed WifiUnavailable Device Fault is the
    // sole owner of a driver that stopped answering, so nothing here is recorded OR published —
    // not even for a result the supervisor had already reached before the quarantine landed.
    for (const Result result :
         {Result::Pending, Result::Rejoined, Result::Withdrawn, Result::RestoreFailed,
          Result::RollbackUnavailable, Result::Abandoned}) {
        for (const bool live : {false, true}) {
            const wifi_restore::Report report =
                wifi_restore::report_for(result, wifi_restore::Circumstances{live, true, false});
            EXPECT_EQ(report.publication, wifi_restore::Publication::None);
            EXPECT_EQ(report.failure, provisioning_flow::Failure::None)
                << "result " << static_cast<int>(result);
        }
    }
}

TEST(WifiRestorePublicationNegativeControlTests, TheThreeGuardsAreEachLoadBearing) {
    using wifi_restore::Circumstances;
    using wifi_restore::Publication;
    using wifi_restore::report_for;

    // ⛔ CONTROL: published after the surface expired. The live and expired answers for the SAME
    // result must differ, or the surface term does nothing.
    ASSERT_NE(report_for(Result::RestoreFailed, Circumstances{true, false, false}).publication,
              report_for(Result::RestoreFailed, Circumstances{false, false, false}).publication);

    // ⛔ CONTROL: a Device Fault double-reported. The quarantined and unquarantined answers for the
    // same live failure must differ.
    ASSERT_NE(report_for(Result::RestoreFailed, Circumstances{true, false, false}).failure,
              report_for(Result::RestoreFailed, Circumstances{true, true, false}).failure);

    // ⛔ CONTROL: acceptance declared success. Pending must never carry a failure or a publication,
    // whatever the surface — an implementation that reported at arming time would fail here.
    for (const bool live : {false, true}) {
        for (const bool quarantined : {false, true}) {
            ASSERT_EQ(
                report_for(Result::Pending, Circumstances{live, quarantined, false}).publication,
                Publication::None);
            ASSERT_EQ(report_for(Result::Pending, Circumstances{live, quarantined, false}).failure,
                      provisioning_flow::Failure::None);
        }
    }

    // And the two failures are distinguished from each other, not merely from success.
    ASSERT_NE(report_for(Result::RestoreFailed, Circumstances{true, false, false}).failure,
              report_for(Result::RollbackUnavailable, Circumstances{true, false, false}).failure);
}

// --- One failure, one publication, one accounting trail ----------------------
//
// ⛔ THE DEFECT THIS PINS. A staged previous-pair disagreement published WifiRollbackUnavailable at
// the candidate's settlement — which it must, because that publication is what moves the page off
// Validating — and then published the SAME typed failure again when the asynchronous withdrawal
// reached Disabled. Two identical announcements for one occurrence.

namespace {

using wifi_restore::Circumstances;
using wifi_restore::Publication;

// The whole lifecycle of one cleanup, as the two facts a caller can observe: how many times the
// browser was told, and whether the terminal settlement was recorded.
struct Trail {
    int publications{0};
    int recorded_failures{0};
    provisioning_flow::Failure last{provisioning_flow::Failure::None};
    Result terminal{Result::Pending};
};

// Drives a withdrawal-based cleanup from arming to `Disabled`, counting publications the way
// conclude_restore() does. `already_published` models the candidate settlement having spoken first.
Trail withdrawal_trail(const provisioning_flow::Failure carry, const bool already_published,
                       const bool surface_live, const bool quarantined,
                       const int publications_before = 0) {
    Trail trail;
    trail.publications = publications_before;

    wifi_restore::Supervisor supervisor;
    supervisor.arm_withdrawal(carry, already_published);

    // Not down yet: the radio is still on the candidate, and nothing may be reported.
    for (const NetworkPhase phase : {NetworkPhase::Connected, NetworkPhase::Connecting}) {
        const Result pending = supervisor.observe(
            phase, quarantined ? NetworkFault::DriverUnresponsive : NetworkFault::None, false,
            kNoTag, Outcome::Leased);
        if (pending != Result::Pending) {
            trail.terminal = pending;
            break;
        }
    }

    if (trail.terminal == Result::Pending) {
        trail.terminal =
            supervisor.observe(NetworkPhase::Disabled,
                               quarantined ? NetworkFault::DriverUnresponsive : NetworkFault::None,
                               false, kNoTag, Outcome::Leased);
    }

    const wifi_restore::Report report = wifi_restore::report_for(
        trail.terminal, Circumstances{surface_live, quarantined, supervisor.already_reported()});

    if (report.publication == Publication::SetupFailed) {
        trail.publications += 1;
    }

    if (report.failure != provisioning_flow::Failure::None) {
        trail.recorded_failures += 1;
        trail.last = report.failure;
    }

    return trail;
}

} // namespace

TEST(WifiRestorePublicationLifecycleTests, AStagingDisagreementTellsTheBrowserExactlyOnce) {
    // The candidate's own settlement already published wifi-rollback-unavailable — count it — and the
    // withdrawal it scheduled is still asynchronous.
    const Trail trail = withdrawal_trail(provisioning_flow::Failure::WifiRollbackUnavailable,
                                         /*already_published=*/true, /*surface_live=*/true,
                                         /*quarantined=*/false, /*publications_before=*/1);

    EXPECT_EQ(trail.terminal, Result::RollbackUnavailable);
    EXPECT_EQ(trail.publications, 1) << "one occurrence, one user-facing publication";

    // ⛔ AND THE TERMINAL SETTLEMENT IS STILL RECORDED. The accounting trail is what says the radio
    // finally came down; suppressing the second ANNOUNCEMENT must not suppress the record.
    EXPECT_EQ(trail.recorded_failures, 1);
    EXPECT_EQ(trail.last, provisioning_flow::Failure::WifiRollbackUnavailable);
}

TEST(WifiRestorePublicationLifecycleTests, APreflightRefusalIsOnePublicationAndNoCleanupAtAll) {
    // ⛔ THE ONE PATH WHERE AN IMMEDIATE PUBLICATION IS THE WHOLE STORY. The rollback target could not
    // be established BEFORE the radio was disturbed, so the candidate is refused outright, esp_wifi
    // is never touched and there is nothing to take down. No cleanup is armed, so no terminal exists
    // and nothing can announce the same failure a second time.
    test::FakeKeyValue store;
    (void)seed_working_device(store, "HomeNetwork", "homepassword");
    store.fail_read(storage::Namespace::System, kConfigPointer);

    const Target unestablished = wifi_restore::target_for(store);
    ASSERT_EQ(unestablished.action, Action::Unavailable);

    // Nothing that could be handed to the station exists, which is what makes refusing before the
    // submission the honest order rather than merely the convenient one.
    const Credential nothing = wifi_restore::credential_for(store, unestablished);
    EXPECT_FALSE(nothing.loaded);
    EXPECT_TRUE(nothing.ssid.view().empty());

    // An unestablished target agrees with no staged capture, so it can never be confirmed later.
    EXPECT_FALSE(
        wifi_restore::agrees(unestablished, config::kNoGeneration, secrets::kNoGeneration));

    // ⛔ AND AN UNARMED SUPERVISOR PRODUCES NO TERMINAL, WHICH IS WHY THERE IS NO SECOND PUBLICATION.
    // The single user-facing statement comes from the candidate's own settlement; this asserts that
    // nothing else in the lifecycle can add to it.
    wifi_restore::Supervisor supervisor;
    EXPECT_FALSE(supervisor.armed());

    for (const NetworkPhase phase :
         {NetworkPhase::Connected, NetworkPhase::Disabled, NetworkPhase::Unavailable}) {
        const Result result =
            supervisor.observe(phase, NetworkFault::None, false, kNoTag, Outcome::Leased);

        EXPECT_EQ(result, Result::Pending);

        const wifi_restore::Report report =
            wifi_restore::report_for(result, Circumstances{true, false, false});

        EXPECT_EQ(report.publication, Publication::None);
        EXPECT_EQ(report.failure, provisioning_flow::Failure::None);
    }
}

TEST(WifiRestorePublicationLifecycleTests, PostDisturbanceRollbackNeverClaimsCompletionEarly) {
    // ⛔ "NEVER STATE OR IMPLY THAT RADIO ROLLBACK COMPLETED BEFORE Disabled." The credential-load and
    // submission-refusal routes arm a withdrawal whose failure has NOT been published yet, and no
    // terminal result — therefore no publication and no recorded failure — exists until the station
    // says Disabled.
    wifi_restore::Supervisor supervisor;
    supervisor.arm_withdrawal(provisioning_flow::Failure::WifiRollbackUnavailable, false);

    for (const NetworkPhase phase :
         {NetworkPhase::Connected, NetworkPhase::Connecting, NetworkPhase::Reconnecting,
          NetworkPhase::Unavailable, NetworkPhase::Starting}) {
        const Result pending =
            supervisor.observe(phase, NetworkFault::None, false, kNoTag, Outcome::Leased);

        ASSERT_EQ(pending, Result::Pending) << "phase " << static_cast<int>(phase);

        const wifi_restore::Report premature =
            wifi_restore::report_for(pending, Circumstances{true, false, false});

        EXPECT_EQ(premature.publication, Publication::None);
        EXPECT_EQ(premature.failure, provisioning_flow::Failure::None)
            << "nothing may be recorded before the station has actually come down";
    }

    // And then, exactly once, at the terminal.
    const Trail trail =
        withdrawal_trail(provisioning_flow::Failure::WifiRollbackUnavailable, false, true, false);
    EXPECT_EQ(trail.terminal, Result::RollbackUnavailable);
    EXPECT_EQ(trail.publications, 1);
    EXPECT_EQ(trail.recorded_failures, 1);
}

TEST(WifiRestorePublicationLifecycleTests, ADeviceFaultDuringCleanupAddsNoSecondFailure) {
    // ⛔ PM RULING 3 THROUGH THE WHOLE LIFECYCLE. A driver that stops answering during the cleanup
    // ends the supervision silently: no restore failure, no rollback failure, nothing recorded.
    for (const bool already_published : {false, true}) {
        const Trail trail = withdrawal_trail(
            provisioning_flow::Failure::WifiRollbackUnavailable, already_published,
            /*surface_live=*/true, /*quarantined=*/true, already_published ? 1 : 0);

        EXPECT_EQ(trail.terminal, Result::Abandoned);
        EXPECT_EQ(trail.recorded_failures, 0) << "the Device Fault is the sole owner";
        EXPECT_EQ(trail.publications, already_published ? 1 : 0)
            << "no SECOND statement about the same radio, and no retraction of the first";
    }

    // ⛔ AND THE SETUP STATUS IS NOT STRANDED. The page was moved off Validating by the candidate's
    // own settlement before any cleanup was scheduled — which is why suppressing here withholds a
    // second statement rather than the first. Asserted as the property that makes it safe: a
    // suppressed terminal never publishes SetupState at all, so it cannot overwrite or undo it.
    const wifi_restore::Report suppressed =
        wifi_restore::report_for(Result::Abandoned, Circumstances{true, true, false});
    EXPECT_EQ(suppressed.publication, Publication::None);
    EXPECT_EQ(suppressed.failure, provisioning_flow::Failure::None);
}

TEST(WifiRestorePublicationLifecycleTests,
     AnAcceptedRestoreEndingUnreachableIsOneFailureAndStaysUnavailable) {
    // ⛔ THE PRECISE CONTRACT WifiRestoreFailed NOW HAS. Accepted, then matching Unreachable — and the
    // station is truthfully NetworkPhase::Unavailable throughout, never withdrawn to Disabled. The
    // corrected comment in provisioning_flow.hpp says exactly this; the test is what keeps it true.
    wifi_restore::Supervisor supervisor;
    supervisor.arm_restore(21);

    // Unavailable is the level a station reaches when §16.2 gives up, and it is NOT a terminal on its
    // own: only the matching evidence decides.
    EXPECT_EQ(supervisor.observe(NetworkPhase::Unavailable, NetworkFault::LinkLost, false, kNoTag,
                                 Outcome::Leased),
              Result::Pending);

    const Result terminal = supervisor.observe(NetworkPhase::Unavailable, NetworkFault::LinkLost,
                                               true, 21, Outcome::Unreachable);
    EXPECT_EQ(terminal, Result::RestoreFailed);

    int publications = 0;
    const wifi_restore::Report report = wifi_restore::report_for(
        terminal, Circumstances{true, false, supervisor.already_reported()});

    EXPECT_FALSE(supervisor.already_reported())
        << "a restore's own failure has never been published before its terminal";
    EXPECT_EQ(report.publication, Publication::SetupFailed);
    EXPECT_EQ(report.failure, provisioning_flow::Failure::WifiRestoreFailed);
    publications += 1;

    // Settled once: a repeat step cannot produce a second failure.
    EXPECT_EQ(supervisor.observe(NetworkPhase::Unavailable, NetworkFault::LinkLost, true, 21,
                                 Outcome::Unreachable),
              Result::Pending);
    EXPECT_EQ(publications, 1);

    // ⛔ AND NO WITHDRAWAL IS IMPLIED. The supervisor is idle, not withdrawing, so nothing in this
    // firmware will drive the station to Disabled on account of this outcome.
    EXPECT_FALSE(supervisor.armed());
    EXPECT_NE(supervisor.phase(), SupervisorPhase::Withdrawing);
}

TEST(WifiRestorePublicationNegativeControlTests, TheDuplicatePublicationWouldReappear) {
    // ⛔ THE CONTROL FOR THIS CORRECTION, AS AN ASSERTION RATHER THAN A COMMENT. If `already_reported`
    // stopped suppressing delivery, the staging-disagreement lifecycle would publish twice. The two
    // answers for the same result and the same live surface MUST differ on that flag alone.
    ASSERT_NE(
        wifi_restore::report_for(Result::RollbackUnavailable, Circumstances{true, false, true})
            .publication,
        wifi_restore::report_for(Result::RollbackUnavailable, Circumstances{true, false, false})
            .publication);

    // ⬅️ AND THE ACCOUNTING TRAIL MUST *NOT* DIFFER, which is the other half of the correction: the
    // terminal settlement is recorded either way.
    ASSERT_EQ(
        wifi_restore::report_for(Result::RollbackUnavailable, Circumstances{true, false, true})
            .failure,
        wifi_restore::report_for(Result::RollbackUnavailable, Circumstances{true, false, false})
            .failure);

    // The same two properties for the restore failure, so the rule is about the flag and not about
    // one enumerator.
    ASSERT_NE(wifi_restore::report_for(Result::RestoreFailed, Circumstances{true, false, true})
                  .publication,
              wifi_restore::report_for(Result::RestoreFailed, Circumstances{true, false, false})
                  .publication);
    ASSERT_EQ(
        wifi_restore::report_for(Result::RestoreFailed, Circumstances{true, false, true}).failure,
        wifi_restore::report_for(Result::RestoreFailed, Circumstances{true, false, false}).failure);
}

// --- The retained target versus a later read --------------------------------

TEST(WifiRestoreRetentionTests, TheRetainedTargetOutlivesAPointerReadThatWouldNowFail) {
    // ⛔ FINDING 4, AS THE PROPERTY THAT MATTERS. The target is captured before the radio is
    // disturbed. If the ACTIVATION POINTERS later become unreadable, a fresh read can no longer name
    // the pair — but the retained generations still load their records, because load_staged() matches
    // on the generation and reads both slots rather than following a pointer.
    test::FakeKeyValue store;
    const provisioning_flow::StagedPair working =
        seed_working_device(store, "HomeNetwork", "homepassword");

    // Captured while the device is provably running that pair.
    const Target retained = wifi_restore::target_for(store);
    ASSERT_EQ(retained.action, Action::Restore);
    ASSERT_EQ(retained.configuration_generation, working.configuration_generation);

    // The store now refuses the pointer reads a later capture would depend on.
    store.fail_read(storage::Namespace::System, kConfigPointer);
    store.fail_read(storage::Namespace::System, kSecretPointer);

    EXPECT_EQ(wifi_restore::target_for(store).action, Action::Unavailable)
        << "a FRESH read can no longer name the pair — which is what the first version relied on";

    // ⛔ RE-ARMED, AND THIS IS LOAD-BEARING. FakeKeyValue's injected failures are ONE-SHOT: the
    // assertion above consumed both, so without arming them again the call below would run against a
    // healthy store and would pass even for an implementation that ignored `retained` and re-read the
    // pointers itself. A negative control on this file caught exactly that.
    store.fail_read(storage::Namespace::System, kConfigPointer);
    store.fail_read(storage::Namespace::System, kSecretPointer);

    // ⬅️ AND THE RETAINED TARGET STILL WORKS, WHICH IS THE WHOLE POINT. load_staged() matches on the
    // generation and reads both slots, so it never follows the pointer that has become unreadable.
    Credential credential = wifi_restore::credential_for(store, retained);
    EXPECT_TRUE(credential.loaded);
    EXPECT_EQ(credential.ssid.view(), "HomeNetwork");
    wifi_restore::wipe(credential);
}

TEST(WifiRestoreRetentionNegativeControlTests, ALateActivePointerReadIsNotEquivalent) {
    // ⛔ THE CONTROL FOR "late active pointers replace the retained target". The two must be
    // distinguishable, or retaining anything is pointless. Asserted side by side on one store.
    test::FakeKeyValue store;
    (void)seed_working_device(store, "HomeNetwork", "homepassword");

    const Target retained = wifi_restore::target_for(store);
    ASSERT_EQ(retained.action, Action::Restore);

    // ⬅️ THE POINTER LIVES IN `system`, NOT IN ITS OWN NAMESPACE — see candidate.hpp: that is the
    // activation boundary's actual shape, and reading the wrong namespace here would have made this
    // control silently vacuous.
    store.fail_read(storage::Namespace::System, kConfigPointer);

    const Target late = wifi_restore::target_for(store);

    ASSERT_NE(retained.action, late.action);
    ASSERT_NE(retained.configuration_generation, late.configuration_generation);
}

TEST(WifiRestoreRetentionTests, AnUnestablishedTargetYieldsNoCredentialToSubmit) {
    // Requirement 2's other half, at the level this unit owns it: even if a caller tried, an
    // Unavailable target produces nothing that could be handed to the station. app_core refuses the
    // candidate before calling submit() at all, which is read-verified; this is the property that
    // makes such a call harmless if it were ever added.
    test::FakeKeyValue store;
    (void)seed_working_device(store, "HomeNetwork", "homepassword");
    store.fail_read(storage::Namespace::System, kConfigPointer);

    const Target unavailable = wifi_restore::target_for(store);
    ASSERT_EQ(unavailable.action, Action::Unavailable);

    const Credential credential = wifi_restore::credential_for(store, unavailable);
    EXPECT_FALSE(credential.loaded);
    EXPECT_TRUE(credential.ssid.view().empty());
    EXPECT_TRUE(credential.password.view().empty());
}

// --- The two accounts of the previous pair must agree ------------------------

TEST(WifiRestoreAgreementTests, TheStagedCaptureAndTheRetainedTargetAreCheckedAgainstEachOther) {
    test::FakeKeyValue store;
    const provisioning_flow::StagedPair working =
        seed_working_device(store, "HomeNetwork", "homepassword");

    const Target retained = wifi_restore::target_for(store);
    ASSERT_EQ(retained.action, Action::Restore);

    // The ordinary case: stage a candidate and the two accounts agree, because both read the same
    // two pointers.
    const provisioning_flow::StagedPair candidate = stage_candidate(store);
    EXPECT_TRUE(wifi_restore::agrees(retained, candidate.previous_configuration_generation,
                                     candidate.previous_secret_generation));
    EXPECT_EQ(candidate.previous_configuration_generation, working.configuration_generation);

    // ⛔ AND A DISAGREEMENT IS CAUGHT. One generation off in either half is enough: it means the
    // store moved underneath a live transaction, and neither account can then be trusted.
    EXPECT_FALSE(wifi_restore::agrees(retained, candidate.previous_configuration_generation + 1,
                                      candidate.previous_secret_generation));
    EXPECT_FALSE(wifi_restore::agrees(retained, candidate.previous_configuration_generation,
                                      candidate.previous_secret_generation + 1));
    EXPECT_FALSE(wifi_restore::agrees(retained, config::kNoGeneration, secrets::kNoGeneration));
}

TEST(WifiRestoreAgreementTests, AFirstProvisioningAgreesOnHavingNoPreviousPair) {
    test::FakeKeyValue virgin;

    const Target target = wifi_restore::target_for(virgin);
    ASSERT_EQ(target.action, Action::Withdraw);

    const provisioning_flow::StagedPair first = stage_candidate(virgin);
    EXPECT_EQ(first.previous_configuration_generation, config::kNoGeneration);
    EXPECT_EQ(first.previous_secret_generation, secrets::kNoGeneration);

    EXPECT_TRUE(wifi_restore::agrees(target, first.previous_configuration_generation,
                                     first.previous_secret_generation));

    // A Withdraw target must not agree with a capture that names a real pair — that would mean the
    // device acquired a previous pair mid-transaction.
    EXPECT_FALSE(wifi_restore::agrees(target, 1, 1));
}

TEST(WifiRestoreAgreementTests, AnUnestablishedTargetAgreesWithNothing) {
    Target unavailable;
    unavailable.action = Action::Unavailable;

    EXPECT_FALSE(wifi_restore::agrees(unavailable, config::kNoGeneration, secrets::kNoGeneration));
    EXPECT_FALSE(wifi_restore::agrees(unavailable, 1, 1));
    EXPECT_FALSE(wifi_restore::agrees(unavailable, 7, 9));
}

// --- Labels and negative controls -------------------------------------------

TEST(WifiRestoreTests, EveryActionHasADistinctNonSecretLabel) {
    const std::string restore{wifi_restore::log_label(Action::Restore)};
    const std::string withdraw{wifi_restore::log_label(Action::Withdraw)};
    const std::string unavailable{wifi_restore::log_label(Action::Unavailable)};

    EXPECT_NE(restore, withdraw);
    EXPECT_NE(restore, unavailable);
    EXPECT_NE(withdraw, unavailable);

    for (const std::string& label : {restore, withdraw, unavailable}) {
        EXPECT_FALSE(label.empty());
        EXPECT_EQ(label.find("HomeNetwork"), std::string::npos);
        EXPECT_EQ(label.find("password"), std::string::npos);
    }
}

TEST(WifiRestoreNegativeControlTests, TheThreeAnswersAreGenuinelyReachableAndDistinct) {
    // ⛔ AN IMPLEMENTATION THAT ALWAYS ANSWERED THE SAME WAY WOULD PASS MOST OF THE TESTS ABOVE. The
    // three store states that must produce three different answers, asserted side by side so a
    // collapse fails here rather than being noticed on hardware.
    test::FakeKeyValue whole;
    (void)seed_working_device(whole, "HomeNetwork", "homepassword");

    test::FakeKeyValue empty;

    test::FakeKeyValue broken;
    (void)seed_working_device(broken, "HomeNetwork", "homepassword");
    broken.fail_read(storage::Namespace::System, kConfigPointer);

    EXPECT_EQ(wifi_restore::target_for(whole).action, Action::Restore);
    EXPECT_EQ(wifi_restore::target_for(empty).action, Action::Withdraw);
    EXPECT_EQ(wifi_restore::target_for(broken).action, Action::Unavailable);
}

TEST(WifiRestoreNegativeControlTests, TheTypedRestoreFailureLabelIsItsOwnAndIsNonSecret) {
    // The label the device records when a restoration was required and did not happen. It must not
    // collide with any other setup outcome — a reader that could not tell it apart from a credential
    // failure would send a user to retype a password that was never the problem.
    const std::string restore{
        provisioning_flow::log_label(provisioning_flow::Failure::WifiRestoreFailed)};

    EXPECT_EQ(restore, "wifi-restore-failed");

    for (const provisioning_flow::Failure other :
         {provisioning_flow::Failure::None, provisioning_flow::Failure::WifiUnreachable,
          provisioning_flow::Failure::WifiTimedOut, provisioning_flow::Failure::WifiDriverStalled,
          provisioning_flow::Failure::WifiEvidenceLost, provisioning_flow::Failure::StorageRejected,
          provisioning_flow::Failure::StorageCandidateFailed,
          provisioning_flow::Failure::StorageCommitFailed,
          provisioning_flow::Failure::PrinterUnreachable,
          provisioning_flow::Failure::PrinterUnauthorized,
          provisioning_flow::Failure::PrinterUnsupported,
          provisioning_flow::Failure::PrinterUnusableResponse,
          provisioning_flow::Failure::PromotionFailed,
          provisioning_flow::Failure::WifiRollbackUnavailable}) {
        EXPECT_NE(std::string{provisioning_flow::log_label(other)}, restore);
    }
}

TEST(WifiRestoreNegativeControlTests, TheRollbackFailureIsItsOwnLabelAndMeansSomethingElse) {
    // ⛔ THE TWO MUST NOT COLLAPSE. "A restore was tried and the network did not come back" and "the
    // device could not work out what to try" have different causes and different user actions, and
    // only the first is a statement about the user's network.
    const std::string rollback{
        provisioning_flow::log_label(provisioning_flow::Failure::WifiRollbackUnavailable)};
    const std::string restore{
        provisioning_flow::log_label(provisioning_flow::Failure::WifiRestoreFailed)};

    EXPECT_EQ(rollback, "wifi-rollback-unavailable");
    EXPECT_NE(rollback, restore);

    for (const provisioning_flow::Failure other :
         {provisioning_flow::Failure::None, provisioning_flow::Failure::WifiUnreachable,
          provisioning_flow::Failure::WifiTimedOut, provisioning_flow::Failure::WifiDriverStalled,
          provisioning_flow::Failure::WifiEvidenceLost, provisioning_flow::Failure::StorageRejected,
          provisioning_flow::Failure::StorageCandidateFailed,
          provisioning_flow::Failure::StorageCommitFailed,
          provisioning_flow::Failure::PrinterUnreachable,
          provisioning_flow::Failure::PrinterUnauthorized,
          provisioning_flow::Failure::PrinterUnsupported,
          provisioning_flow::Failure::PrinterUnusableResponse,
          provisioning_flow::Failure::PromotionFailed,
          provisioning_flow::Failure::WifiRestoreFailed}) {
        EXPECT_NE(std::string{provisioning_flow::log_label(other)}, rollback);
    }
}

TEST(WifiRestoreNegativeControlTests, TheExistingFailureLabelsKeepTheirNumericValues) {
    // ⛔ THE CHECKPOINT-3c EVIDENCE ARCHIVE CONTAINS `validation-failed failure=<n>` RECORDS, and the
    // numbers in them are this enumerator's values. WifiRestoreFailed was appended rather than
    // grouped with the other Wifi* members for exactly that reason, and this is what stops a later
    // tidy-up from silently changing what every archived line means.
    EXPECT_EQ(static_cast<int>(provisioning_flow::Failure::None), 0);
    EXPECT_EQ(static_cast<int>(provisioning_flow::Failure::WifiUnreachable), 1);
    EXPECT_EQ(static_cast<int>(provisioning_flow::Failure::WifiTimedOut), 2);
    EXPECT_EQ(static_cast<int>(provisioning_flow::Failure::WifiDriverStalled), 3);
    EXPECT_EQ(static_cast<int>(provisioning_flow::Failure::WifiEvidenceLost), 4);
    EXPECT_EQ(static_cast<int>(provisioning_flow::Failure::StorageRejected), 5);
    EXPECT_EQ(static_cast<int>(provisioning_flow::Failure::PromotionFailed), 12);
    EXPECT_EQ(static_cast<int>(provisioning_flow::Failure::WifiRestoreFailed), 13);

    // Appended in the same pass and for the same reason, so it is pinned from the day it exists.
    EXPECT_EQ(static_cast<int>(provisioning_flow::Failure::WifiRollbackUnavailable), 14);
}

} // namespace
} // namespace prd::app_core
