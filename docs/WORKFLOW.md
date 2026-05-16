# GREYLOCK Workflow — Phase Gates and Cadence

> Project-specific operational workflow for GREYLOCK / TerminalFinance.
> Phase gates, current blockers, definitions of done. Cross-project
> roles + channels + per-change loop live in PCC-SharedOS
> `WORKFLOW.md` — read that first if you're new to how Rory + Tristan
> + their AIs collaborate.

## Phase gates

| # | Phase | Exit gate | Status |
|---|---|---|---|
| **0** | Security recovery | All 5 boxes on GREYLOCK#1 ticked: Plaid Production rotated, Plaid Dev tier active with new keys, new `TF_MASTER_KEY` generated + backed up, `~/.greylock.env` updated with `chmod 600`, no bank linked using burned creds | **OPEN** — blocks Phase 1 |
| **1** | Sandbox end-to-end | Plaid Sandbox: `link_token_create` → `item_public_token_exchange` → `/transactions/sync` works against a fake bank; TUI `L` keybind opens browser, polls, flips `is_plaid_linked`; tests for the three new `PlaidApiClient` methods + `PlaidTokenBroker` round-trip pass | Code written locally (per OpenCode reply on #1); **not pushed yet** |
| **2** | First real bank (Dev tier) | USAA (or another non-OAuth bank) linked end-to-end; ≥30 days of transactions in DB; force-sync endpoint works; DB unlocks correctly with the rotated `TF_MASTER_KEY` | Blocked on Phase 0 + Phase 1 |
| **3** | Webhook + hardening | Plaid webhook handler at `POST /plaid/webhook` verifies the JWT signature and processes `TRANSACTIONS:SYNC_UPDATES_AVAILABLE`, `ITEM:ERROR`, `ITEM:LOGIN_REQUIRED`; item-list endpoint + drill view in TUI; ITEM_LOGIN_REQUIRED re-auth flow exercised | Phase 2 backlog |
| **4** | Multi-bank + multi-entity | Rory's accounts + Caitlin's + PCC LLC accounts all linked; entity-attribution correct in TUI dashboard and consolidation view; multi-bank scenario tested for transaction-hash collisions | After Phase 3 |
| **5** | Production tier evaluation | Decision on whether to keep Plaid Dev or pursue Production review (only if item-count >100 or external-user requirements emerge); not work, just a Rory decision | Calendar gate, not work gate |

---

## Per-phase work breakdown

### Phase 0 (security recovery)

**Owner:** Tristan (credential custody)
**Deliverable:** Status comment on GREYLOCK#1 with 5 boxes ticked. No key values pasted (per § Secrets discipline in `docs/QA_PROMPT.md`).

Steps:
1. Plaid dashboard → Team Settings → Keys → **Rotate** Production secret (the leaked one stops working)
2. Plaid dashboard → switch active environment from Production → Development
3. Generate fresh `TF_MASTER_KEY` (`openssl rand -base64 32`); paper-backup to two safes; Bitwarden Send for the one-time digital share
4. Update `~/.greylock.env`: new `PLAID_CLIENT_ID`, `PLAID_SECRET`, `PLAID_ENV=development`, new `TF_MASTER_KEY`; verify `chmod 600`
5. Confirm no bank was ever linked using the burned creds (if one was, rotate everything again)
6. Reply on GREYLOCK#1 with the 5 boxes ticked

### Phase 1 (Sandbox end-to-end)

**Owner:** OpenCode (under Tristan)
**Deliverable:** PR merged to `main`; sandbox link → sync round-trip demonstrated.

Work units (per `docs/PLAID_SETUP.md` Track 2):

| Unit | File(s) | Description |
|---|---|---|
| 1 | `server/plaid/PlaidApiClient.{h,cpp}` | Add `link_token_create`, `item_public_token_exchange`, `item_remove` |
| 2 | `server/data/PlaidLinkHandler.{h,cpp}` | New handler — `POST /accounts/:id/link/init`, `GET /link`, `POST /accounts/:id/link-plaid` |
| 3 | `server/data/PlaidLinkHandler.cpp` | Inline HTML Link page (~80 lines), loads Plaid Link JS |
| 4 | `server/data/AccountsHandler.cpp` | Add `POST /accounts/:id/sync` (force-sync, debugging) |
| 5 | `src/services/PlaidService.cpp` | Rewrite `link_account` — POST `/link/init` → browser → poll for `is_plaid_linked` |
| 6 | `src/services/PlaidService.cpp` | `open_browser` platform shims (`ShellExecute` / `open` / `xdg-open`) |
| 7 | `src/views/AccountsView.cpp` | `L` keybind when account selected + not yet linked |
| 8 | `tests/test_plaid_api_client.cpp` | Extend with the three new methods against stub `IHttpClient` |
| 9 | `tests/test_plaid_token_broker.cpp` | Round-trip with fresh master key |

Branch: `feat/plaid-link-flow`. One PR per logical chunk if it gets big; one PR total if it's small enough.

Verification before merge:
- `ctest --test-dir build --output-on-failure` — all targets pass including the new tests
- CI green on the branch
- Claude review approved

### Phase 2 (first real Dev-tier bank)

**Owner:** Tristan operationally; OpenCode does the click-through; Claude observes via issue thread.
**Deliverable:** Issue closes with screenshots/output showing USAA linked + transactions in DB.

Steps (after Phase 1 merged):
1. Start `TerminalFinanceServer` with the rotated env
2. Enroll + login through TUI
3. Create account stub in TUI, press `L`
4. Authenticate with USAA in the browser
5. Wait 15 min OR call `POST /accounts/:id/sync`
6. Verify in DB:
   ```sql
   SELECT id, name, is_plaid_linked, length(encrypted_token) FROM accounts;
   SELECT COUNT(*), MIN(posted_at_unix), MAX(posted_at_unix) FROM transactions WHERE account_id='<id>';
   ```

### Phase 3 (webhook + hardening)

**Owner:** Claude drafts spec; OpenCode implements under Tristan.

Work units:
- `server/plaid/PlaidWebhookHandler.{h,cpp}` — `POST /plaid/webhook`
- JWT signature verification (Plaid uses asymmetric signing — fetch the JWK on first use, cache, verify on every webhook)
- Handle event types: `TRANSACTIONS:SYNC_UPDATES_AVAILABLE`, `ITEM:ERROR`, `ITEM:LOGIN_REQUIRED`, `ITEM:NEW_ACCOUNTS_AVAILABLE`
- `server/data/AccountsHandler.cpp` — `GET /items` (list items + status), `POST /accounts/:id/relink` (mint a Link token in update mode for `ITEM_LOGIN_REQUIRED`)
- `src/views/drills/Drill_SyncStatus.{h,cpp}` — extend per-item with last-sync-time, error state, re-auth required indicator
- Tailscale Funnel config for `/plaid/webhook` route only (rest of API stays tailnet-private)
- Tests: webhook signature validation, event dispatch, state machine for item lifecycle

### Phase 4 (multi-bank + multi-entity)

**Owner:** Tristan operationally; entity attribution work is engineering, owned by OpenCode under Tristan.

Cross-cutting concerns:
- Entity assignment per account (Personal vs PCC LLC) — needs UI flow if not already wired
- Transaction dedup across accounts (same merchant → same supplier mapping, different accounts → different transaction rows)
- Consolidation view tested with mixed-entity inputs

### Phase 5 (Production tier eval)

**Owner:** Rory decision; Claude prepares the analysis.

Decision inputs:
- Current item count vs the 100-item Dev cap
- External-user requirements (Caitlin? Brian? Cecilia? Anyone outside the immediate family?)
- Plaid Production pricing at the projected volume
- Effort estimate for Plaid's Production security review (~2 weeks calendar)

---

## Current state (2026-05-16)

**Phase 0:** OPEN. OpenCode's last reply on GREYLOCK#1 contained the leaked secrets — the trigger for everything below. The 5 corrective checkboxes are issued; no confirmation yet.

**Phase 1:** Code work claimed locally per OpenCode's first reply on GREYLOCK#1. NOT pushed. Need a `feat/plaid-link-flow` branch to land before review starts.

**Blockers:**
- Phase 0 must close before Phase 1 merges (don't merge code that will run against burned creds in any test path)
- No `feat/plaid-link-flow` branch on remote yet
- OpenCode hasn't surfaced verifiable status since the initial reply

**Watch:**
- CI status on `de5917a` shows `queued` — self-hosted runner (`skynet`) may need attention. Not Phase 0 blocker but should be sorted before Phase 1 PR.

---

## Cadence (this project specifically)

| When | What |
|---|---|
| Session start (both AIs) | Run `SESSION_START.md` per PCC-SharedOS root |
| Daily during Phase 0–2 | Tristan/OpenCode comment on the relevant tracking issue with progress (or "no progress today" — silence ≠ done) |
| Weekly | Claude posts a roll-up issue: phase status table + open blockers |
| Phase exit | Issue closed; durable insight distilled to PCC-SharedOS `synthesis/<topic>.md` |
| Incident | Immediate issue + bridge ping; full post-mortem in synthesis if non-trivial (see `synthesis/greylock-plaid-build.md` § 2026-05-15 incident) |

---

## Definition of "done" per work unit

- Code committed to a branch (not local-only)
- Tests for the new code paths exist and pass locally
- CI green on the branch
- PR opened with `@-mention` to Claude for review
- Claude review comments addressed
- Tristan merges
- Phase gate verification step runs against `main` post-merge
- Status comment on the relevant tracking issue

Without each of these, the work unit is not done — regardless of how
much was written locally.

---

## Related

- PCC-SharedOS: `WORKFLOW.md` — cross-project workflow (roles, channels, per-change loop)
- PCC-SharedOS: `SESSION_START.md` — session-start protocol both AIs run
- PCC-SharedOS: `synthesis/greylock-plaid-build.md` — durable build record + 2026-05-15 incident
- This repo: `docs/PLAID_SETUP.md` — Plaid Dev signup + Link flow + first-sync verification
- This repo: `docs/QA_PROMPT.md` — game-tester QA loop for OpenCode + secrets discipline
- This repo: `docs/RUNBOOK.md` — server operations
- This repo: `docs/ARCHITECTURE.md` — system design
- This repo: `docs/THREAT_MODEL.md` — security model
- This repo: `V0_2_PLAN.md` — the six-phase v0.2 build plan
- Issues: `tristanmloftus/GREYLOCK#1` (security checklist, in flight), `#2` (MCP bridge deployment)
