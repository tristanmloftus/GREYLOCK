# TerminalFinance v0.3 — TUI UX Redesign Proposal

**Status:** DRAFT (research + design pass; no code changed).
**Author:** Claude, UX engineering pass, 2026-05-11.
**Predecessor:** [`V0_2_PLAN.md`](../V0_2_PLAN.md) Phase 5 shipped the five Dashboard widgets and the snapshot harness. This proposal builds on that surface.
**Reviewers:** Tristan (binary owner, Windows operator), Rory (server operator, macOS operator).

This is the design specification the v0.3 implementation phase will execute against. Where it makes a decision, the orchestrator acts on it. Where it surfaces an open question (§5), Rory and Tristan agree before any dependent task dispatches.

The thesis in one sentence: **v0.2 is a static dashboard; v0.3 turns it into a launching pad by adding widget-level focus, Enter-to-drill, and a `:`-driven command palette.**

---

## Table of contents

1. [Diagnosis](#1-diagnosis)
2. [Patterns from prior art](#2-patterns-from-prior-art)
3. [Proposed v0.3 interaction model](#3-proposed-v03-interaction-model)
   - [3a. Focus model](#3a-focus-model)
   - [3b. Drill-down flow (per widget)](#3b-drill-down-flow-per-widget)
   - [3c. Command palette](#3c-command-palette)
   - [3d. Status bar redesign](#3d-status-bar-redesign)
   - [3e. Color palette (semantic)](#3e-color-palette-semantic)
   - [3f. Keybinding map](#3f-keybinding-map)
4. [v0.3 implementation phase plan](#4-v03-implementation-phase-plan)
5. [Open questions](#5-open-questions)

---

## 1. Diagnosis

The v0.2 TUI is **competent and lifeless**. Every pixel is correct; nothing invites interaction.

### 1.1 What's actually broken

There are four specific, namable maneuverability gaps. They are not stylistic complaints; each one represents a flow the user wants to perform and can't.

**Gap 1 — The Dashboard is read-only.**
Five widgets render side-by-side: `ui_net_worth`, `ui_shovel_score`, `ui_sync_status`, `ui_shovel_intelligence`, `ui_category_trends`. Composed in `src/views/DashboardView.cpp:264` as `vbox({hbox({net_worth | shovel_score | sync_status}), hbox({shovel_intel | category_trends})})`. The user can read all five panels but cannot:

- Highlight one of them.
- Press anything that scopes the rest of the UI to "this widget".
- Get to the underlying transactions, accounts, or supplier history behind a number.

Concretely: the user reads "NVDA  $2500.00  ^ 120.0% MoM" in `tests/snapshot/fixtures/shovel_intelligence.txt:4` and wants to see the 38 transactions that built that number. There is no key that does that. To investigate, the user has to: (a) Tab to Transactions, (b) mentally filter for NVDA-coded merchants in a list that doesn't expose the supplier mapping, (c) tally by hand.

**Gap 2 — Tab is the only navigation key for views; arrows are the only nav inside views; nothing exists between.**
`src/main.cpp:752-773` wires:

- `Tab` / `Shift-Tab` — cycle the four top-level views.
- `ArrowUp` / `ArrowDown` — move the selection index inside Accounts / Transactions / Budget lists.
- `1` / `2` — switch entity.

Nothing moves focus *within* the Dashboard. The Dashboard widgets are an unstructured visual collage. `Tab` from Dashboard goes to Accounts; there is no "focus the net-worth panel" key.

**Gap 3 — Action discoverability hits a ceiling.**
The status bar at `src/main.cpp:235` is one flat line:

```
[1-2] Switch entity  [Tab] Switch view  [P] Link Plaid  [L] Link test  [C] Config  [Q] Quit
```

This is the entire affordance surface. Every action the user can take is listed here. Add a 7th action and the line wraps. Add a 15th and the user has to memorize it. There is no `?` for a help overlay, no `:` for command search, no per-view contextual hints.

**Gap 4 — Color carries information ambiguously.**
The current palette in `src/views/ViewCommon.h:10-11` is `LED_BLUE` (chrome) plus stock FTXUI colors. Red means three different things depending on widget:

- Negative balance (`src/views/ViewCommon.h:25`: `val < 0 → Color::Red`).
- Spending going up MoM (`src/views/widgets/ui_category_trends.cpp:44-46`: `percent_change > 0 → Color::Red`, because more spending is bad).
- Negative credit balance (`src/views/widgets/ui_net_worth.cpp:41-42`: `credit < 0 → Color::Red`).

A user looking at the Shovel Intelligence panel — `tests/snapshot/fixtures/shovel_intelligence.txt:4` shows `NVDA $2500.00 ^ 120.0% MoM` in red — can reasonably read that as "this is bad". It is in fact the opposite: NVDA spend going up is the headline thesis the product is trying to surface. The widget at `src/views/widgets/ui_shovel_intelligence.cpp` inverted the polarity by reusing the same color convention.

### 1.2 Cited snapshots showing current state

| Widget | Fixture | Observation |
| --- | --- | --- |
| Net Worth | `tests/snapshot/fixtures/net_worth_positive.txt` | Dense, dark `LED_BLUE` chrome; no focus indicator; no key for "show me the accounts that make this up". |
| Net Worth (negative) | `tests/snapshot/fixtures/net_worth_negative.txt:4` | `-$14900.00` rendered in red — but so is the credit row at `:8` (`-$15000.00`). Red is doing double duty. |
| Shovel Score | `tests/snapshot/fixtures/shovel_score.txt:4-5` | `72/100  EARLY ADOPTER` — but no key to see *which* suppliers produced this score. |
| Shovel Intelligence | `tests/snapshot/fixtures/shovel_intelligence.txt:4-7` | Top 4 tickers with MoM %. Red `^` for "going up" — semantically inverted from the product thesis. |
| Sync Status | `tests/snapshot/fixtures/sync_status.txt:5` | `[x] Bank of America  auth failed` — the user reads this and the only action available is `[P]` or `[L]`, neither of which is a per-institution re-auth. |
| Category Trends | `tests/snapshot/fixtures/category_trends.txt` | Same red-up / green-down convention as Net Worth, semantically defensible here ("less spending = good") but reinforces Gap 4. |
| Updates | `tests/snapshot/fixtures/updates.txt` | A standalone supplier-mapping list. Not wired into Dashboard composition. v0.3 will absorb it into a drill view. |
| Consolidation UI | `tests/snapshot/fixtures/consolidation_ui.txt` | Same data as Sync Status, different framing. Probably the right home for a drill-in from `ui_sync_status`. |

### 1.3 What v0.2 has right and v0.3 must not break

- The five composed widgets exist and have snapshot coverage. Don't regress them; extend.
- `DataStore` provides everything needed for every drill-down envisioned below. `data_store_.transactions`, `get_accounts_for_entity`, `DiscoveryService::mapToSupplier` — already there.
- The `set_entity_id` filter pattern (`src/views/DashboardView.h:27`) propagates correctly when the user presses `1` or `2`. The drill-down model preserves this.
- The two-binary architecture from V0_2_PLAN.md §a is orthogonal to UX. None of this proposal touches `BackendClient` or the server.

---

## 2. Patterns from prior art

Four projects studied. One paragraph of what the pattern is, one of how it lands on TerminalFinance.

### 2.1 lazygit — focused panel with active-border highlighting

**Pattern.** lazygit shows 4–6 side panels simultaneously (branches, commits, stash, status, files) plus one main panel. Exactly one side panel is "focused" at any time. The focused panel gets a green bold border via the `activeBorderColor: [green, bold]` theme key; non-focused panels render with `inactiveBorderColor: [default]`. `<tab>` cycles between side panels; `]` and `[` cycle tabs *within* a panel; `<enter>` drills into the focused panel's selection (e.g., enter on a branch shows its commits in the main panel); `<esc>` returns one level up. Keys are heavily context-sensitive: `s` in commits squashes; `s` in files stages. The bottom status line shifts to reflect the focused panel's hotkeys, and `?` opens a full keybinding overlay. Modal pickers (rebase options, etc.) layer on top.

**Applied to TerminalFinance.** Today's Dashboard composition (`src/views/DashboardView.cpp:264`) becomes a five-panel grid where exactly one panel is focused. Focus shows as a yellow bordered panel (FTXUI: replace the widget's `border` call with `borderStyled(ROUNDED)` on the focused panel and `borderStyled(LIGHT)` on the rest, plus `color(Color::Yellow)` on the border element). `Tab` cycles to the next widget; `Shift-Tab` to the previous. `Enter` drills into the focused widget's data. `Esc` returns to the Dashboard with no widget focused. The status bar at `src/main.cpp:235` becomes contextual — different per focused widget. This is the single most important pattern in this proposal.

### 2.2 lazygit + helix — `:` command palette, `/` search, `?` help

**Pattern.** Both editors gate "I know the name of the thing I want, just let me type it" behind a single keystroke prefix. `:` opens a one-line minibuffer at the bottom of the screen with a prompt (`> `); the user types the command name and presses Enter. `/` opens a similar minibuffer scoped to the current view as a substring/regex filter. `?` opens a full-screen overlay listing every key binding in the current context. The minibuffer typically supports fuzzy completion: typing `:tx` matches "Switch to Transactions", "Search transactions", "Filter transactions by..." and ranks them. lazygit additionally rebinds the entire keyboard layer when a panel is focused: the same physical key produces different completions in different contexts.

**Applied to TerminalFinance.** `:` opens a command palette with verbs like `:plaid-link`, `:account-link-test`, `:config`, `:switch-entity personal`, `:goto transactions`, `:refresh`, `:export csv`. The vocabulary is small (~20 actions in v0.3, designed to grow). Fuzzy match via a checked-in single-header library (recommendation in §3c) so we don't introduce a heavy dependency. `/` is reserved for in-view filter (search transactions, search supplier list). `?` opens a help overlay built off the same data structure that powers the keybinding map (§3f) — meaning if a maintainer adds a new key, the help overlay updates automatically. The command palette is the answer to Gap 3: discoverability without status-bar clutter.

### 2.3 k9s — drill-down hierarchy with breadcrumbs and resource shortcuts

**Pattern.** k9s navigates the Kubernetes object graph by typing `:resource-name` (e.g., `:pod`, `:deployment`, `:ns`). Inside a list, `<enter>` drills into the row (pod → containers → shell). The current path is rendered as breadcrumbs at the bottom: `pods(default)` → after Enter on a pod row → `pods(default)>pod-xyz>containers`. `<esc>` pops one breadcrumb level. Filters use `/<pattern>` to regex-match the visible list; `/-l label=value` filters by label; `/-f fuzzy` switches to fuzzy match. The combination makes a ten-thousand-resource cluster feel like a tree the user is walking.

**Applied to TerminalFinance.** Each drill-in (§3b) renders a breadcrumb at the top: `Dashboard > Net Worth > Chase Checking …4242`. `Esc` walks the breadcrumb back one segment. `/<pattern>` in any list view (Accounts, Transactions, Suppliers) filters by description/name. This composes cleanly with the focus model: focus a widget → `Enter` to drill → `/<filter>` to narrow → `Enter` on a row to drill one more level → `Esc` × N to return. The breadcrumb is the answer to "where am I and how did I get here".

### 2.4 btop / htop — selective visibility and toggleable widgets

**Pattern.** btop never assumes all widgets need to be on screen. The user picks a preset (`1`–`9`) and each preset shows a different subset (`cpu mem net proc` vs `cpu gpu0 gpu1`). Mouse and keyboard both work. Color is used sparingly and semantically: percentage gradients (green → yellow → red) on CPU/memory utilization track the *meaning* of the value, not the *category* of the panel.

**Applied to TerminalFinance.** v0.3 keeps all five widgets visible by default but treats this as a layout choice, not a fixed truth. Future v0.4 work can ship layout presets (`1`: "today" — sync + transactions; `2`: "month" — net worth + budget + categories; `3`: "shovel" — shovel intel + shovel score full-screen). For v0.3 the takeaway is narrower: stop using red to mean "category: bad". Use it to mean "value is below zero" (a magnitude statement), reserve amber for "warning: needs attention", and let the Shovel Intelligence panel use a *neutral* color for `^` arrows since the polarity of "increasing supplier spend" is thesis-dependent and not universally bad.

### 2.5 helix — selection-first, mode-prefix mnemonics

**Pattern.** Helix never separates "what" from "how": you select first, then act on the selection. The `Space` key is the universal prefix for "command palette" actions (`Space f` find file, `Space b` buffers, `Space k` hover doc). Sub-modes are entered with a single character: `g` → goto mode (`gg` top, `ge` end, `gd` definition), `m` → match (`mi(` select inside parens). Every mnemonic is two strokes; muscle memory builds fast.

**Applied to TerminalFinance.** The "selection-first" idea translates directly: focus a widget, then everything the user types is interpreted relative to that widget. Press `r` on a focused Sync Status widget → re-sync. Press `r` on a focused Shovel Intelligence widget → re-run discovery. Same key, different effect by context — the keybinding map (§3f) is small per context, large in aggregate. We do not implement a Helix-style multi-key prefix in v0.3 (overkill for the action vocabulary). We do reserve `Space` as the "leader" key for v0.4 if the vocabulary grows.

### 2.6 Synthesis — patterns adopted vs deferred

| Pattern | Source | v0.3? | Notes |
| --- | --- | --- | --- |
| Side-panel + main-panel two-region layout | lazygit | **No** | Our information shape is widgets-on-a-dashboard; the lazygit model assumes one main content stream (a diff) and side context. Net worth + shovel intel + sync status are coequal. |
| Focused-panel highlight by border color | lazygit | **Yes** | Yellow border on focused widget. Single strongest UX win in the proposal. |
| `Tab` cycles focus | lazygit | **Yes** | Within Dashboard, between widgets; in other views, between top-level views. |
| `Enter` to drill, `Esc` to back out | lazygit + k9s | **Yes** | Drill-views per widget; breadcrumb at top of each drill. |
| `:` command palette | lazygit + helix + k9s + vim | **Yes** | Single keystroke. Fuzzy match via vendored single-header. |
| `/` filter in current view | lazygit + k9s + helix | **Yes** | Substring match against visible rows. Esc clears. |
| `?` help overlay | lazygit + less + vim | **Yes** | Generated from the same `KeyHint` registry the status bar uses. |
| Contextual key hints in status bar | lazygit | **Yes** | Two-row footer: top contextual, bottom global. |
| Resource-shortcut completion (`:po`) | k9s | **Partial** | Palette has fuzzy match; we do not auto-execute on partial match. `:tx`+Enter executes the top-ranked entry. |
| Layout presets (`1`–`9`) | btop | **No** | Deferred to v0.4. v0.3 keeps the fixed five-widget Dashboard. |
| Toggleable widgets | btop | **No** | Deferred to v0.4. The five widgets are the proven set. |
| Semantic color discipline | btop + GUI conventions | **Yes** | Red = below zero or error. Yellow = focus. Magenta = "thesis-positive event". §3e. |
| Selection-first mode | helix | **Partial** | We adopt the *idea* (focus a widget, then interpret keys relative to it) but not Helix's literal selection model — there is no "selection extension" in a finance TUI. |
| `Space` as leader-key prefix | helix | **No** | Reserved for v0.4 if the action vocabulary outgrows the palette. |
| Mouse support | btop | **No** | Out of scope. FTXUI does support mouse but TerminalFinance is keyboard-first and the existing operators work over SSH. |

The asymmetry is intentional: we adopt every pattern that *increases keyboard agency*; we defer every pattern that adds modes or layouts before we know the user wants them.

---

## 3. Proposed v0.3 interaction model

### 3a. Focus model

#### Where does focus live

Focus is a single integer in the App struct (`src/main.cpp:49`) representing a *focusable target*. The set of focusable targets at any moment is:

```
focus_target ::= None                      // Dashboard-level, no widget focused
              | View(view_id)              // top-level view tab
              | Widget(view_id, widget_id) // a specific widget on Dashboard
              | Row(view_id, row_index)    // a row in Accounts/Transactions/Budget
              | DrillView(drill_id)        // user has drilled into a widget
              | Modal(modal_id)            // command palette, help overlay, etc.
```

In v0.3, only `None`, `View`, `Widget(Dashboard, *)`, `Row(*)`, `DrillView(*)`, and `Modal(:|?)` are implemented. The model is intentionally hierarchical: focus *descends* from view → widget → row → drill. Esc *ascends* one level.

State lives in a new `FocusController` class (header `src/views/FocusController.h`, impl `src/views/FocusController.cpp`). It owns:

```cpp
class FocusController {
public:
  enum class Level { Dashboard, Widget, Row, DrillView, Modal };
  Level level() const;
  std::optional<WidgetId> focused_widget() const;
  void focus_next_widget();   // Tab on Dashboard
  void focus_prev_widget();   // Shift-Tab on Dashboard
  void focus_left();          // h
  void focus_right();         // l
  void focus_up();            // k
  void focus_down();          // j
  bool drill_into();          // Enter; true if a drill is available
  bool escape_one_level();    // Esc
  // ... modal entry/exit, etc.
};
```

The App passes the FocusController to each view's `render()` so views can read focus and decorate themselves accordingly. Views do **not** mutate focus; the App's event loop is the only mutator.

#### How focus moves

| Key | When | Action |
| --- | --- | --- |
| `Tab` | Dashboard, no widget focused | Focus the first widget (top-left: `ui_net_worth`). |
| `Tab` | Dashboard, widget focused | Focus the next widget in reading order. After last, wrap to first. |
| `Tab` | Inside a non-Dashboard view (Accounts, Transactions, Budget) | Cycle to the next top-level view. (Preserves v0.2 behavior outside Dashboard.) |
| `Shift-Tab` | Symmetric reverse | — |
| `h` / `l` | Dashboard, widget focused | Move focus to the spatially-left / spatially-right widget. If none, no-op. |
| `j` / `k` | Dashboard, widget focused | Move focus down / up. |
| `h` / `l` | List view (Accounts/Transactions/Budget) | No-op (single-column layouts; reserved for future column nav). |
| `j` / `k` | List view, row focused | Same as `ArrowDown` / `ArrowUp`. |
| Arrow keys | Anywhere | Always work as their vim equivalents. Beginners use arrows; vim users use hjkl. Both supported. |
| `Enter` | Widget focused | Drill into that widget (§3b). |
| `Enter` | Row focused | Drill into that row (account → transactions for account; transaction → detail). |
| `Esc` | DrillView | Pop one drill level. |
| `Esc` | Widget focused | Unfocus → Dashboard-level. |
| `Esc` | Dashboard-level | No-op (or, debated: quit confirm — see §5 Q3). |
| `Esc` | Modal open | Close modal. |
| `g g` | List view | Jump to first row (Helix mnemonic). |
| `G` | List view | Jump to last row. |

#### How focus is shown visually

Three signals stack. Use all three so partial-color terminals still indicate focus:

1. **Border style/color.** Focused widgets render with `borderStyled(ROUNDED)` + `color(Color::Yellow)` on the border. Non-focused widgets render with `borderStyled(LIGHT)` + the dim chrome color. FTXUI primitives:

   ```cpp
   Element widget_panel = ...; // existing render
   if (focused) {
       widget_panel = widget_panel | borderStyled(ROUNDED) | color(Color::Yellow);
   } else {
       widget_panel = widget_panel | borderStyled(LIGHT) | color(LED_BLUE_DIM);
   }
   ```

2. **Title brightness.** The widget's title row (currently `text("Net Worth") | bold` at `src/views/widgets/ui_net_worth.cpp:30`) renders bright-white on focus, dim on blur. Implementation: pass a `bool focused` parameter into each widget's `Renderer` function and inflect the title.

3. **Status-bar context.** The bottom status bar reflects the focused widget's actions (§3d). This is the strongest signal for "I know what I can do right now" — a user who's never seen a TUI before can read `[Enter] Show accounts breakdown  [r] Refresh` and immediately know which widget is the subject.

For row focus inside list views, the existing `bold | color(LED_BLUE)` highlight (`src/views/AccountsView.h:54`) is kept and gets a leading `▸ ` glyph for visual gravity.

#### Default: no widget focused

On startup, `FocusController::level()` is `Dashboard`. No widget is yellow-bordered. The status bar shows the global actions (§3d, "top-level" example).

The user pressing `Tab` once enters Widget level on the first widget. Pressing `Esc` returns to Dashboard level. Pressing `Tab` from `View(Accounts)` cycles to `View(Transactions)` — top-level view navigation is preserved in the non-Dashboard views because they don't have a widget grid.

This means **Dashboard is special**: it's the only view where `Tab` enters widget-level focus instead of cycling views. Once a Dashboard widget is focused, `Shift-Tab` past the first widget pops back to Dashboard level, and `Tab` then cycles top-level views again. This mirrors lazygit's "side panels first, then tab cycles tabs-within-panel".

#### Focus state machine

The full transition graph. Read each row as: "from this state, this event yields this state, optionally firing this side effect."

```
                  ┌──────────────────────┐
                  │       Dashboard      │ ◄── initial state
                  │   (no widget focus)  │
                  └──────────┬───────────┘
                             │
                Tab          │           Tab (from non-Dashboard view)
            ┌────────────────┼──────────────────────┐
            │                │                      │
            ▼                ▼                      ▼
  ┌──────────────────┐ ┌──────────────────┐  ┌────────────────────┐
  │ Widget(net_worth)│ │ Widget(shvl_int) │  │ View(Accounts)     │
  └────────┬─────────┘ └────────┬─────────┘  │ row focus           │
           │ Enter              │            └─────────┬──────────┘
           ▼                    ▼                      │ Enter
  ┌──────────────────┐ ┌──────────────────┐            ▼
  │ Drill(net_worth) │ │ Drill(shvl_int)  │  ┌────────────────────┐
  └────────┬─────────┘ └────────┬─────────┘  │  Detail(account)   │
           │ Enter on row       │ Enter      └────────────────────┘
           ▼                    ▼
  ┌──────────────────┐ ┌──────────────────┐
  │ Drill(account →  │ │ Drill(supplier → │
  │  txns)           │ │  txns)           │
  └──────────────────┘ └──────────────────┘

  All states: Esc pops one level. : opens Modal(palette). ? opens Modal(help).
  From any Modal: Esc → restore previous state.
```

Transition table (machine-readable; matches the implementation in `FocusController::handle_event`):

| Current state | Event | Next state | Side effect |
| --- | --- | --- | --- |
| Dashboard | `Tab` | `Widget(w0)` | none |
| Dashboard | `Shift-Tab` | `Widget(w4)` | none (wrap to last widget) |
| Dashboard | `Esc` | Dashboard | none (no-op per §5 Q3) |
| Dashboard | `:` | `Modal(palette)` | save prev state |
| Dashboard | `?` | `Modal(help)` | save prev state |
| `Widget(w)` | `Tab` / `l` / `j` (depending on grid neighbor) | `Widget(next)` | none |
| `Widget(w)` | `Shift-Tab` / `h` / `k` | `Widget(prev)` or `Dashboard` if wrap-back | none |
| `Widget(w)` | `Enter` | `Drill(w)` | render drill |
| `Widget(w)` | `Esc` | `Dashboard` | none |
| `Drill(w)` | `j` / `k` | `Drill(w)` (selected_++ / --) | re-render |
| `Drill(w)` | `Enter` on a row | `Drill(w, depth+1)` or `View(accounts)` filtered | depends on drill |
| `Drill(w)` | `/` | `Drill(w)` + filter prompt | enter filter mode |
| `Drill(w)` | `Esc` | `Widget(w)` | pop |
| `Modal(*)` | `Enter` | execute → previous state | run command |
| `Modal(*)` | `Esc` | previous state | none |

The state machine is small enough to live in one `FocusController.cpp` file and round-trip through GoogleTest unit tests in v0.3-1.

#### Widget IDs and reading order

For the `Tab` cycle to be deterministic, each widget gets a stable `WidgetId` enum:

```cpp
enum class WidgetId {
    NetWorth      = 0,  // top-left
    ShovelScore   = 1,  // top-center
    SyncStatus    = 2,  // top-right
    ShovelIntel   = 3,  // bottom-left
    CategoryTrend = 4,  // bottom-right
};
```

Reading order is left-to-right, top-to-bottom (matches `src/views/DashboardView.cpp:264`'s composition). `Tab` increments; `Shift-Tab` decrements. `h`/`l`/`j`/`k` use a static 2-row × 3-column grid lookup table (with `(1,2)` = unused; `j` from `SyncStatus` lands on `CategoryTrend` because rows shift left).

The grid table is six entries, hand-coded:

```cpp
//                  Up           Down           Left        Right
{NetWorth,       NetWorth,    ShovelIntel,    NetWorth,   ShovelScore},
{ShovelScore,    ShovelScore, ShovelIntel,    NetWorth,   SyncStatus},
{SyncStatus,     SyncStatus,  CategoryTrend,  ShovelScore,SyncStatus},
{ShovelIntel,    NetWorth,    ShovelIntel,    ShovelIntel,CategoryTrend},
{CategoryTrend,  ShovelScore, CategoryTrend,  ShovelIntel,CategoryTrend},
```

(Wrapping disabled at grid edges; movement key is a no-op at the edge. This matches Helix's behavior — Vim, by contrast, wraps. We pick non-wrap because it's less surprising; the user sees the focus stay put and knows they're at the edge.)

---

### 3b. Drill-down flow (per widget)

Each Dashboard widget defines a `drill_into()` method that returns a Drill view object. The Drill view is a full-screen replacement for the Dashboard render until Esc returns. Breadcrumbs render at the top.

#### Drill 1 — Net Worth → Per-Account Breakdown

**What "drill into" means.** Show every account contributing to the Net Worth headline, one row each, with balance, type, last-sync, and the breakdown by entity if no entity filter is active.

**Detail view (120 × 40 ASCII mockup):**

```
╭────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╮
│ Dashboard > Net Worth                                                                                  [Esc] Back       │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                                        │
│   Net Worth (Personal)                                              $13,234.56                                          │
│   ───────────────────────────────────────────────────────────────────────────────                                       │
│                                                                                                                        │
│     #   Account                       Type        Balance      Last sync     Institution                               │
│   ▸ 1   Checking …4242                Checking    $1,234.56    2026-04-01    Chase                                     │
│     2   Emergency Savings             Savings     $2,500.00    2026-04-01    Chase                                     │
│     3   Sapphire Reserve              Credit       -$500.00    2026-04-01    Chase                                     │
│     4   Brokerage …9999               Investment  $10,000.00   2026-03-28    Fidelity                                  │
│                                                                                                                        │
│   ───────────────────────────────────────────────────────────────────────────────                                       │
│     Sum                                            $13,234.56                                                           │
│                                                                                                                        │
│                                                                                                                        │
│   Breakdown by type                                                                                                    │
│     Checking      $1,234.56  ████░░░░░░░░░░░░░░░░░░░░░░  9.3%                                                          │
│     Savings       $2,500.00  ████████░░░░░░░░░░░░░░░░░░ 18.9%                                                          │
│     Credit         -$500.00  ░░░░░░░░░░░░░░░░░░░░░░░░░░  liability                                                      │
│     Investment   $10,000.00  ████████████████████░░░░░░ 75.6%                                                          │
│                                                                                                                        │
│                                                                                                                        │
│                                                                                                                        │
│                                                                                                                        │
│                                                                                                                        │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ [j/k] Select  [Enter] Open account  [/] Filter  [r] Refresh  [Esc] Back to Dashboard                                   │
╰────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

**Data sources.**
- `DataStore::get_accounts_for_entity(entity_id_)` — exists (`src/models/DataStore.h:33`).
- `DataStore::accounts` (raw vector) — exists (`src/models/DataStore.h:74`).
- Last-sync — derived the same way `DashboardView.cpp:227-245` already derives it (newest tx date per institution). Reuse that helper; promote to `src/views/Drill_NetWorth.cpp`.

**Backend.** None needed beyond what DataStore already exposes.

**Esc behavior.** `Esc` → pops back to Dashboard with `ui_net_worth` still focused. A second `Esc` un-focuses.

**Composability.** `Enter` on a row drills one level deeper into "Transactions for this account" — which is the existing `TransactionsView` filtered to the account. Reuse `TransactionsView::set_account_id` (`src/views/TransactionsView.h`).

#### Drill 2 — Shovel Score → Score Composition

**What "drill into" means.** Explain the 72/100 number. Show: the formula (the v0.2 stop-gap of "median MoM velocity across top-10 suppliers, clamped to [0, 100]" lives in `src/views/DashboardView.cpp:87-95`), the per-supplier inputs, and the suppliers ranked by contribution.

**Detail view (120 × 40 ASCII mockup):**

```
╭────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╮
│ Dashboard > Shovel Score                                                                               [Esc] Back       │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                                        │
│            72 / 100                       EARLY ADOPTER                                                                 │
│            ▮▮▮▮▮▮▮▮▮▮▮▮▮▮▮▮▮▮░░░░░░░░░                                                                                  │
│                                                                                                                        │
│   Method   median MoM velocity across top-10 suppliers (by absolute spend), clamped [0, 100]                            │
│            v0.2 stop-gap formula; ref src/views/DashboardView.cpp:87-95                                                 │
│                                                                                                                        │
│   Inputs (sorted by absolute spend)                                                                                    │
│                                                                                                                        │
│     Rank  Ticker  Spend (3mo)   Cur Mo     Prev Mo    Velocity (MoM)    In top-10?                                     │
│   ▸  1    NVDA    $2,500.00     $1,200.00  $545.00      +120.0%          yes                                           │
│      2    AMZN    $1,800.00       $900.00  $783.00       +15.0%          yes                                           │
│      3    MSFT      $950.00       $310.00  $345.00       -10.0%          yes                                           │
│      4    GOOGL     $640.00       $200.00  $200.00         0.0%          yes                                           │
│                                                                                                                        │
│   Median of top-10 |velocity|  =  72.5  →  clamp [0, 100]  →  72                                                        │
│                                                                                                                        │
│                                                                                                                        │
│   Bands                                                                                                                │
│     0  – 24   No-go                                                                                                    │
│     25 – 49   Watching                                                                                                 │
│     50 – 74   Early Adopter      ◀ you are here                                                                         │
│     75 – 100  Believer                                                                                                 │
│                                                                                                                        │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ [j/k] Select supplier  [Enter] Drill to supplier transactions  [r] Recompute  [Esc] Back                               │
╰────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

**Data sources.**
- The same per-ticker aggregation already in `DashboardView::render` (`src/views/DashboardView.cpp:183-211`). Extract to `DiscoveryService::aggregate_supplier_spend(transactions, current_month, prev_month)` returning a `std::vector<SupplierSpend>` — [backend method needed], lightweight refactor of existing inline code.
- `DiscoveryService::mapToSupplier()` — exists (`src/services/DiscoveryService.h:83`).

**Backend.** Lightweight refactor only; the math already runs every Dashboard render. v0.3 promotes it to a service method so the Drill view and the widget share one truth.

**Esc.** Pops to Dashboard with `ui_shovel_score` focused.

**Onward drill.** `Enter` on a supplier row goes to Drill 4 (Shovel Intelligence → per-supplier transactions).

#### Drill 3 — Sync Status → Per-Institution Sync Detail + Re-auth

**What "drill into" means.** Show every institution's sync state, last-success timestamp, last-failure reason, and offer per-institution actions: `r` re-sync now, `R` re-auth (Plaid Link), `x` remove.

**Detail view (120 × 40 ASCII mockup):**

```
╭────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╮
│ Dashboard > Sync Status                                                                                [Esc] Back       │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                                        │
│   Institutions                                                                                                         │
│                                                                                                                        │
│     #   Institution         Status           Last sync          Accounts    Action                                     │
│   ▸ 1   Chase               [+] connected    2026-04-01 09:14   3           ─                                          │
│     2   Bank of America     [!] auth failed  2026-03-22 04:00   2           re-auth pending                            │
│     3   Fidelity            [+] connected    2026-03-28 09:14   1           ─                                          │
│                                                                                                                        │
│   ────────────────────────────────────────────────────────────────────────────────                                      │
│   Details — Bank of America (selected)                                                                                 │
│                                                                                                                        │
│     item_id           plaid-item-bofa-7c2f                                                                             │
│     last_success      2026-03-22 04:00:11 UTC                                                                          │
│     last_attempt      2026-04-01 04:00:03 UTC  →  ITEM_LOGIN_REQUIRED                                                  │
│     accounts          Checking …8821, Savings …8829                                                                    │
│                                                                                                                        │
│     Recommended action                                                                                                 │
│     Run [R] to launch Plaid Link → re-authenticate this Item.                                                          │
│     If re-auth is not possible, run [x] to remove the Item; transactions are                                           │
│     preserved server-side.                                                                                             │
│                                                                                                                        │
│                                                                                                                        │
│                                                                                                                        │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ [j/k] Select institution  [r] Re-sync now  [R] Re-auth (Plaid Link)  [x] Remove  [Esc] Back                            │
╰────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

**Data sources.**
- `DataStore::accounts` (filter by institution) — exists.
- Newest tx date per institution — derived (`src/views/DashboardView.cpp:227-245`); reuse.
- Plaid Item ID, last_attempt outcome — **[backend method needed]**: `BackendClient::get_sync_status() → std::vector<InstitutionSyncRecord>`. Server-side schema already has `accounts.institution` and the `audit_log` table per V0_2_PLAN.md §f; this is mostly an aggregation endpoint, not new storage.
- Re-auth flow — Phase 4 of v0.2 wires Plaid Link via `PlaidService::link_account` (`src/main.cpp:792`). v0.3 just exposes the existing call from the drill view bound to `R`.

**Esc.** Pops to Dashboard with `ui_sync_status` focused.

#### Drill 4 — Shovel Intelligence → Per-Supplier Transaction List

**What "drill into" means.** Top-level shows the ranked supplier list (today's widget content); pressing `Enter` on a supplier row shows every transaction that the discovery engine mapped to that supplier — the answer to "what 38 transactions add up to NVDA $2,500".

**Detail view, level 1 (supplier list):**

```
╭────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╮
│ Dashboard > Shovel Intelligence                                                                        [Esc] Back       │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                                        │
│   AI-infrastructure suppliers (3-month aggregate, all entities)                                                        │
│                                                                                                                        │
│     #   Ticker  Supplier                  Spend (3mo)   MoM       Count    Last txn                                    │
│   ▸ 1   NVDA    NVIDIA                    $2,500.00     +120.0%      8     2026-04-12                                  │
│     2   AMZN    Amazon Web Services       $1,800.00      +15.0%     12     2026-04-11                                  │
│     3   MSFT    Microsoft Azure             $950.00      -10.0%      4     2026-04-09                                  │
│     4   GOOGL   Google Cloud Platform       $640.00        0.0%      3     2026-04-08                                  │
│                                                                                                                        │
│   ────────────────────────────────────────────────────────────────────────────────                                      │
│   Why each ticker matched                                                                                              │
│     NVDA   match='NVIDIA'  rule_kind=Contains  src/services/DiscoveryService.h:47                                      │
│            8 transactions; 100% description-match (no fuzzy fallback hits)                                             │
│                                                                                                                        │
│                                                                                                                        │
│                                                                                                                        │
│                                                                                                                        │
│                                                                                                                        │
│                                                                                                                        │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ [j/k] Select supplier  [Enter] View transactions  [/] Filter  [r] Refresh map  [Esc] Back                              │
╰────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

**Detail view, level 2 (supplier → transactions; Enter on a supplier row):**

```
╭────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╮
│ Dashboard > Shovel Intelligence > NVDA                                                                 [Esc] Back       │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│   NVIDIA  NVDA   $2,500.00 (3mo)   8 transactions   +120% MoM                                                          │
│                                                                                                                        │
│     Date         Account                    Amount      Description                                                    │
│   ▸ 2026-04-12   Sapphire Reserve …4242    -$420.00     NVIDIA STORE CA      → matched rule "NVIDIA" (Contains)        │
│     2026-04-09   Sapphire Reserve …4242    -$380.00     NVIDIA STORE CA                                                │
│     2026-04-02   Checking …4242            -$250.00     NVIDIA*GEFORCENOW                                              │
│     2026-03-28   Sapphire Reserve …4242    -$300.00     NVIDIA STORE CA                                                │
│     2026-03-15   Sapphire Reserve …4242    -$350.00     NVIDIA STORE CA                                                │
│     2026-03-04   Checking …4242            -$250.00     NVIDIA*GEFORCENOW                                              │
│     2026-02-20   Sapphire Reserve …4242    -$300.00     NVIDIA STORE CA                                                │
│     2026-02-05   Sapphire Reserve …4242    -$250.00     NVIDIA STORE CA                                                │
│                                                                                                                        │
│   ────────────────────────────────────────────────────────────────────────────────                                      │
│                                                            Subtotal  $2,500.00                                          │
│                                                                                                                        │
│   ▶ [Enter] on a row opens the transaction in TransactionsView for re-categorization.                                  │
│                                                                                                                        │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ [j/k] Select transaction  [Enter] Open in Transactions  [c] Mis-classify (report)  [Esc] Back                          │
╰────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

**Data sources.**
- `data_store_.transactions` iterated and `DiscoveryService::getSupplierInfo(tx.description)` filtered by ticker — same as `DashboardView::render` (`src/views/DashboardView.cpp:183-192`).
- The `getSupplierInfo` call exposes the matched rule (`src/services/DiscoveryService.h:90`) — already present.
- Re-categorization → `DataStore::update_transaction_category(transaction_id, category_id)` exists (`src/models/DataStore.h:42`).

**Backend.** Nothing new. Lift the existing inline aggregation into `DiscoveryService::aggregate_supplier_spend()` (shared with Drill 2).

**Esc.** Two pops back to Dashboard. `Esc` from supplier-transactions returns to the supplier list; `Esc` again returns to Dashboard with widget focused.

#### Drill 5 — Category Trends → Per-Category Transaction List

**What "drill into" means.** Five categories visible on the widget. Drill into the focused category to see every transaction in the current month plus the prior month, side-by-side; the MoM delta is broken down per-merchant.

**Detail view (120 × 40 ASCII mockup):**

```
╭────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╮
│ Dashboard > Category Trends > Food & Dining                                                            [Esc] Back       │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                                        │
│   Food & Dining  [food]   2026-04: $420.50   2026-03: $365.65   MoM: +15.0%                                            │
│                                                                                                                        │
│   This month  (2026-04)                              Last month  (2026-03)                                             │
│     Date       Merchant         Amount                 Date       Merchant         Amount                              │
│   ▸ 04-12      Starbucks         -$8.50                03-30      Trader Joe's    -$45.20                              │
│     04-11      Sweetgreen       -$14.25                03-28      Starbucks        -$8.50                              │
│     04-10      Trader Joe's     -$62.10                03-25      Sweetgreen      -$14.25                              │
│     04-08      Starbucks         -$8.50                03-22      Trader Joe's    -$50.10                              │
│     04-06      Sweetgreen       -$14.25                ...                                                             │
│     ...                                                                                                                │
│     Total                       -$420.50                Total                    -$365.65                              │
│                                                                                                                        │
│   Top contributors to the +15% delta                                                                                   │
│     Trader Joe's     +$22.00   (2 extra runs)                                                                          │
│     Restaurants      +$32.85                                                                                           │
│                                                                                                                        │
│   Budget for 2026-04   $400.00     spent $420.50    +$20.50 over                                                       │
│                                                                                                                        │
│                                                                                                                        │
│                                                                                                                        │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ [j/k] Select  [Enter] Open transaction  [/] Filter merchant  [b] Adjust budget  [Esc] Back                             │
╰────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

**Data sources.**
- `DataStore::transactions` filtered by `category_id` and `tx.date.substr(0,7)` — exact same logic as `sum_expense_by_category_in_month` (`src/views/DashboardView.cpp:67-80`). Lift to a helper.
- `DataStore::get_budget(category_id, month)` — exists (`src/models/DataStore.h:53`).

**Backend.** Nothing new.

**Esc.** Pops to Dashboard with `ui_category_trends` focused.

---

### 3c. Command palette

#### Activation key

**Recommendation: `:`** — colon. Justification:

1. Matches lazygit + helix + vim + k9s. The user's muscle memory transfers from every tool in the prior-art set.
2. `:` is unambiguous; no overlap with `c` (config), no risk of conflict with a future widget shortcut.
3. `Ctrl-K` (the VS Code option) collides with FTXUI's existing scroll behaviors on Windows terminals and adds a multi-key chord where one keystroke suffices.

If Rory prefers `Ctrl-K`, see §5 Q1.

#### Action vocabulary (v0.3 launch set)

Each entry shows the canonical command name, aliases (any of which match), and the effect.

| Command | Aliases | Effect |
| --- | --- | --- |
| `:quit` | `:q`, `:exit` | Save and exit. Same as `Q`. |
| `:save` | `:w` | Persist to backend. Same as `S`. |
| `:dashboard` | `:dash` | Switch to Dashboard view. |
| `:accounts` | `:acc` | Switch to Accounts view. |
| `:transactions` | `:tx` | Switch to Transactions view. |
| `:budget` | `:bud` | Switch to Budget view. |
| `:entity personal` | `:1` | Switch to Personal entity (also bound to `1`). |
| `:entity business` | `:2` | Switch to Business LLC entity. |
| `:plaid-link` | `:link` | Begin Plaid Link flow for the first un-linked account. |
| `:plaid-test` | `:link-test` | Sandbox link (current `L` key). |
| `:config` | `:cfg`, `:c` | Show config status banner (current `C` key). |
| `:refresh` | `:r`, `:sync` | Trigger backend sync. |
| `:filter <pattern>` | `:f <pattern>`, `/` | In a list view, filter rows by substring. `/` is the shortcut. |
| `:search <pattern>` | — | Global search across transactions. |
| `:help` | `:?`, `?` | Open help overlay. `?` is the shortcut. |
| `:logout` | — | Logout (clear session). |
| `:goto <view>` | — | Polymorphic. `:goto chase` jumps to Accounts view with Chase pre-filtered. |
| `:drill <widget>` | — | Drill into a Dashboard widget by name (`:drill net-worth`). |
| `:export csv <path>` | — | Export current view to CSV. (Plumbing exists in v0.2; this binds it.) |
| `:theme <name>` | — | Switch between two ship themes: `default` (current LED-blue) and `mono` (no chrome color; see §5 Q4). |

20 commands at launch. Adding a 21st is a single entry in `command_registry.cpp`.

#### Fuzzy-match library

**Recommendation:** **[`fts_fuzzy_match`](https://github.com/forrestthewoods/lib_fts) by Forrest Smith.** Single-header (≈600 LOC), permissive license, zero external deps, MIT. Drop into `src/utils/fuzzy_match.h`. The same algorithm Sublime Text uses for its command palette.

Why not alternatives:

- **fzf** is a binary, not a library. Not embeddable in a C++ process.
- **nucleo (Rust)** has C bindings but pulls Rust toolchain.
- **rolling our own Levenshtein** is straightforward but adds maintenance.

The match function ranks entries by a score combining: (1) first-char match, (2) consecutive-character bonus, (3) camel/word boundary bonus, (4) penalty per unmatched character. Sublime/Helix-quality results in 600 lines.

#### Mockup of palette open over current view

The palette renders as a bottom-aligned overlay that does not destroy the underlying view. FTXUI primitive: `dbox({background_view, palette_overlay})` with the palette an `hbox({...}) | center | borderHeavy` pinned to the bottom 8 rows.

```
╭────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╮
│ [x] 1:Personal  [ ] 2:Business LLC                                                                                     │
│ [x] Dashboard  [ ] Accounts  [ ] Transactions  [ ] Budget                                                              │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│  Net Worth                  Shovel Score                Sync Status                                                    │
│  $13,234.56                 72/100                      [+] Chase  synced 2026-04-01                                   │
│  Checking:    $1,234.56     EARLY ADOPTER               [!] BofA   auth failed                                         │
│  Savings:     $2,500.00     Total: $5,890.00            [+] Fidelity synced 2026-03-28                                 │
│  Credit:        -$500.00    Shovel co's: 4                                                                             │
│  Investment: $10,000.00                                                                                                │
│                                                                                                                        │
│  Shovel Intelligence                                Category Trends                                                    │
│  NVDA   $2,500.00  +120% MoM   8 tx                 [food] Food & Dining   $420.50  +15.0%                             │
│  AMZN   $1,800.00   +15% MoM  12 tx                 [trsp] Transportation $180.00   -25.0%                             │
│  MSFT     $950.00   -10% MoM   4 tx                 [shop] Shopping       $600.00     0.0%                             │
│  GOOGL    $640.00     0% MoM   3 tx                                                                                    │
│                                                                                                                        │
│             ╭──────────────────────────────────────────────────────────────────────────────────────────╮               │
│             │  :tx│                                                                                    │               │
│             │   ▸  transactions          jump to Transactions view                                     │               │
│             │      tx-list-export        export Transactions to CSV                                    │               │
│             │      tx-search             search transactions by description                            │               │
│             │      drill net-worth       drill into Net Worth widget                                   │               │
│             ╰──────────────────────────────────────────────────────────────────────────────────────────╯               │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ [j/k] Select  [Enter] Execute  [Esc] Cancel  [Tab] Complete                                                            │
╰────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

Interaction:

- `:` from anywhere opens the palette. Focus is captured.
- Typing filters the visible action list via fuzzy match. Top match is pre-selected.
- `j`/`k` (or arrows) move the highlight.
- `Tab` completes the typed text to the highlighted entry (so `:tx`+`Tab` becomes `:transactions`).
- `Enter` executes; the palette closes.
- `Esc` cancels; the palette closes; the prior focus is restored.

The palette's history persists across the session (in `App::command_history`, `std::vector<std::string>`); pressing `↑` in an empty palette recalls the previous command. Bash-style.

---

### 3d. Status bar redesign

The current bar (`src/main.cpp:235`) is one flat line of every global hotkey. v0.3 makes it **contextual + global**: a two-row footer where the top row reflects the *focused thing*, the bottom row keeps a stable global set.

#### Layout

```
[ top row    ] <- contextual: changes based on focus / mode
[ bottom row ] <- global: stable across the whole app
```

#### Example A — Top-level (no widget focused, on Dashboard)

```
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Dashboard — 5 widgets ready          [Tab] Focus first widget  [/] Search  [:] Command palette  [?] Help               │
│ [1-2] Entity  [Tab-cycle] View  [P] Plaid link  [L] Test link  [r] Sync  [s] Save  [q] Quit                            │
╰────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

#### Example B — Widget focused (Shovel Intelligence)

```
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Shovel Intelligence focused          [Enter] Drill in  [r] Refresh map  [/] Filter  [Esc] Unfocus                      │
│ [h/j/k/l] Move focus  [1-2] Entity  [Tab] Next view  [:] Command  [?] Help  [q] Quit                                   │
╰────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

#### Example C — Drilled in (Net Worth drill, account row focused)

```
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Net Worth > Sapphire Reserve …4242    [Enter] Open transactions  [r] Refresh  [/] Filter  [Esc] Back                   │
│ [j/k] Select row  [g/G] Top/Bottom  [:] Command  [?] Help  [q] Quit                                                    │
╰────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

#### Implementation

A new `StatusBar` class in `src/views/StatusBar.{h,cpp}` takes the `FocusController` and a per-context "hint list" registered by each widget/view. Widgets implement a virtual `std::vector<KeyHint> hints_when_focused() const` method; views implement `std::vector<KeyHint> hints_top_level() const`. The StatusBar joins them with the global set into the two-row render.

```cpp
struct KeyHint {
  std::string key;        // "Enter"
  std::string description; // "Drill in"
};
```

The same `KeyHint` registry powers the `?` help overlay so they cannot drift apart.

---

### 3e. Color palette (semantic)

The current `LED_BLUE` palette in `src/views/ViewCommon.h:10-11` becomes one entry in a wider table. **Every color has exactly one meaning.** Red is reclaimed for "below zero, full stop"; "MoM increasing" goes to amber (warning) or neutral (no opinion), as the widget chooses.

#### Token table

| Token | FTXUI value | Meaning | When used |
| --- | --- | --- | --- |
| `fg_default` | `Color::Default` | Body text | Most rows, list items |
| `fg_dim` | `Color::GrayDark` (or `Color::Default` + `\| dim`) | Secondary text, labels | "Checking:", "Savings:", breadcrumbs |
| `fg_emphasized` | `Color::White` + `bold` | Headline numbers, focused-widget titles | Net Worth big number, focused widget title |
| `chrome` | `LED_BLUE` `Color(39, 170, 255)` | Branding chrome, view-tab strip | Entity tabs, view tabs, borders of non-focused widgets |
| `chrome_dim` | `LED_BLUE_DIM` `Color(20, 85, 135)` | Inactive chrome | Non-active entity/view tabs |
| `accent_positive` | `Color::Green` | **Magnitude is positive (value ≥ 0)** | Net Worth ≥ 0, account balance ≥ 0, surplus in Budget |
| `accent_negative` | `Color::Red` | **Magnitude is negative (value < 0) OR an error state** | Negative balance, credit row when balance < 0, sync error |
| `accent_warning` | `Color::Yellow` | **Needs attention** | Over-budget rows, stale sync (>72h), focus border |
| `accent_info` | `Color::Cyan` | Neutral metadata, timestamps, breadcrumbs | "Last sync: 2026-04-01", breadcrumb segments |
| `focus` | `Color::Yellow` + `bold` | **The thing currently focused** | Widget border on focused widget, row-selection arrow |
| `accent_thesis_up` | `Color::Magenta` | **Shovel-thesis: this number going up is *interesting*, not bad** | `^ +120% MoM` on `ui_shovel_intelligence`. Reserved for the discovery widgets; not used elsewhere. |

#### Rules

1. **One meaning per color.** Red means below zero or error. It does *not* mean "MoM up". Today's `ui_category_trends.cpp:44-46` (red for `percent_change > 0`) is defensible because spending going up is uniformly bad; we keep it. `ui_shovel_intelligence.cpp` is the opposite — Shovel spend going up is *the thesis* — and gets `accent_thesis_up` (magenta) for the `^` arrow.
2. **Focus is yellow.** Nothing else is yellow. No widget uses yellow for data. This is the strongest signal in the UI.
3. **Bold is for headline values only.** Subordinate labels and row text use dim/default. The current pattern of `bold` on every value (`src/views/widgets/ui_net_worth.cpp:34`, `:43`) over-uses it; v0.3 bolds only the top-line metric per widget.
4. **Don't decorate; differentiate.** Color tokens exist where the difference matters. Pure decoration (e.g., the existing `LED_BLUE` border on every widget) is fine because it signals "this is a TerminalFinance widget" — branding chrome.

#### Migration

`src/views/ViewCommon.h` gains a `ColorTokens` struct:

```cpp
struct ColorTokens {
  Color fg_default      = Color::Default;
  Color fg_emphasized   = Color::White;
  Color chrome          = Color(39, 170, 255);
  Color chrome_dim      = Color(20, 85, 135);
  Color accent_positive = Color::Green;
  Color accent_negative = Color::Red;
  Color accent_warning  = Color::Yellow;
  Color accent_info     = Color::Cyan;
  Color focus           = Color::Yellow;
  Color thesis_up       = Color::Magenta;
};
inline const ColorTokens kTokens;
```

Widget code stops referring to raw `Color::Red` / `Color::Green` and reads from `kTokens`. The single-file change is mechanical (search/replace). Adding a `kMonoTokens` later flips the theme palette.

---

### 3f. Keybinding map

Complete keybinding table for v0.3. Every key the binary responds to. Read top-to-bottom.

**Global (work in every context unless overridden):**

| Key | Action |
| --- | --- |
| `q` / `Q` | Save and quit (existing). |
| `s` / `S` | Save without quit (existing). |
| `:` | Open command palette. |
| `?` | Open help overlay. |
| `/` | In list view: filter rows. Elsewhere: open command palette pre-typed with `:search `. |
| `Ctrl-C` | Interrupt (matches POSIX expectation; current `Esc` does this). |

**Top-level / Dashboard level:**

| Key | Action |
| --- | --- |
| `Tab` (on Dashboard, no widget focused) | Focus first widget. |
| `Tab` (on Dashboard, widget focused) | Focus next widget in reading order. |
| `Tab` (in non-Dashboard view) | Cycle to next view. |
| `Shift-Tab` | Inverse of `Tab`. |
| `1` / `2` | Switch entity (existing). |
| `Esc` | Unfocus / dismiss modal / pop drill (level-dependent). |

**Widget-focused (on Dashboard):**

| Key | Action |
| --- | --- |
| `h` / `←` | Focus widget to the left. |
| `l` / `→` | Focus widget to the right. |
| `j` / `↓` | Focus widget below. |
| `k` / `↑` | Focus widget above. |
| `Enter` | Drill into focused widget. |
| `r` | Widget-specific refresh (see per-widget table below). |
| `Esc` | Unfocus → Dashboard level. |

**List views (Accounts / Transactions / Budget):**

| Key | Action |
| --- | --- |
| `j` / `↓` | Next row. |
| `k` / `↑` | Previous row. |
| `g g` | First row. |
| `G` | Last row. |
| `Enter` | Open row detail (per-view; existing partial behavior). |
| `/` | Filter rows by pattern. |
| `Esc` | Clear filter, else return to Dashboard. |

**Drill views (universal):**

| Key | Action |
| --- | --- |
| `j` / `k` | Move selection inside drill. |
| `Enter` | Drill one level deeper if defined. |
| `Esc` | Pop one drill level. |
| `/` | Filter the drill's primary list. |
| `r` | Refresh data (re-query backend). |

**Widget-specific actions when focused or drilled:**

| Widget | Focused-Action key | Effect |
| --- | --- | --- |
| `ui_net_worth` | `r` | Recompute net worth from DataStore. |
| `ui_shovel_score` | `r` | Recompute via `DiscoveryService::aggregate_supplier_spend`. |
| `ui_sync_status` | `r` | Trigger `BackendClient::refresh()` (Phase 4 plumbing). |
| `ui_sync_status` | `R` | Begin re-auth flow for selected institution (Plaid Link). |
| `ui_shovel_intelligence` | `r` | Re-run discovery from supplier_map.json. |
| `ui_category_trends` | `r` | Recompute MoM. |
| `ui_category_trends` (drill) | `b` | Open budget editor for the category. |

**Command palette and help overlay:**

| Key | Action |
| --- | --- |
| `Enter` | Execute selected entry. |
| `Tab` | Complete typed text to selected entry. |
| `j`/`k` / `↑`/`↓` | Move selection. |
| `↑` (empty palette) | Recall previous command. |
| `Esc` | Cancel palette. |

**Removed / repurposed from v0.2:**

| v0.2 key | v0.3 disposition |
| --- | --- |
| `P` | **Removed as a hidden hotkey.** Becomes `:plaid-link` (no top-level binding). Reasoning: it's rare. The status bar is reclaimed for high-frequency keys. |
| `L` | **Removed as a hidden hotkey.** Becomes `:plaid-test`. |
| `C` | **Removed.** Replaced by `:config`. |

The shed of `P`/`L`/`C` is intentional: these are once-per-setup operations. The status bar now teaches the *focused* widget's hotkeys instead.

---

## 4. v0.3 implementation phase plan

Five executor tasks. Each is sized to one dispatch (≈ 200–500 LOC). Dependencies are linear except where noted.

### Task v0.3-1 — FocusController + visual focus

**Lead:** tui-engineer.

**Scope:**

- **Files created:** `src/views/FocusController.h`, `src/views/FocusController.cpp`, `tests/test_focus_controller.cpp`.
- **Files modified:** `src/main.cpp` (own a `FocusController` member on `App`; route key events through it before the existing handlers), `src/views/DashboardView.cpp` (accept a `const FocusController&` parameter in `render()`; decorate each panel based on which widget is focused), `src/views/widgets/ui_*.{h,cpp}` × 5 (each `Renderer` function accepts a `bool focused` parameter; toggles border style / title brightness; no logic changes), `src/views/ViewCommon.h` (add `ColorTokens` struct).
- **No new dependencies.**

**Approx LOC:** ~450 net new (FocusController ~180; widget signature updates ~50; ColorTokens ~30; DashboardView wiring ~70; main.cpp event routing ~80; tests ~40).

**Test strategy:**
- Unit tests on FocusController state transitions: 12 cases covering Tab cycle, hjkl movement, Esc pop, modal entry/exit. No FTXUI involved.
- Update existing snapshot fixtures in `tests/snapshot/fixtures/*.txt`: each widget gets a *new* paired fixture `<widget>_focused.txt` showing the yellow border + bright title. The existing unfocused fixtures keep their current content (this is the proof of no regression).
- One end-to-end snapshot test of Dashboard with `ui_net_worth` focused — proves the integration.

**Order dependency:** Foundation. Everything else depends on this.

---

### Task v0.3-2 — Drill views for net_worth + shovel_score

**Lead:** tui-engineer.

**Scope:**

- **Files created:** `src/views/drills/Drill_NetWorth.{h,cpp}`, `src/views/drills/Drill_ShovelScore.{h,cpp}`, `tests/snapshot/fixtures/drill_net_worth.txt`, `tests/snapshot/fixtures/drill_shovel_score.txt`.
- **Files modified:** `src/services/DiscoveryService.{h,cpp}` (add `aggregate_supplier_spend(transactions, current_month, prev_month) → std::vector<SupplierSpend>` — extract the inline aggregation from `DashboardView.cpp:183-211`), `src/views/DashboardView.cpp` (replace inline aggregation with the new service call; no behavior change), `src/main.cpp` (route `Enter` from a focused widget to `App::drill_into(WidgetId)`).
- **No new dependencies.**

**Approx LOC:** ~400 net new (two drill views ~140 each; DiscoveryService refactor ~50; main routing ~30; tests ~40).

**Test strategy:**
- Snapshot tests for both drill views at 120 × 40.
- Unit test for `aggregate_supplier_spend` against a fixture transaction set — proves the extracted function matches the inline behavior byte-for-byte.

**Order dependency:** Depends on v0.3-1.

---

### Task v0.3-3 — Drill views for sync_status + shovel_intelligence + category_trends

**Lead:** tui-engineer.

**Scope:**

- **Files created:** `src/views/drills/Drill_SyncStatus.{h,cpp}`, `src/views/drills/Drill_ShovelIntelligence.{h,cpp}`, `src/views/drills/Drill_CategoryTrends.{h,cpp}`, paired snapshot fixtures.
- **Files modified:** `src/main.cpp` (`Enter`/`Esc` routing for these three widgets); `src/services/BackendClient.{h,cpp}` (add `get_sync_status()` returning per-Item state — see v0.3-3 backend dependency).

**Approx LOC:** ~550 net new (three drill views ~150 each; BackendClient method ~50; tests ~50).

**Backend dependency:**

- `BackendClient::get_sync_status()` is **[backend method needed]**. Server-side route `GET /sync-status` returns `[{institution, item_id, last_success_unix, last_attempt_unix, last_error_code, accounts: [account_id]}, ...]`. This is purely aggregation over existing server-side data (audit log + accounts table per V0_2_PLAN.md §f).
- If Phase 4 of v0.2 has not landed the server-side sync orchestration, v0.3-3's Drill_SyncStatus stub renders the DataStore-derived view it does today (same data as the existing widget, just full-screen) and gates the `R` re-auth key behind a feature flag.

**Test strategy:**
- Three snapshot tests (one per drill).
- Mock `BackendClient::get_sync_status()` for the sync_status drill test.
- The supplier-transactions drill (Shovel Intelligence level 2) uses an inline fixture from `SyntheticGenerator`.

**Order dependency:** Depends on v0.3-2. Can run in parallel with v0.3-4 if separate engineer.

---

### Task v0.3-4 — Command palette + fuzzy match + help overlay

**Lead:** tui-engineer (separate engineer from drill work if dispatched in parallel).

**Scope:**

- **Files created:** `src/utils/fuzzy_match.h` (single-header fts_fuzzy_match drop-in), `src/views/CommandPalette.{h,cpp}`, `src/views/HelpOverlay.{h,cpp}`, `src/utils/CommandRegistry.{h,cpp}` (the 20 commands as data), `tests/test_command_palette.cpp`, `tests/test_fuzzy_match.cpp`, `tests/snapshot/fixtures/command_palette_open.txt`, `tests/snapshot/fixtures/help_overlay.txt`.
- **Files modified:** `src/main.cpp` (key events `:` and `?` enter modal; events route through the modal first; `App` owns a `CommandPalette` and a `HelpOverlay`), `src/views/StatusBar.{h,cpp}` (new file — replaces the inline status bar in `main.cpp:235`).

**Approx LOC:** ~700 net new (fuzzy_match.h ~620 vendored; CommandPalette ~120; HelpOverlay ~80; CommandRegistry ~100; StatusBar ~80; tests ~80; main.cpp wiring ~30). The vendored fuzzy_match.h is third-party so doesn't count against our LOC for review purposes, but it is included in the diff for offline reproducibility.

**Test strategy:**
- Unit tests for fuzzy match against the canonical fts test cases.
- Unit test for `CommandRegistry::lookup(":tx", &out)` returning the right ordered candidates.
- Snapshot tests of palette open / help overlay open. These test the overlay's layout, not the underlying view.

**Order dependency:** Independent of v0.3-2 / v0.3-3 (no shared files). Depends on v0.3-1's FocusController for "Esc closes the modal first".

---

### Task v0.3-5 — Color token migration + status bar contextual hints

**Lead:** tui-engineer.

**Scope:**

- **Files modified:** Every widget (`src/views/widgets/ui_*.cpp` × 5) — replace literal `Color::Red`/`Color::Green` with `kTokens.*`; `src/views/widgets/ui_shovel_intelligence.cpp` — change `^` arrow color from `Color::Red` to `kTokens.thesis_up` (magenta); `src/views/StatusBar.{h,cpp}` — gain `set_focus_hints(widget_id, std::vector<KeyHint>)` and renders two-row layout; each widget gains a `hints_when_focused()` method returning its KeyHint vector.
- **Files updated:** All snapshot fixtures in `tests/snapshot/fixtures/*.txt` — color changes show up in the ANSI codes embedded in the fixtures, so each fixture needs `--update-snapshots` and human review.

**Approx LOC:** ~250 net new (palette migration ~80; StatusBar hints API ~100; widget hint methods ~50; tests ~20). Plus mechanical fixture updates.

**Test strategy:**
- All existing snapshot tests pass with the regenerated fixtures.
- A pinned manual review step: a human looks at each new fixture and confirms the visual change is intentional (no silent regression).

**Order dependency:** Depends on v0.3-1 (ColorTokens struct must exist). Can run after v0.3-4 to share the StatusBar work.

---

### Aggregate

Five tasks, ~2,350 LOC total (excluding the vendored fuzzy_match.h). Snapshot test count grows from 8 to ~16 (5 widget-focused + 5 drill + 2 modal + existing 8 - 4 retired = roughly 16). No new third-party dependencies — fuzzy_match is vendored as a single header.

Dependency DAG:

```
v0.3-1 (Focus + visual) ─┬─> v0.3-2 (drills A) ─> v0.3-3 (drills B)
                         ├─> v0.3-4 (palette + help)
                         └─> v0.3-5 (colors + status bar)
```

v0.3-2 / v0.3-3 / v0.3-4 / v0.3-5 can fan out after v0.3-1 lands.

---

## 5. Open questions

These gate dispatch. Tentative recommendations in **bold**; tradeoffs explicit.

### Q1 — Command palette activation key: `:` vs `Ctrl-K`

**Recommendation: `:`.**

Tradeoff:
- `:` matches lazygit, vim, helix, k9s. Single keystroke. No modifier.
- `Ctrl-K` matches VS Code, Slack, modern web apps. Two keystrokes. Discoverable to users coming from GUI apps.
- The TerminalFinance user base today is two operators who already use vim-derived tools. v0.4 might add a non-technical user (Cade) for whom `Ctrl-K` would be more discoverable.

**Why I'd still pick `:`.** The status bar always shows `[:] Command` — that's the discoverability. A new user reads the bar and learns the key in 2 seconds. Meanwhile, `:` saves a keystroke a hundred times a day for the daily user.

### Q2 — Focus indicator color: `Yellow` vs reusing `LED_BLUE`

**Recommendation: `Color::Yellow` for focus.**

Tradeoff:
- Yellow is universally readable on every terminal background and never collides with semantic colors (no widget value is yellow).
- Reusing `LED_BLUE` keeps the visual identity tighter; the existing palette already feels coherent. But then "focused" and "non-focused" only differ in border *intensity*, which is a subtle signal.
- A third option: `Color::White` `+ bold` border. Maximum contrast, no color at all. Loses some visual gravity.

**Why Yellow.** It's the one color in the FTXUI default set that nothing else in the app uses. Forces interpretation: "yellow means the cursor is here."

### Q3 — `Esc` at Dashboard level (no widget focused): no-op vs quit-confirm vs back-to-login

**Recommendation: no-op.**

Tradeoff:
- **No-op** is safe; the user has to consciously press `Q` to quit.
- **Quit-confirm** ("Press Esc again to quit") is the GUI convention but adds a state machine and a banner.
- **Back-to-login** is too aggressive; v0.2 doesn't have a "logged-in" state visible from the TUI, only via `--whoami` (`src/main.cpp:467`).

**Why no-op.** Esc already overloads heavily (unfocus, pop drill, close modal). Adding a fourth meaning is asking for misclicks.

### Q4 — Theme variants: ship `default` + `mono` or only `default`?

**Recommendation: ship both.**

Tradeoff:
- The `mono` theme (no LED_BLUE chrome; just bold/dim) is useful for screen-sharing, low-contrast terminals, and recording demos.
- Adds one struct + one switch in `ViewCommon.h` — trivial cost.
- Risk: regression surface doubles. Every snapshot test runs against the default theme; mono regressions could ship silently.

**Mitigation.** Snapshot harness gains a `--theme=mono` flag, runs the same fixtures, produces a second set of fixtures under `tests/snapshot/fixtures/mono/`. CI runs both. Owner: v0.3-5.

### Q5 — Drill-view back-stack depth limit

When the user drills from Shovel Intelligence → supplier → transaction → (Enter would open Transactions view filtered to that account), how deep does Esc need to remember?

**Recommendation: depth 4 max; deeper drills reset the stack.**

Tradeoff:
- A back-stack of arbitrary depth lets the user wander indefinitely but eats memory and risks state desync (what if the underlying data changes mid-drill?).
- Depth 4 covers every drill flow described in §3b (Dashboard → widget → row → next-row). Anything deeper means "go to a full view"; the breadcrumb gives the user a quick reference and the view becomes the new home.

**Why 4.** Matches the maximum useful breadcrumb width on a 120-column terminal without truncating segment names. Deeper would force ellipsis.

### Q6 — `?` vs `:help`: which is canonical?

**Recommendation: both work; `?` is the marketing surface.**

Tradeoff:
- `?` is universal (man, less, vim, lazygit). Single key.
- `:help` is the command-palette path; needed anyway for the registry to know about help.
- Documenting both is fine: the status bar advertises `?`; users who learn `:` discover `:help` naturally.

**No real choice to make.** Implement both; the status bar shows `?`.

---

## Appendix A — file & service inventory (current state, cited for v0.3 work)

For the v0.3 implementer. Every file v0.3 touches, with current paths.

**Widgets (existing, will gain `bool focused` param):**
- `src/views/widgets/ui_net_worth.{h,cpp}`
- `src/views/widgets/ui_shovel_score.{h,cpp}`
- `src/views/widgets/ui_sync_status.{h,cpp}`
- `src/views/widgets/ui_shovel_intelligence.{h,cpp}`
- `src/views/widgets/ui_category_trends.{h,cpp}`
- `src/views/widgets/ui_updates.{h,cpp}` (not currently in Dashboard composition; v0.3 leaves it alone)
- `src/views/widgets/consolidation_ui.{h,cpp}` (admin view; v0.3 leaves it alone)

**Composition (existing, will be updated for focus + drill routing):**
- `src/views/DashboardView.{h,cpp}` — composes the five Dashboard panels (`src/views/DashboardView.cpp:264`).
- `src/views/AccountsView.h`, `src/views/TransactionsView.h`, `src/views/BudgetView.h` — non-Dashboard views; gain `j`/`k` aliases for arrow keys, otherwise unchanged.
- `src/views/ViewCommon.h` — gains `ColorTokens` struct.

**App shell (existing, will gain FocusController + modal routing):**
- `src/main.cpp` — App class, event loop, status-bar inline render. Key sites: line 49 (App struct), line 191 (render), line 235 (status bar), line 733 (CatchEvent), line 752 (Tab), line 762 (ArrowUp).

**Backend / data (read-only references from v0.3):**
- `src/models/DataStore.{h,cpp}` — all drill data sources.
- `src/services/DiscoveryService.{h,cpp}` — gains `aggregate_supplier_spend()` in v0.3-2.
- `src/services/BackendClient.{h,cpp}` — gains `get_sync_status()` in v0.3-3 (or stub-falls-back if Phase 4 of v0.2 hasn't landed it).

**Snapshot harness (existing; v0.3 extends):**
- `tests/snapshot/snapshot_helper.h`
- `tests/snapshot/fixtures/*.txt` — 8 fixtures today; ~16 after v0.3.
- `tests/test_widgets_snapshot.cpp` — extend to test focused variants and drill views.

**New files in v0.3:**
- `src/views/FocusController.{h,cpp}`
- `src/views/CommandPalette.{h,cpp}`
- `src/views/HelpOverlay.{h,cpp}`
- `src/views/StatusBar.{h,cpp}`
- `src/views/drills/Drill_NetWorth.{h,cpp}`
- `src/views/drills/Drill_ShovelScore.{h,cpp}`
- `src/views/drills/Drill_SyncStatus.{h,cpp}`
- `src/views/drills/Drill_ShovelIntelligence.{h,cpp}`
- `src/views/drills/Drill_CategoryTrends.{h,cpp}`
- `src/utils/fuzzy_match.h` (vendored single-header)
- `src/utils/CommandRegistry.{h,cpp}`
- `tests/test_focus_controller.cpp`
- `tests/test_command_palette.cpp`
- `tests/test_fuzzy_match.cpp`

---

## Appendix B — definitions of done for v0.3

The orchestrator considers v0.3 shipped when all of these are true:

1. `Tab` from Dashboard focuses a widget; the widget's border turns yellow; the status bar's top row reflects that widget's hotkeys.
2. `Enter` on any of the five focused widgets renders a drill view per §3b. `Esc` returns to Dashboard with the widget still focused.
3. `:` opens a command palette. Typing `:tx` ranks "transactions" first. Enter executes. Esc cancels.
4. `?` opens a help overlay listing every key in the current context. The overlay's content is generated from the same `KeyHint` registry the status bar reads — they cannot diverge.
5. Red appears in the TUI in exactly two places: (a) values below zero, (b) error states (sync failure). Spending-going-up uses red only in `ui_category_trends`; supplier-spending-going-up in `ui_shovel_intelligence` uses magenta.
6. Snapshot tests pass on both Windows and macOS for: 5 widgets focused + 5 widgets unfocused + 5 drill views + palette open + help open + Dashboard top-level. ~17 fixtures, all green.
7. The hjkl-vs-arrows mapping is symmetric: every flow described in §3f works with both.
8. The `P` / `L` / `C` keys are gone from the status bar. `:plaid-link`, `:plaid-test`, `:config` work from the palette.

When all eight are true, v0.3 is the launching pad. Today's static dashboard becomes a thing the user *operates*.

---

## Appendix C — FTXUI integration patterns

FTXUI 5.0.0 has the primitives we need. The v0.3 implementer should not invent abstractions where FTXUI already provides them. This appendix sketches the integration points.

### C.1 Focus visualization

FTXUI provides `border`, `borderLight`, `borderHeavy`, `borderRounded`, `borderDouble`, and `borderStyled(BorderStyle)` decorators. Color comes from chaining `| color(...)` on the bordered element. The pattern v0.3 uses:

```cpp
// src/views/widgets/ui_net_worth.cpp — current (v0.2):
return vbox(std::move(rows)) | border;

// src/views/widgets/ui_net_worth.cpp — v0.3:
Element panel = vbox(std::move(rows));
if (focused) {
    panel = panel | borderStyled(ROUNDED) | color(kTokens.focus);
    // Title gets bright treatment.
    // (Title is the first row of `rows`; we re-decorate it in render
    //  before the vbox is composed.)
} else {
    panel = panel | borderStyled(LIGHT) | color(kTokens.chrome_dim);
}
return panel;
```

The `bool focused` parameter threads through every `*Renderer` function in `src/views/widgets/*.cpp`. The DashboardView reads `FocusController::focused_widget()` and passes `true` for the matching widget, `false` for the rest. No widget knows what *other* widget might be focused.

### C.2 Modal overlay (palette + help)

FTXUI's `dbox` stacks elements: the last one in the vector renders on top. The pattern:

```cpp
// In App::render(), when a modal is active:
Element background = render_normal_app();  // computes Dashboard / view as today
Element palette    = render_command_palette();  // overlays
return dbox({
    background,
    palette,  // bottom-aligned, drawn over background
});
```

When no modal is open, just return `background`. The `dbox` approach means the background view is still computed (and visible faintly behind the palette in case of partial-width overlays), preserving the visual context. The palette element pins itself to the bottom 8 rows with `vbox({filler() | flex, palette_inner})`.

The `App` struct gains:

```cpp
class App {
  ...
  std::optional<CommandPalette> command_palette_;  // present when : is open
  std::optional<HelpOverlay> help_overlay_;        // present when ? is open

  bool modal_active() const {
    return command_palette_.has_value() || help_overlay_.has_value();
  }
};
```

The event loop routes events to the topmost active modal first; only if the modal does not consume the event does it fall through to view-level handlers.

### C.3 Event routing precedence

The current `CatchEvent` chain in `src/main.cpp:733-819` is a linear if-else cascade. v0.3 makes the precedence explicit:

```cpp
component = CatchEvent(component, [&](Event event) {
    // 1. Modal layer (top priority): if a palette or help overlay is open,
    //    it sees the event first and can consume Esc / character keys.
    if (app.modal_active()) {
        return app.handle_modal_event(event);
    }

    // 2. Focus controller: hjkl, Tab, Enter, Esc against the current focus
    //    level. Returns true if the event moved focus or drilled.
    if (app.focus.handle_event(event)) {
        return true;
    }

    // 3. Global hotkeys: Q, S, : (open palette), ? (open help), entity 1/2.
    if (handle_global_event(event)) {
        return true;
    }

    // 4. View-specific: AccountsView / TransactionsView arrow handling, etc.
    return app.handle_view_event(event);
});
```

This is a refactor, not a rewrite. The existing handler bodies move into the corresponding `handle_*` methods. The control flow becomes legible at the top of the function.

### C.4 Breadcrumb rendering

The breadcrumb is a row of `text()` elements joined by `text(" > ") | color(kTokens.fg_dim)`:

```cpp
Element render_breadcrumb(const std::vector<std::string>& segments) {
    Elements parts;
    for (size_t i = 0; i < segments.size(); ++i) {
        const bool last = (i + 1 == segments.size());
        Element seg = text(segments[i]) | (last ? bold : dim);
        if (last) seg = seg | color(kTokens.fg_emphasized);
        parts.push_back(seg);
        if (!last) parts.push_back(text(" > ") | color(kTokens.fg_dim));
    }
    return hbox(std::move(parts));
}
```

Each Drill view holds a `std::vector<std::string>` of segments. `Esc` pops the last segment; if the result is empty, the drill closes.

### C.5 Snapshot determinism

The existing harness in `tests/snapshot/snapshot_helper.h` renders a view at a fixed `Screen(120, 40)`. The v0.3 work must keep this contract. Two new concerns:

1. **Focused snapshots.** Each widget gets a paired fixture `<widget>_focused.txt` showing the yellow-bordered variant. The snapshot helper gains a small extension:

   ```cpp
   // tests/snapshot/snapshot_helper.h — proposed v0.3 addition
   std::string render_widget_with_focus(
       std::function<Element(bool /*focused*/)> widget_renderer,
       bool focused
   );
   ```

   The rest of the snapshot harness stays identical.

2. **Drill snapshots.** A new harness `render_drill(...)` that takes a Drill view and a `DataStore` fixture, renders it at 120 × 40, and emits a fixture. Six new fixtures (one per drill, plus a second level for the supplier-transactions drill).

### C.6 Performance

FTXUI re-renders the whole tree every event. Five widgets at 120 × 40 is small enough that this is invisible. The drill views are also single-screen. We add zero network/IO to the render path; every drill reads from `DataStore` (already in memory). The command palette's fuzzy match runs over ~20 entries on every keystroke — single-digit microseconds.

The only real performance hazard is the supplier aggregation that today runs every render (`src/views/DashboardView.cpp:183-211` iterates every transaction). v0.3-2 extracts this into `DiscoveryService::aggregate_supplier_spend()` and the implementer should add memoization keyed on `transactions.size() + last_modified_unix` so repeated Dashboard renders skip recomputation. This is a side-quest, not blocking.

---

## Appendix D — sample wire-up of a single drill (Net Worth)

Sketch only. Real code lands in v0.3-2.

```cpp
// src/views/drills/Drill_NetWorth.h
#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

class DataStore;

class Drill_NetWorth {
public:
    Drill_NetWorth(DataStore& data, std::string entity_id);

    // Render the drill at 120 × 40.  current_month is "YYYY-MM".
    ftxui::Element render(const std::string& current_month) const;

    // Returns a breadcrumb prefix; the App appends "> Net Worth".
    std::vector<std::string> breadcrumb_segments() const;

    // Movement.
    void select_next();
    void select_prev();

    // Drill one level deeper: returns the account_id under selection,
    // or nullopt if the selection is invalid.  The App routes that into
    // TransactionsView::set_account_id().
    std::optional<std::string> selected_account_id() const;

private:
    DataStore& data_;
    std::string entity_id_;
    int selected_ = 0;
};
```

```cpp
// src/views/drills/Drill_NetWorth.cpp — abbreviated
ftxui::Element Drill_NetWorth::render(const std::string& current_month) const {
    using namespace ftxui;

    auto accounts = entity_id_.empty()
        ? std::vector<Account*>{}  // (collect all via data_.accounts loop)
        : data_.get_accounts_for_entity(entity_id_);

    if (entity_id_.empty()) {
        for (auto& acc : data_.accounts) accounts.push_back(&acc);
    }

    Elements rows;
    rows.push_back(render_header(/* "Net Worth (Personal)", "$13,234.56" */));
    rows.push_back(separator());

    int idx = 0;
    double sum = 0.0;
    for (auto* acc : accounts) {
        const bool focused_row = (idx == selected_);
        Element row = hbox({
            text(focused_row ? "▸ " : "  "),
            text(std::to_string(idx + 1) + "   "),
            text(acc->name + std::string(20 - acc->name.size(), ' ')),
            text(to_type_str(acc->type) + "  "),
            DecorateAmount(acc->balance) | flex,
            text("  " + acc->institution),
        });
        if (focused_row) {
            row = row | color(kTokens.focus) | bold;
        }
        rows.push_back(row);
        sum += acc->balance;
        idx++;
    }

    rows.push_back(separator());
    rows.push_back(hbox({
        text("Sum                                              "),
        DecorateAmount(sum) | bold,
    }));

    // … breakdown by type, status bar, etc.
    return vbox(std::move(rows)) | border;
}
```

The drill is a regular FTXUI Element. It does not need its own component or event loop — the App's CatchEvent routes `j`/`k`/`Enter`/`Esc` into `select_next()`/`select_prev()`/`selected_account_id()`/pop-drill respectively.

---

## Appendix E — interaction transcripts (v0.3 in motion)

Three example sessions that demonstrate the model end-to-end. These are not tests; they're calibration for what the executor should make feel natural.

### Transcript 1 — "Why is my net worth this number?"

```
User opens TerminalFinance.
  → Dashboard renders. No widget focused. Status bar shows global keys.

User presses Tab.
  → ui_net_worth gets a yellow border. Status bar top row updates to:
      "Net Worth focused   [Enter] Drill   [r] Refresh   [Esc] Unfocus"

User presses Enter.
  → Drill_NetWorth renders full-screen. First account row selected.
    Breadcrumb: "Dashboard > Net Worth".

User presses j four times.
  → Selection moves down through the four account rows.

User presses Enter on "Sapphire Reserve …4242".
  → TransactionsView renders, filtered to that account.
    Breadcrumb: "Dashboard > Net Worth > Sapphire Reserve …4242".

User presses /trader.
  → Transactions filter to descriptions containing "trader".

User presses Esc three times.
  → 1st Esc: clear filter (back to all transactions for the account).
  → 2nd Esc: pop to Drill_NetWorth.
  → 3rd Esc: pop to Dashboard, with ui_net_worth still yellow-bordered.
```

Total keystrokes from "I want to investigate" to "I'm looking at Trader Joe's transactions on the Sapphire card": **8 keys**. In v0.2: not possible without leaving Dashboard, switching to Transactions, eyeballing dates and amounts to mentally filter.

### Transcript 2 — "Is the Bank of America sync still failing?"

```
User opens TerminalFinance.
  → Dashboard. The sync_status widget shows "[!] Bank of America auth failed".

User presses : then types "sync" then Enter.
  → :sync-status is the top match (the palette's lookup against "sync-status",
    ":refresh", "sync_status drill" returns sync-status first).
  → Drill_SyncStatus opens. The Bank of America row is highlighted in amber.

User presses j.
  → Selects the Bank of America row. Details panel updates: last_attempt
    ITEM_LOGIN_REQUIRED at 2026-04-01 04:00:03 UTC.

User presses R (capital R).
  → PlaidService::link_account(...) runs in re-auth mode for that Item.
    (Phase 4 plumbing.)

User presses Esc twice.
  → Back to Dashboard.
```

Total: **5 keys** from cold start to re-auth flow. In v0.2: no path at all.

### Transcript 3 — "Show me a CSV of this month's restaurant spending."

```
User opens TerminalFinance.
User presses :.
  → Palette opens.

User types "export tx".
  → Top match: ":export csv <path>".  Below it: ":transactions",
    ":tx-list-export".

User presses Tab.
  → Typed text completes to ":export csv ".  User is now in argument mode.

User types "/tmp/april.csv" and presses Enter.
  → Current view (Dashboard) doesn't have a list to export.  Palette returns:
    "No tabular view focused — drill into a category or open Transactions."

User presses Tab.
  → Top-level view tab cycles to Accounts.

User presses Tab.
  → Cycles to Transactions.

User presses /Trader and Enter.
  → Transactions filtered to Trader-* descriptions.

User presses : then types "export csv /tmp/april.csv" and Enter.
  → CSV written.  Status bar: "Exported 12 rows to /tmp/april.csv."
```

Total: **~15 keys**. The point is not minimum keystrokes; the point is that the user *knew where to look* at each step because the status bar and the palette's response text guided them.

---

## Appendix F — what we explicitly do not change in v0.3

To prevent scope creep during implementation, these are the v0.2 surfaces v0.3 leaves alone:

- **The four top-level views.** Dashboard, Accounts, Transactions, Budget. Their internal layouts get hjkl aliases and `/` filter, but the structure is preserved.
- **The entity tab strip** (`src/main.cpp:204-214`). Stays as-is. `1`/`2` still switches.
- **The `LED_BLUE` brand color** (`src/views/ViewCommon.h:10`). Stays as the chrome color. The `kTokens` struct adopts it as `chrome`.
- **The snapshot harness API** (`tests/snapshot/snapshot_helper.h`). Extended, not rewritten. Existing 8 fixtures remain valid through the migration (new fixtures join them; if any existing fixture changes, the changing test must justify the diff in its PR description).
- **The CLI flags** (`--login`, `--enroll`, `--logout`, `--whoami`, `--migrate-from-local`). All unchanged.
- **The two-binary architecture.** Client + server boundaries are untouched. v0.3 is entirely client-side.
- **The `consolidation_ui` widget and the `ui_updates` widget** (`src/views/widgets/consolidation_ui.{h,cpp}`, `src/views/widgets/ui_updates.{h,cpp}`). Out of v0.3 scope. Their fixtures (`tests/snapshot/fixtures/consolidation_ui.txt`, `tests/snapshot/fixtures/updates.txt`) are not modified.

---

## Appendix G — risk register

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| FTXUI 5.0.0 lacks a primitive we assume (e.g., `dbox` overlay semantics differ on Windows) | Low | Medium | v0.3-1 ships first with a thin "FTXUI sanity test" snapshot that proves `dbox` and `borderStyled(ROUNDED)` render identically on both runners. Phase 0 of v0.2 already established the dual-OS CI matrix. |
| Drill views balloon in LOC because each one wants its own micro-features | Medium | Medium | The five drill mockups in §3b are the contract. Reviewer rejects drill PRs that exceed ~150 LOC each without justification. |
| `:` palette becomes a god-mode dumping ground for every flag and option | Medium | Low | The 20-command vocabulary is a ceiling, not a starting point. v0.3 reviewer enforces: any new command must (a) map to an existing flow and (b) have an alias listed in §3c. v0.4 can expand. |
| Color migration breaks every snapshot test at once | High | Low | Expected. v0.3-5 is dispatched after v0.3-4 specifically so the snapshot-diff review pass happens once, not five times. Snapshot updater flag (`--update-snapshots`) regenerates all 16+ fixtures in one command. |
| Helix-style mode-prefix muscle memory leaks in (someone wants `Space f` for file-list) | Low | Low | v0.3 doesn't bind `Space`; explicit no-op. v0.4 can add. |
| Re-auth flow (Drill_SyncStatus `R` key) requires Phase 4 plumbing that hasn't landed | Medium | Medium | Drill_SyncStatus's `R` key gates on `BackendClient::supports_reauth()` feature flag. If false, status bar shows `[R] Re-auth (unavailable — backend Phase 4 pending)`. The drill view ships either way. |
| Fuzzy-match library has a corner-case bug | Low | Low | Vendored, not external dep. The fts library is a single header with known test cases; v0.3-4 runs the original tests as part of CI. |

---

## Appendix H — language for v0.3 release notes

For when v0.3 ships, this is the user-facing summary. Drop into the README or CHANGELOG.

> **v0.3 — The Dashboard Becomes a Launchpad**
>
> v0.2 made the Dashboard real. v0.3 makes it *operable*.
>
> - Press `Tab` on the Dashboard to focus a widget. The widget gets a yellow border.
> - Press `Enter` to drill into the focused widget. You get a full-screen detail view: accounts that make up your net worth, suppliers behind your Shovel Score, transactions per category.
> - Press `Esc` to back out. Breadcrumb at the top shows where you are.
> - Press `:` to open a command palette: type `:tx` to jump to Transactions, `:plaid-link` to add an account, `:export csv` to dump the current view.
> - Press `?` to see every key available in your current context.
>
> Color is now consistent: red means a value is below zero (negative balance, an error). Yellow means "this is what's focused right now." Magenta on the Shovel Intelligence panel highlights MoM increases — the thesis.
>
> Existing keys still work. Arrows still work. `1`/`2` still switches entity. `Tab` still cycles views *when no widget is focused*.

---

*End of design proposal.*
