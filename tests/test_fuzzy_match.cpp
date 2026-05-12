// test_fuzzy_match.cpp — Task v0.3-4 unit tests for the vendored
// fts_fuzzy_match algorithm.  See src/utils/fuzzy_match.h for the
// algorithm header and license dedication.
//
// These tests cover the canonical fts test vectors that are documented
// in the upstream README + the bonus cases we rely on from the command
// palette (acronym match for "nw" -> "Net Worth", case-insensitive).
// Score values are NOT asserted exactly because the algorithm's score
// constants are documented in the header and changing them would be a
// deliberate breaking change — instead we assert relative ordering
// ("an exact-substring match scores higher than a scattered subsequence
// match") which is the property the command palette actually relies on.

#include "../src/utils/fuzzy_match.h"

#include <gtest/gtest.h>

#include <string>

using tf::utils::fuzzy_match;
using tf::utils::fuzzy_match_simple;

// ---------------------------------------------------------------------------
// 1. Exact substring at a word boundary scores higher than the same pattern
//    appearing as a scattered/interior subsequence.
//
//    Pattern "tax" against "Set tax bracket" gets a separator bonus
//    (space precedes 't') + consecutive bonuses.  The same pattern
//    against "exotaxidermy" (interior subsequence, no word boundary on
//    'tax' as a unit) scores lower.  We pin the *ordering*, not the
//    absolute numbers, because the upstream score constants are a
//    vendored implementation detail.
// ---------------------------------------------------------------------------
TEST(FuzzyMatch, Match_ExactSubstring_HasHighestScore) {
    int score_word_boundary = 0;
    int score_interior      = 0;

    ASSERT_TRUE(fuzzy_match("tax", "Set tax bracket",  score_word_boundary));
    ASSERT_TRUE(fuzzy_match("tax", "metaxylem-prefix", score_interior));

    EXPECT_GT(score_word_boundary, score_interior)
        << "Word-boundary match must outscore interior match "
        << "(boundary=" << score_word_boundary
        << " interior=" << score_interior << ")";
}

// ---------------------------------------------------------------------------
// 2. Acronym/initials match: "nw" matches "Net Worth".  This is the
//    smoking-gun fts behavior; the separator bonus + first-letter bonus
//    stack so initials are strongly preferred.
// ---------------------------------------------------------------------------
TEST(FuzzyMatch, Match_Acronym_Matches) {
    int score = 0;
    ASSERT_TRUE(fuzzy_match("nw", "Net Worth", score));

    // And it should beat the same pattern against an arbitrary string
    // that contains 'n' and 'w' in order but not at word boundaries.
    int interior = 0;
    ASSERT_TRUE(fuzzy_match("nw", "Snow plow", interior));
    EXPECT_GT(score, interior);
}

// ---------------------------------------------------------------------------
// 3. Case-insensitive: capitalization in pattern OR str must not affect
//    whether the match succeeds.  Scores between case variants of the
//    pattern may differ slightly (camelCase bonus can fire on the str
//    side) but the boolean result must be identical.
// ---------------------------------------------------------------------------
TEST(FuzzyMatch, Match_CaseInsensitive) {
    int s1 = 0, s2 = 0, s3 = 0;
    EXPECT_TRUE(fuzzy_match("DASH",     "Switch view: Dashboard", s1));
    EXPECT_TRUE(fuzzy_match("dash",     "Switch view: Dashboard", s2));
    EXPECT_TRUE(fuzzy_match("Dash",     "switch view: dashboard", s3));

    // fuzzy_match_simple is the boolean-only fast path; it must also be
    // case-insensitive.
    EXPECT_TRUE(fuzzy_match_simple("DASHBOARD", "Switch view: Dashboard"));
    EXPECT_TRUE(fuzzy_match_simple("dashboard", "Switch view: Dashboard"));
}

// ---------------------------------------------------------------------------
// 4. Empty pattern matches everything (degenerate case).  Upstream behavior
//    documented in the header.  Empty str + non-empty pattern fails.
// ---------------------------------------------------------------------------
TEST(FuzzyMatch, Empty_Pattern_TriviallyMatches) {
    int score = 0;
    EXPECT_TRUE(fuzzy_match("", "Anything",  score));
    EXPECT_TRUE(fuzzy_match_simple("", "Anything"));

    int empty_str_score = 0;
    EXPECT_FALSE(fuzzy_match("x", "", empty_str_score));
    EXPECT_FALSE(fuzzy_match_simple("x", ""));
}

// ---------------------------------------------------------------------------
// 5. Out-of-order characters do not match.  Pattern "wn" vs "Net Worth"
//    must fail -- the 'w' in "Worth" comes AFTER the 'n' in "Net", and
//    fuzzy match is order-preserving.
// ---------------------------------------------------------------------------
TEST(FuzzyMatch, NoMatch_OutOfOrderChars) {
    int score = 0;
    EXPECT_FALSE(fuzzy_match("wn", "Net Worth", score));
    EXPECT_FALSE(fuzzy_match_simple("wn", "Net Worth"));

    // Sanity: the in-order variant DOES match.
    int ok = 0;
    EXPECT_TRUE(fuzzy_match("nw", "Net Worth", ok));
}

// ---------------------------------------------------------------------------
// 6. Consecutive characters beat scattered ones when neither path collects
//    word-boundary bonuses.
//
//    Pattern "abc" against "abcdef" finds three consecutive matches at
//    offsets 0,1,2 (first-letter + 2 consecutive bonuses).  The same
//    pattern against "axbxc" finds the chars scattered (no consecutive
//    bonuses; just first-letter).  Consecutive must win.
// ---------------------------------------------------------------------------
TEST(FuzzyMatch, Match_ConsecutiveBeatsScattered) {
    int consecutive = 0;
    int scattered   = 0;
    ASSERT_TRUE(fuzzy_match("abc", "abcdef",  consecutive));
    ASSERT_TRUE(fuzzy_match("abc", "axbxcxx", scattered));
    EXPECT_GT(consecutive, scattered)
        << "consecutive=" << consecutive << " scattered=" << scattered;
}
