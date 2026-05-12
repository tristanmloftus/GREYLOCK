// ============================================================================
// fuzzy_match.h — fts_fuzzy_match (Sublime Text-style fuzzy matching).
// ============================================================================
//
// Vendored single-header port of Forrest Smith's `fts_fuzzy_match` algorithm.
//   Source:  https://github.com/forrestthewoods/lib_fts
//   File:    code/fts_fuzzy_match.h
//   License: The lib_fts project is dedicated to the public domain via the
//            Unlicense (https://unlicense.org); see the upstream LICENSE
//            for the canonical statement.  This port preserves that
//            dedication.  No additional license terms are imposed.
//
// This file is a faithful reimplementation of the canonical algorithm,
// adapted to:
//   1. C++17 std::string_view interfaces (the original takes const char*).
//   2. The tf::utils namespace, so callers in TerminalFinance read
//      `tf::utils::fuzzy_match(...)` rather than a globally-scoped name.
//   3. A pure-header packaging (the upstream is also header-only).
//
// SCORING SEMANTICS (preserved from upstream)
// --------------------------------------------------------------------------
// `fuzzy_match_simple(pattern, str)` returns true iff every character of
// pattern appears in str in order (case-insensitive).
//
// `fuzzy_match(pattern, str, out_score)` performs a recursive best-match
// search that rewards consecutive matches, word/camelCase boundaries, and
// first-letter matches; penalises unmatched leading characters and
// non-matched body characters.  Higher score is a BETTER match.  The
// recursion depth is bounded (`kRecursionLimit`) so degenerate inputs
// cannot stack-overflow.
//
// SCORE TIE-BREAK NOTES
// --------------------------------------------------------------------------
// Two strings can produce identical scores (e.g. exact substring at start
// and exact substring after a separator).  The upstream algorithm is
// stable in the sense that the first match path explored wins; we
// preserve that behaviour and document it so callers know to break ties
// externally (the CommandRegistry sorts identical scores by declaration
// order in `all_commands()`).
//
// CONSTANTS
// --------------------------------------------------------------------------
// Inlined constexpr values from the upstream:
//
//   sequential_bonus      = 15  — consecutive match in str.
//   separator_bonus       = 30  — match immediately after a word boundary
//                                  (' ', '_', '-' precede the match char).
//   camel_bonus           = 30  — lower→upper transition in str at match.
//   first_letter_bonus    = 15  — match is the first char of str.
//   leading_letter_penalty= -5  — penalty per unmatched leading char.
//   max_leading_penalty   = -15 — clamp on leading penalty.
//   unmatched_penalty     = -1  — penalty per non-matched body char.
//   kRecursionLimit       = 10  — max depth of the best-path recursion.
//
// USAGE
// --------------------------------------------------------------------------
//   if (tf::utils::fuzzy_match_simple("tx", "Switch view: Transactions")) {
//       // pattern characters all present in order.
//   }
//
//   int score = 0;
//   if (tf::utils::fuzzy_match("nw", "Net Worth", score)) {
//       // score reflects match quality; higher is better.
//   }
// ============================================================================

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cctype>
#include <string>
#include <string_view>

namespace tf::utils {

namespace fuzzy_match_detail {

// Upstream constants — see the file header for rationale.
inline constexpr int kSequentialBonus       = 15;
inline constexpr int kSeparatorBonus        = 30;
inline constexpr int kCamelBonus            = 30;
inline constexpr int kFirstLetterBonus      = 15;
inline constexpr int kLeadingLetterPenalty  = -5;
inline constexpr int kMaxLeadingPenalty     = -15;
inline constexpr int kUnmatchedPenalty      = -1;
inline constexpr int kRecursionLimit        = 10;
inline constexpr std::size_t kMaxMatches    = 256;

// Internal recursive worker.  Mirrors the upstream
// `fuzzy_match_recursive` exactly: walks pattern and str cursors, branching
// at every wildcard-eligible character to find the best-scoring match
// path subject to the recursion cap.
//
// `str_len` is the total length of the str string (NOT the offset of the
// current cursor) -- the unmatched-letter penalty uses the full string
// length minus the number of matched chars, per upstream.
inline bool fuzzy_match_recursive(
    const char* pattern,
    const char* str,
    int&        out_score,
    const char* str_begin,
    std::size_t str_len,
    const std::size_t* src_matches,
    std::size_t* matches,
    std::size_t  max_matches,
    std::size_t  next_match,
    int&         recursion_count) {

    ++recursion_count;
    if (recursion_count >= kRecursionLimit) {
        return false;
    }

    // Pattern exhausted or str exhausted means terminal.
    if (*pattern == '\0' || *str == '\0') {
        return false;
    }

    bool recursive_match = false;
    std::size_t best_recursive_matches[kMaxMatches];
    int best_recursive_score = 0;

    bool first_match = true;
    while (*pattern != '\0' && *str != '\0') {

        // Case-insensitive char compare.
        if (std::tolower(static_cast<unsigned char>(*pattern)) ==
            std::tolower(static_cast<unsigned char>(*str))) {

            if (next_match >= max_matches) {
                return false;
            }

            if (first_match && src_matches) {
                std::memcpy(matches, src_matches, next_match * sizeof(std::size_t));
                first_match = false;
            }

            // Recurse: explore the path that SKIPS this str char and
            // tries to match the pattern char later.  Keep the better
            // of {skip-here, take-here}.
            std::size_t recursive_matches[kMaxMatches];
            int recursive_score = 0;
            if (fuzzy_match_recursive(pattern, str + 1, recursive_score,
                                       str_begin, str_len,
                                       matches, recursive_matches,
                                       max_matches, next_match,
                                       recursion_count)) {
                if (!recursive_match || recursive_score > best_recursive_score) {
                    std::memcpy(best_recursive_matches, recursive_matches,
                                kMaxMatches * sizeof(std::size_t));
                    best_recursive_score = recursive_score;
                }
                recursive_match = true;
            }

            // Take-here path: record the match, advance both cursors.
            matches[next_match++] = static_cast<std::size_t>(str - str_begin);
            ++pattern;
        }
        ++str;
    }

    const bool matched = (*pattern == '\0');

    if (matched) {
        // Compute score for this match path.
        out_score = 100;

        // Leading-letter penalty (clamped).
        const int penalty = std::max(
            kLeadingLetterPenalty * static_cast<int>(matches[0]),
            kMaxLeadingPenalty);
        out_score += penalty;

        // Unmatched-letter penalty.  Upstream uses the FULL str length
        // minus the count of matched chars -- not the prefix consumed by
        // the walk.  This is what makes long unrelated strings score
        // lower than short ones that contain the same matches.
        const int unmatched =
            static_cast<int>(str_len) - static_cast<int>(next_match);
        out_score += kUnmatchedPenalty * (unmatched > 0 ? unmatched : 0);

        // Per-match bonuses.
        for (std::size_t i = 0; i < next_match; ++i) {
            const std::size_t idx = matches[i];

            if (i > 0) {
                const std::size_t prev_idx = matches[i - 1];
                if (idx == prev_idx + 1) {
                    out_score += kSequentialBonus;
                }
            }

            if (idx > 0) {
                const char neighbor = str_begin[idx - 1];
                const char curr     = str_begin[idx];

                if (std::islower(static_cast<unsigned char>(neighbor)) &&
                    std::isupper(static_cast<unsigned char>(curr))) {
                    out_score += kCamelBonus;
                }
                if (neighbor == ' ' || neighbor == '_' || neighbor == '-') {
                    out_score += kSeparatorBonus;
                }
            } else {
                out_score += kFirstLetterBonus;
            }
        }
    }

    // Pick the better of {matched here, best recursive match}.
    if (recursive_match && (!matched || best_recursive_score > out_score)) {
        std::memcpy(matches, best_recursive_matches,
                    max_matches * sizeof(std::size_t));
        out_score = best_recursive_score;
        return true;
    } else if (matched) {
        return true;
    } else {
        return false;
    }
}

}  // namespace fuzzy_match_detail

// ----------------------------------------------------------------------------
// fuzzy_match_simple — case-insensitive subsequence test.
//
// Returns true iff every character of `pattern` appears in `str`, in order,
// case-insensitively.  No scoring.  This is the cheap boolean filter used
// when score-ranking is unnecessary (or when an empty pattern should match
// everything — note: the empty pattern returns true here, matching the
// upstream's behaviour where an empty pattern trivially satisfies the
// "all characters appear in order" predicate).
// ----------------------------------------------------------------------------
inline bool fuzzy_match_simple(std::string_view pattern, std::string_view str) {
    std::size_t pi = 0;
    std::size_t si = 0;
    while (pi < pattern.size() && si < str.size()) {
        if (std::tolower(static_cast<unsigned char>(pattern[pi])) ==
            std::tolower(static_cast<unsigned char>(str[si]))) {
            ++pi;
        }
        ++si;
    }
    return pi == pattern.size();
}

// ----------------------------------------------------------------------------
// fuzzy_match — scored subsequence match.
//
// Returns true iff `pattern` matches `str` (every pattern char present in
// order, case-insensitive); on success `out_score` receives the score.
// Higher scores indicate better matches.  See the file header for the
// constants that determine relative weights.
//
// Empty pattern: returns true with out_score = 0 (no match needed; degree
// of match is zero).  Empty str with non-empty pattern: returns false.
// ----------------------------------------------------------------------------
inline bool fuzzy_match(std::string_view pattern, std::string_view str, int& out_score) {
    using namespace fuzzy_match_detail;

    if (pattern.empty()) {
        out_score = 0;
        return true;
    }
    if (str.empty()) {
        out_score = 0;
        return false;
    }

    // Stack-allocated matches buffer; caps at kMaxMatches.
    std::size_t matches[kMaxMatches] = {};
    int  recursion_count = 0;

    // NUL-terminated copies for the C-string API of the recursive worker.
    // string_view is not guaranteed to be NUL-terminated; create local
    // std::string copies to be safe.  For typical command-palette inputs
    // (pattern ≤ 32 chars, str ≤ 80 chars) this is negligible.
    std::string pat(pattern);
    std::string s(str);

    return fuzzy_match_recursive(
        pat.c_str(), s.c_str(), out_score,
        s.c_str(), s.size(),
        /*src_matches=*/nullptr, matches,
        kMaxMatches, /*next_match=*/0, recursion_count);
}

}  // namespace tf::utils
