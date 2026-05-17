# Greylock TUI style audit

> Reference: the 5-panel design Rory specced for the tiled HomeView.  Every
> view below is graded against the same conformance rules.  The audit is
> the source of truth for whether a view is allowed to ship; it must be
> re-run any time a new view lands.

## Conformance rules

A view passes the style audit iff:

1. **monospace, single weight** — no italics, no double-width glyphs, no
   bold for *emphasis*.  Bold reserved for headline values + focused-pane
   titles (token: `kTokens.fg_emphasized`).
2. **hierarchy via color + whitespace only** — no boxes-within-boxes,
   no nested borders, no "card" chrome.  Indentation and the `kTokens.fg_*`
   spectrum carry hierarchy.
3. **field separator is `·` (middle dot, U+00B7)** — never `|`, never `-`.
4. **tree glyphs are `├ ─ ╰`** (U+251C / U+2500 / U+2570), drawn
   left-aligned with two leading spaces.
5. **bars are full-block** (U+2588) and sparklines are the 1/8-block
   ramp `▁ ▂ ▃ ▄ ▅ ▆ ▇ █` (U+2581…U+2588).
6. **color semantics, single meaning each:**
   - `accent_positive` (green) — data is OK / cash on hand / positive
     delta / "on cadence".
   - `accent_warning` (amber) — needs attention / cadence behind /
     amber drift annotation / "you owe him" heading.
   - `accent_negative` (red) — relational-debt headings only ("he owes
     you").  *Never* used as a generic error tint inside data rows.
   - `fg_dim` (dim gray) — metadata, timestamps, footer chrome, labels.
   - `fg_default` (light gray / default) — body text.
   - `fg_emphasized` (white, bold permitted) — headline values, focused
     titles, bullets that should pop.
   - `thesis_up` (magenta) — sparklines + "going-up-is-interesting"
     widgets only.  Reserved.
7. **prompt strip** `rory@greylock:~ $ <cmd> ▮` at the bottom of every
   pane that simulates a shell.  Cursor is U+25AE.
8. **pane badge** `[N] <command>` in the top-left of every tile when the
   view is rendered inside a multi-pane composer.

Drift = any deviation from rules 1–8.  Drift in v0.3.x is acceptable
*only* if it's loud (logged here) and time-boxed (linked to a TODO).

## View-by-view audit (2026-05-16)

| View                          | Path                                  | Pass | Notes |
|-------------------------------|---------------------------------------|------|-------|
| HomeView (tiled composer)     | `src/views/HomeView.h`                | ✓    | Built this pass against rules 1-8.  Pane badges + prompt strips per tile; 180-col stacked-fallback path tested. |
| GraphView                     | `src/views/GraphView.h`               | ✓    | Tree glyphs correct; `eff_depth` path threads from `render_tile()` so depth=2 is enforced.  Footer condensed in tile mode. |
| AskView                       | `src/views/AskView.h`                 | ✓    | Bar uses U+2588 + `accent_positive`.  Framework-drift line goes amber once a capital-framework decision is logged (rule 6 amber). |
| DecisionDetailView            | `src/views/DecisionDetailView.h`      | ✓    | Field rows use `·` via DetailViewCommon::label_row.  Tile mode collapses 2-col → 1-col; full-pane preserves the right-rail layout. |
| RelationshipDetailView        | `src/views/RelationshipDetailView.h`  | ✓    | `he owes you` is red, `you owe him` is amber, sparkline uses the full 1/8-block ramp + magenta `thesis_up`. |
| DetailViewCommon (helpers)    | `src/views/DetailViewCommon.h`        | ✓    | Single source of truth for `label_row`, `heading`, `bulleted_list`, `tag_chips`.  All four detail views consume from here. |
| HelpOverlay                   | `src/views/HelpOverlay.cpp`           | ✓    | Sectioned cheat sheet; tokens correct. |
| **— follow-up commit below —** | | | |
| AccountsView                  | `src/views/AccountsView.h`            | ⚠    | Uses `LED_BLUE` + `LED_BLUE_DIM` from the v0.2 era for tab chrome (not in `kTokens`).  Body rows pass.  Migrate the chrome to `accent_info` + `fg_dim` in a follow-up. |
| TransactionsView              | `src/views/TransactionsView.h`        | ⚠    | Column separators are spaces (rule 3 says `·`).  Header strip uses `LED_BLUE`.  Reformat in follow-up. |
| BudgetView                    | `src/views/BudgetView.h`              | ⚠    | Bar glyph is `#` not U+2588 (rule 5).  Over-budget rows use amber correctly.  Glyph swap in follow-up. |
| CategoriesView                | `src/views/CategoriesView.h`          | ⚠    | List uses `-` not `·` between fields.  Pass on color tokens; fail on rule 3. |
| DashboardView (widget grid)   | `src/views/DashboardView.cpp`         | ⚠    | Pre-v0.3 widget grid; uses raw `Color::*` literals in places.  Migrate to tokens. |
| PlaceholderView               | `src/views/PlaceholderView.h`         | ✓    | Single-screen stub; conforms.  Used by every v3/v4 list surface awaiting real data. |
| Notes / Tasks / Events / Proposals / Targets list / Relationships list / Real Estate (v3/v4 list surfaces) | (all PlaceholderView)             | ✓    | Inherits PlaceholderView styling.  Real list views replace these one-by-one; each replacement is gated on a fresh audit row. |

Legend: ✓ = conforms; ⚠ = drift, follow-up commit on the same branch.

## Drift fixed in this commit

- **HomeView** was previously only Panel 1 (the morning digest).  It now
  composes all five reference panels into a 2×2 + bottom-spanning grid.
  Pane badges + prompt strips per tile.  Stacked-vertical fallback for
  terminals narrower than 180 cols.
- **GraphView** gained `render_tile()` reusing the same `build_tree_rows`
  helper that powers `render()`.  No layout-logic duplication.  Depth
  pinned to 2 in tile mode, footer condensed to a single dim line.
- **AskView** gained `render_tile()` that delegates to the existing
  `render_cash_position("pcc")` so the cash-position bars and source-line
  layout stay single-sourced.
- **DecisionDetailView** gained `render_tile()` reusing
  `DetailViewCommon::label_row` / `heading` / `body_paragraph` /
  `bulleted_list`.  Tile mode collapses the 2-col field layout into 4
  stacked rows + 2-line rationale + condensed alternatives.
- **RelationshipDetailView** gained `render_tile()` reusing the same
  helpers.  Tile mode drops the right rail (he-owes / you-owe / relevant
  decisions) and keeps cadence + working_on + last_real + sparkline.

## Drift to fix in the follow-up commit on this branch

- AccountsView / TransactionsView: migrate `LED_BLUE*` literals to
  `kTokens.accent_info` + `kTokens.fg_dim`.
- TransactionsView / CategoriesView: replace inter-field whitespace
  with `·`.
- BudgetView: bar glyph `#` → `█` (U+2588).
- DashboardView: complete the v0.3 token migration started in v0.3-5.

## Open questions Rory asked

These three came down with the spec; answers below are my recommendation,
not gospel.  Rory can flip any of them in a single PR.

### Q1: Panel 3's framework-comparison decision id source

**Recommendation: query-driven lookup by canonical slug.**

Reason: a config-driven id (env var or settings table) hardcodes a
specific decision row.  When the framework gets re-versioned (e.g.,
2026-11 you redefine the operating/treasury/deployed split), the env
var becomes stale + has to be rotated.  A canonical-slug query
(`/decisions?canonical_slug=three-tier-capital-framework`) means the
*latest confirmed* decision under that slug always wins.  Decisions
table needs one extra column (`canonical_slug TEXT NULL`).  Hardcoding
the lookup to slug `"three-tier-capital-framework"` in the App is fine
for v1; the column migration lands when that decision is actually
logged.

### Q2: Panel 5's "most recent relationship"

**Recommendation: most-overdue cadence, not most-recent interaction.**

Reason: most-recent interaction biases the panel toward whoever you've
already texted today, which is information you don't need surfaced —
you already know.  Most-overdue cadence (`MAX(NOW - last_contact_unix
- target_cadence_days * 86400)`) surfaces the person you should call
*right now* but haven't.  This matches the "JARVIS, not digital twin"
operating mode in `~/.claude/CLAUDE.md` — the panel should nudge you
toward action, not narrate the past.

Server-side: `GET /relationships?surface=overdue&limit=1` returns the
single most-overdue row.  If `target_cadence_days` is null for the row,
fall back to most-recent so the panel never goes empty.

### Q3: On home, should `g` keys still navigate away, or should hjkl-style focus cycle through tiles first?

**Recommendation: immediate navigation (current behavior).**

Reason: the dashboard widget grid (`g s` / Snapshot, tab 13) is the
view that owns `FocusController` and the hjkl-cycle pattern.  Home is
deliberately *read-only* — the tiles are at-a-glance signals, not
interaction targets.  If you want to dig into a tile, `g G` / `:ask` /
`:open decision …` already takes you to the full-pane version with
real keybindings.  Adding focus cycling on home would force every tile
to expose a "focused" visual state, which doubles the styling surface
without unlocking any new action.

If you change your mind later, the lowest-friction migration is a
keymap toggle: a config flag `home.focus_cycle = false|true` that
flips the home key handler.  No structural changes to HomeView itself
required; the tiles already know how to render in tile mode.

## Re-running the audit

Add a row above for every new view as it ships.  An audit row is a
contract: any future change to that view must re-grade it.  When a row
is downgraded from ✓ to ⚠, open a paired issue + link the issue id in
the Notes column so the drift doesn't slip past the next PR.
