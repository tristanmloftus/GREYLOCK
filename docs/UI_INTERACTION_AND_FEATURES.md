# TerminalFinance — UI Interaction & Features Spec

> **Owner:** Rory (UI/UX) + Tristan (engineering).
> **Status:** Draft — fill in as you go. Claude pre-populated the framing,
> current state, and the open questions Rory raised on 2026-05-16.
> Anything tagged **`[FILL]`** is for you to answer.

---

## 1. What this project is

**TerminalFinance** (codename Greylock) is a private, household-scope
**consolidated banking dashboard** for the Loftus family office. It runs as a
native TUI client on macOS/Linux/Windows talking to a self-hosted backend
that owns the encrypted database and all Plaid API calls.

**Not** a SaaS, not a product, not for external users. Two users (Rory,
Tristan), two domains (`#me` personal, `#pcc` business). The TUI is the
UI by **security choice** — no browser surface, no public endpoint, all
data on a Tailscale-only host.

### Current scope (v1, locked)

In scope right now:
- Accounts: unified view across all linked banks/brokerages/credit cards.
- Transactions: 4-axis attribution (entity / account / category / merchant).
- Reimbursements: cross-entity expense flagging.
- Current-state cash flow.

Deferred to v2/v3:
- Leverage modeling.
- Forward projections.
- Scenario planning.

### Today's architecture (one paragraph each)

- **Backend** (`server/`): C++ HTTP server (cpp-httplib + TLS, port 8443).
  Owns a SQLCipher-encrypted SQLite database (`dev/terminalfinance.db`).
  Calls Plaid Production directly via libcurl. Talks to clients over the
  Tailscale tailnet only. Auth is passphrase + TOTP, sessions are bearer
  tokens with an 8-hour absolute timeout.
- **TUI client** (`src/`): FTXUI-based C++ TUI. Authenticates with
  `--login`, caches the session token in a platform-appropriate secret
  store (Keychain on macOS, DPAPI on Windows, mode-600 file on Linux),
  then loads all data from the backend via `RemoteBackendStorageService`.
  Falls back to a local JSON file when no session is cached.
- **Plaid integration**: Production tier, "Limited Production" free band
  (10 items per app). Currently one app; question 3.1 below is whether
  to split.

### What's broken or half-built (as of 2026-05-16)

- **Plaid sync drops `balance_cents`** — account balance always reads
  `$0.00` even after a successful transaction sync. Same shape as the
  `description` bug we fixed earlier today; needs a one-line `INSERT`/
  `UPDATE` patch in `PlaidSyncScheduler`.
- **"Shovel" widgets still present**: `ShovelScore`, `ShovelIntelligence`,
  `Drill_ShovelScore`, the supplier-map JSON, etc. These are vestigial
  OVRWCH-era code. **All shovel mentions to be removed** — call out in
  §4 below what should replace them.
- **No top-level Tab keybinding**: Tab cycles widget focus *inside* the
  Dashboard via `FocusController`. Top-level navigation is palette-only
  (`:accounts`, `:tx`, `:budget`, `:dashboard`). Discoverability problem.
- **Multi-user model is half-wired**: server supports N users + per-entity
  memberships, but the TUI defaults to seeding "Personal" + "Business
  LLC" entities for first-run, which collides with the real entity model.

---

## 2. Pre-populated current state (don't fill — for orientation)

### 2.1 Keybindings as they exist today

| Key | Where | Action |
|-----|-------|--------|
| `:` | global | Open command palette |
| `?` | global | Open help overlay |
| `Esc` | modal | Close palette/help |
| `q` | global | Quit (saves first) |
| `1` / `2` | global | Switch entity (Personal / Business) |
| `Tab` / `Shift-Tab` | Dashboard | Cycle widget focus |
| `Tab` / `Shift-Tab` | other tabs | Switch top-level view |
| `h` / `j` / `k` / `l` | varies by tab | Move selection |
| `Enter` | Dashboard widget | Drill into widget |
| `P` | Accounts | Initiate Plaid Link |
| `L` | Accounts | Initiate Plaid sandbox link |
| `S` | Accounts | Save (no-op under remote storage) |

Command palette aliases: `dashboard`, `accounts`, `tx`, `budget`,
`personal`, `business`, `plaid-link`, `plaid-test`, `config`, `quit`,
`net-worth`, `shovel-score` *(to remove)*, `sync-status`,
`shovel-intelligence` *(to remove)*.

### 2.2 Dashboard widgets (5 panels)

```
┌─ Net Worth ─────┐ ┌─ Shovel Score ──┐ ┌─ Connection Status ─┐
│ $0.00           │ │ 16/100          │ │ No accounts linked. │  <- REMOVE
│ Checking: $0    │ │ WAITING TO DIG  │ │ Press [P] to link.  │
│ Savings:  $0    │ │ Total: $3289.17 │ │                     │
│ Credit:   $0    │ │ Companies: 1    │ │                     │
│ Investment: $0  │ └─────────────────┘ └─────────────────────┘
└─────────────────┘
┌─ Shovel Intelligence ──────┐ ┌─ Top Spending Categories ─────┐
│ JPM  $3289  ^ 15.9% MoM    │ │ No transactions this month.   │  <- REMOVE shovel,
└────────────────────────────┘ └───────────────────────────────┘     keep categories
```

### 2.3 What we proved works end-to-end on 2026-05-16

- Real bank link: USAA Plaid Production item.
- 135 real transactions synced into SQLCipher DB with descriptions.
- TUI authenticates, caches session, and loads all data from backend.
- Transactions tab renders all 135 with merchant names, dates, amounts.

---

## 3. Open architectural questions (Claude's recommendation + your call)

### 3.1 Plaid app structure — one app, two apps, or per-user apps?

**Constraints:**
- Plaid Limited Production = **10 items free per app** (one "item" =
  one bank login, regardless of how many accounts that login exposes).
- Each Plaid app has its own `client_id` + `secret`. They're managed
  in the Plaid dashboard, scoped to a Plaid "company" (owner).
- Greylock's data model already supports multi-entity scoping — every
  account row has an `entity_id` foreign key, and users have
  per-entity memberships.

**Options:**

**A. One Plaid app for everything.** Personal banks + PCC banks + both
   users all hit the same client_id. Greylock scopes by `entity_id`
   internally. Single env file. Tight at 10 items shared across
   everything.

**B. Two Plaid apps — Personal + PCC.** Personal app for both users'
   personal banks; PCC app for PCC business banks. Server picks app by
   `entity.kind`. 20 items total. Cleaner audit/compliance separation
   (PCC books never share an API key with personal).

**C. Three Plaid apps — Rory personal + Tristan personal + PCC.**
   Each user owns/manages their own personal Plaid app; PCC owns its
   own. 30 items total. Best isolation, most operational overhead.

**D. Four Plaid apps — per user × per domain.** What Rory floated
   ("two Plaid accounts per user, 10 personal + 10 business"). 40
   items total but the per-user split makes PCC's books fragmented
   across two API keys, which is bad for PCC bookkeeping.

**Claude's recommendation: Option C.**
- PCC is one LLC → its banking should live under one Plaid app owned
  by PCC, not split per user. Both you and Tristan get read access via
  Greylock's entity-membership model, but the Plaid API key is PCC's.
- Personal is per-user identity → each of you owns your own personal
  Plaid app, tied to your personal email. You'd never want Tristan's
  personal banking flowing through Rory's Plaid app or vice versa.
- 30 free items (10 per app) is comfortable headroom; current usage
  is ~3 personal banks per person + 2-3 PCC banks = ~8-9 items total,
  well inside one app's free tier per domain.

**[FILL] decision:** ___

**[FILL] notes / objections:** ___

### 3.2 First-run entity seeding

Currently `main.cpp` seeds "Personal" + "Business LLC" entities on a
fresh install with no remote data. This is wrong for the real
multi-tenant model.

**Claude's recommendation:** Remove the seed. First-run UX should be:
"You haven't been added to any entities yet. Ask the workspace owner
to invite you." For the workspace owner (first user ever), seed one
entity matching their email's display name.

**[FILL] decision:** ___

### 3.3 What replaces the shovel widgets?

Three Dashboard panels need replacement: `ShovelScore`,
`ShovelIntelligence`, and the supplier-map fed `Drill_ShovelScore`.

**Candidate replacements (ranked by how much Rory will actually use them):**
- Cash flow this month: income, expenses, net.
- Largest 5 transactions this period.
- Upcoming bills / recurring debits.
- Balance trend per account (sparkline).
- Budget-vs-actual gauge.
- Loan balances + payoff curve.
- Investment positions (when brokerage Plaid wires up).
- Reimbursements outstanding (cross-entity).

**[FILL] which 3-4 do you want as the new dashboard panels?** ___

---

## 4. UI interaction sections (fill these out)

### 4.1 Top-level navigation

How should you move between Dashboard / Accounts / Transactions / Budget?

- [ ] Keep Tab for widget focus inside Dashboard; use `:cmd` for top-level.
- [ ] Rebind Tab to top-level; use arrow keys for widget focus.
- [ ] Number keys (`F1`-`F4` or `g d` / `g a` / `g t` / `g b` like vim).
- [ ] Other: ___

**[FILL] preferred model:** ___

**[FILL] keybinding map:**

| Key | Action |
|-----|--------|
|     |        |

### 4.2 Entity & user model

- How many entities do you expect? List them:
  - Rory: `#me Rory`, `#pcc PCC LLC`
  - Tristan: `#me Tristan`, `#pcc PCC LLC` (shared)
  - **[FILL] add/correct:** ___

- Entity-switch UI: number keys (current), tab strip, dropdown?
  - **[FILL]:** ___

- When viewing PCC (shared entity) — should both users see the same
  account list, or filtered by who linked which bank?
  - **[FILL]:** ___

### 4.3 Dashboard

- Which 3-4 widgets do you want (see §3.3)?
  - **[FILL]:** ___

- Should widgets be configurable per-user, or fixed for everyone?
  - **[FILL]:** ___

- Drill-into pattern: full-screen takeover, side panel, modal?
  - **[FILL]:** ___

### 4.4 Accounts view

Current: flat list `# | Name | Type | Balance`.

- Group by institution? By entity? By type?
  - **[FILL]:** ___

- Per-row actions you want: rename, hide, unlink, force-resync, …
  - **[FILL]:** ___

- Balance precision: cents always, dollars when ≥$1000, configurable?
  - **[FILL]:** ___

### 4.5 Transactions view

Current: flat chronological list, no filtering, no editing.

- Default filter: this month, last 90 days, all time, …
  - **[FILL]:** ___

- Sortable columns:
  - **[FILL]:** ___

- Filters you need: by account, by category, by entity, by amount range,
  by merchant, by reimbursement status, by uncategorized
  - **[FILL]:** ___

- Edit-in-place fields: category, notes, entity reassignment, split-tx
  - **[FILL]:** ___

- Bulk actions: select N rows, recategorize, mark reimbursed
  - **[FILL]:** ___

- Search: substring? regex? merchant-only?
  - **[FILL]:** ___

### 4.6 Categories

Today: server has a `categories` table, TUI doesn't expose it.

- Where do categories live in the UI — own tab, sub-view of Transactions,
  modal from category column?
  - **[FILL]:** ___

- Hierarchy: flat, two-level (Group > Category), three-level?
  - **[FILL]:** ___

- Auto-categorization rules (merchant → category) — UI to edit?
  - **[FILL]:** ___

### 4.7 Budget view

Today: skeleton view, no real layout yet.

- Per-entity or unified? Per-month or rolling?
  - **[FILL]:** ___

- Display: bars, numbers only, table with variance column?
  - **[FILL]:** ___

- What does "over budget" trigger — color, banner, status-bar warning?
  - **[FILL]:** ___

### 4.8 Sync / refresh UX

- Manual refresh keybinding (currently broken — `[R]` says "not wired"):
  - **[FILL]:** ___

- Auto-refresh interval? On launch only? Per-tab open?
  - **[FILL]:** ___

- Sync status indicator: status bar, per-account dot, dedicated panel?
  - **[FILL]:** ___

- Failed-sync handling: silent retry, prompt, banner?
  - **[FILL]:** ___

### 4.9 Plaid link flow

Today: `[P]` opens Plaid Link in a browser; user completes; webhook
returns. UX is functional but minimal.

- Should we show "linking…" state in the TUI while the browser is open?
  - **[FILL]:** ___

- Post-link: auto-navigate to the new account, or stay on Accounts?
  - **[FILL]:** ___

- Re-auth flow for expired items: dedicated banner, status icon, modal?
  - **[FILL]:** ___

### 4.10 Loans, investments, future data types

Plaid supports liabilities (loans + credit cards beyond just statements),
investments (brokerage positions + holdings), and identity. Currently
only `transactions` is wired.

- Priority order for adding these:
  - **[FILL]:** ___

- Display surface: own tabs, sub-views, dashboard panels?
  - **[FILL]:** ___

### 4.11 Reimbursements (cross-entity)

Scope: a transaction posted to one entity that should be paid by another
(e.g. PCC pays for a personal expense; needs flagging + tracking until
resolved).

- Where does the user flag a tx as reimbursable — inline in Transactions,
  dedicated tab, both?
  - **[FILL]:** ___

- Status states: pending, paid, written-off?
  - **[FILL]:** ___

- Auto-detect heuristics: merchant patterns, account-of-origin rules?
  - **[FILL]:** ___

### 4.12 Status bar, help, palette

Today: bottom row shows `[1-2] Switch entity  [Tab] Switch view  [P] Link
Plaid  [L] Link test  [C] Config  [Q] Quit`.

- Status bar contents: keybindings, current entity, sync clock, all of these?
  - **[FILL]:** ___

- Help overlay (`?`): scrollable cheat-sheet, contextual to current tab?
  - **[FILL]:** ___

- Palette: should it support fuzzy match, or only exact prefix?
  - **[FILL]:** ___

### 4.13 Keyboard ergonomics

- Vim-style (`hjkl`, `gg`, `G`, `/` search)?
- Emacs-style (`C-n`, `C-p`, `C-s`)?
- Both?
- Mouse: ever? Or strictly keyboard?
  - **[FILL]:** ___

### 4.14 Cosmetic / theme

- Colors: any meaningful color use (red for negative, green for
  positive, blue for transfers)?
- Borders: ASCII (`+--+`), Unicode boxes (current), or minimal?
- Dense vs spacious layout?
  - **[FILL]:** ___

---

## 5. Parking lot

Drop anything that doesn't fit above. Convert to a section above when
it's worth pursuing.

- ___
- ___
- ___

---

## 6. Decisions log

When you make a call above, copy a one-liner here with the date so
we have a trail.

| Date | Decision | Made by |
|------|----------|---------|
|      |          |         |
