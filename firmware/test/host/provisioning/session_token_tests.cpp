// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "escaping.hpp"
#include "session_token.hpp"

// The CSRF session token (owner ruling D9) and the JSON escaper.

namespace prd::provisioning {
namespace {

struct Bytes {
    std::vector<std::uint8_t> values;
    std::size_t index{0};
};

std::uint8_t next(void* const context) {
    auto* const bytes = static_cast<Bytes*>(context);
    const std::uint8_t value = bytes->values[bytes->index];
    bytes->index = (bytes->index + 1) % bytes->values.size();
    return value;
}

Bytes counting() {
    Bytes bytes;
    for (std::uint32_t value = 0; value < 256; ++value) {
        bytes.values.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

} // namespace

TEST(SessionTokenTests, AFreshTokenIsAbsentUntilGenerated) {
    SessionToken token;

    EXPECT_FALSE(token.present());
    EXPECT_TRUE(token.view().empty());

    // And it matches nothing — not even the empty string, which a naive comparison would
    // accept against an empty buffer.
    EXPECT_FALSE(token.matches(""));
    EXPECT_FALSE(token.matches(std::string(kTokenLength, '0')));
}

TEST(SessionTokenTests, TheTokenIsTheDeclaredLengthAndHexadecimal) {
    Bytes bytes = counting();
    SessionToken token;
    token.regenerate(&next, &bytes);

    EXPECT_TRUE(token.present());
    EXPECT_EQ(token.view().size(), kTokenLength);

    for (const char character : token.view()) {
        const bool hex =
            (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        EXPECT_TRUE(hex) << "'" << character << "' is not a hex digit";
    }
}

TEST(SessionTokenTests, ItCarriesTheDeclaredEntropy) {
    // 16 bytes, two hex characters each. Stated as an assertion so a future edit that
    // shortens it has to say so.
    EXPECT_EQ(kTokenBytes, 16U);
    EXPECT_EQ(kTokenLength, kTokenBytes * 2);
}

TEST(SessionTokenTests, RegeneratingProducesADifferentToken) {
    // D9: "regenerated when the provisioning server/session is recreated". There is no way
    // to obtain a token except by minting a new one.
    Bytes first;
    first.values = {0x11};
    Bytes second;
    second.values = {0x22};

    SessionToken a;
    a.regenerate(&next, &first);
    const std::string before{a.view()};

    a.regenerate(&next, &second);

    EXPECT_NE(std::string{a.view()}, before);
    EXPECT_FALSE(a.matches(before)) << "the previous session's token still works";
}

TEST(SessionTokenTests, ItMatchesItselfAndNothingElse) {
    Bytes bytes = counting();
    SessionToken token;
    token.regenerate(&next, &bytes);

    EXPECT_TRUE(token.matches(token.view()));
    EXPECT_FALSE(token.matches(""));
    EXPECT_FALSE(token.matches("0"));
    EXPECT_FALSE(token.matches(std::string(kTokenLength, 'f')));
}

TEST(SessionTokenTests, ASingleWrongCharacterAtEitherEndIsRejected) {
    // A comparison that stopped at the first difference would still pass these; what they
    // pin is correctness, and the timing property is structural in the loop itself.
    Bytes bytes = counting();
    SessionToken token;
    token.regenerate(&next, &bytes);

    std::string candidate{token.view()};
    candidate[0] = candidate[0] == '0' ? '1' : '0';
    EXPECT_FALSE(token.matches(candidate));

    candidate = std::string{token.view()};
    candidate[candidate.size() - 1] = candidate[candidate.size() - 1] == '0' ? '1' : '0';
    EXPECT_FALSE(token.matches(candidate));
}

TEST(SessionTokenTests, ClearingOverwritesAndInvalidates) {
    Bytes bytes = counting();
    SessionToken token;
    token.regenerate(&next, &bytes);

    const std::string before{token.view()};
    token.clear();

    EXPECT_FALSE(token.present());
    EXPECT_TRUE(token.view().empty());
    EXPECT_FALSE(token.matches(before)) << "the token survived the session it belonged to";
}

TEST(SessionTokenTests, DifferentEntropyGivesDifferentTokens) {
    std::set<std::string> distinct;

    for (std::uint8_t seed = 0; seed < 32; ++seed) {
        Bytes bytes;
        bytes.values = {seed, static_cast<std::uint8_t>(seed + 1)};

        SessionToken token;
        token.regenerate(&next, &bytes);
        distinct.insert(std::string{token.view()});
    }

    EXPECT_EQ(distinct.size(), 32U);
}

// --- JSON escaping ----------------------------------------------------------

TEST(EscapingTests, OrdinaryTextIsUnchanged) {
    EXPECT_EQ(json_escape("Workshop"), "Workshop");
    EXPECT_EQ(json_escape("0.1.0-dev"), "0.1.0-dev");
    EXPECT_EQ(json_escape(""), "");
}

TEST(EscapingTests, TheTwoMandatoryEscapes) {
    EXPECT_EQ(json_escape("say \"hi\""), "say \\\"hi\\\"");
    EXPECT_EQ(json_escape("a\\b"), "a\\\\b");
}

TEST(EscapingTests, ControlCharactersCannotReachTheResponseRaw) {
    // A raw control byte inside a JSON string is a parse error, not merely untidy — the
    // page would fail to render rather than render something wrong.
    EXPECT_EQ(json_escape("a\nb"), "a\\nb");
    EXPECT_EQ(json_escape("a\rb"), "a\\rb");
    EXPECT_EQ(json_escape("a\tb"), "a\\tb");
    EXPECT_EQ(json_escape(std::string("a\x01"
                                      "b")),
              "a\\u0001b");
    EXPECT_EQ(json_escape(std::string("a\0b", 3)), "a\\u0000b");
}

TEST(EscapingTests, TheClassicBreakoutAttemptIsNeutralised) {
    // What an injected value would have to do to escape its string and add a field.
    const std::string escaped = json_escape("x\",\"admin\":true,\"y\":\"");

    EXPECT_EQ(escaped.find("\",\""), std::string::npos) << "the value closed its own string";
    EXPECT_NE(escaped.find("\\\""), std::string::npos);
}

TEST(EscapingTests, HighBytesArePassedThroughSoUtf8Survives) {
    EXPECT_EQ(json_escape("Werkstatt-\xC3\xA4"), "Werkstatt-\xC3\xA4");
}

TEST(EscapingTests, TheFieldHelpersProduceWellFormedPairs) {
    std::string out;
    append_json_string(out, FieldName{"product"}, "Prusa \"Remote\" Display");
    EXPECT_EQ(out, "\"product\":\"Prusa \\\"Remote\\\" Display\"");

    out.clear();
    append_json_bool(out, FieldName{"setupActive"}, true);
    EXPECT_EQ(out, "\"setupActive\":true");

    out.clear();
    append_json_bool(out, FieldName{"setupActive"}, false);
    EXPECT_EQ(out, "\"setupActive\":false");
}

} // namespace prd::provisioning
