# Plaid Developer Setup — GREYLOCK

> End-to-end playbook for wiring real Plaid Developer-tier credentials
> into a running GREYLOCK server, linking a real bank, and pulling
> live transactions. ~30 min of Plaid signup + 3-4 hours of
> implementation work + 30 min of run-through.

## Scope

- **Tier:** Plaid Development (≤100 items, free, real data).
- **Goal tonight:** end-to-end flow — TUI press "Link," browser
  opens, real bank auth, transactions land in the DB.
- **Out of scope tonight:** webhooks, OAuth-redirect for major banks,
  ITEM_LOGIN_REQUIRED re-auth, item-list view. See "Phase 2 backlog"
  at the bottom.

---

## Track 1: Plaid account + credentials

### 1. Sign up for Plaid

Go to `https://dashboard.plaid.com/signup`. Use a real email — keys
are tied to it. Sandbox keys appear immediately.

### 2. Submit "Use case" form for Development tier

In the dashboard:
- **Company information:** real entity name (e.g., a family LLC).
- **Use case:** "Personal finance management" / "Business expense
  tracking." Description: "Internal multi-entity finance tool for our
  family office; not customer-facing."
- **Products:** check `transactions` (required), `auth` (useful for
  ACH later), `identity` (optional).

Plaid reviews. Development activation is usually same-day; can be
minutes, can be overnight. If it doesn't activate tonight, start
in **Sandbox** — the code is identical, swap `PLAID_ENV=sandbox` to
`development` later.

### 3. Grab keys

`Team Settings → Keys`. Copy:
- `PLAID_CLIENT_ID`
- `PLAID_SECRET` (one value per environment — grab Sandbox + whichever
  tier is currently active)

### 4. Register redirect URI (only for OAuth banks)

Major US banks (Chase, BoA, Wells, Cap One, Schwab, Fidelity, Vanguard)
require OAuth in Development. Plaid requires the redirect URL to be
pre-registered:

`Team Settings → API → Allowed redirect URIs → Add URI`
→ `https://localhost:8443/link/oauth-return`

To skip this for tonight, pick a non-OAuth bank first (Ally, USAA,
Discover, Marcus, most credit unions).

### 5. Generate `TF_MASTER_KEY` and back it up

```sh
openssl rand -base64 32
```

**Critical.** This is the master key for envelope encryption of every
Plaid access token in the DB. Lose it → every linked account is
permanently dead (no recovery; you re-link from scratch).

Save it in **two** secure places:
1. Password manager entry: "GREYLOCK TF_MASTER_KEY"
2. Second secure location (e.g., printed in a safe, partner's password
   manager)

Do NOT commit, email, or paste in chat.

---

## Track 2: Implementation work

A new branch off `main`. Suggested name: `feat/plaid-link-flow`.

### 6. Server: extend `PlaidApiClient`

`server/plaid/PlaidApiClient.{h,cpp}` — three new public methods:

| Method | HTTP | Returns |
|---|---|---|
| `link_token_create(user_id, account_id, redirect_uri)` | `POST /link/token/create` | `link_token` (TTL ~4 hours) |
| `item_public_token_exchange(public_token)` | `POST /item/public_token/exchange` | `{ access_token, item_id }` |
| `item_remove(access_token)` | `POST /item/remove` | bool ok (for unlink later) |

Auth fields (`client_id`, `secret`) already wired via
`build_auth_fields()`. Reuse `IHttpClient` injection pattern.

Audit-log emissions (existing audit infra):
- `plaid_link_token_minted` on `link_token_create` success
- `plaid_public_token_exchanged` on exchange success
- `plaid_item_added` on token-broker store success
- `plaid_item_removed` on item_remove success

### 7. Server: new `PlaidLinkHandler`

New file `server/data/PlaidLinkHandler.{h,cpp}`. Three routes:

| Route | Purpose |
|---|---|
| `POST /accounts/:id/link/init` | Session-gated. Calls `link_token_create`, returns `{ link_url: "https://<host>/link?account_id=...&token=...", expiration }`. |
| `GET /link?account_id=...&token=...` | Public-ish (page is rendered server-side; `token` is the short-TTL Plaid link_token). Serves the HTML Link page. |
| `POST /accounts/:id/link-plaid` | Session-gated. Body `{ public_token }`. Calls `item_public_token_exchange` → stores via `PlaidTokenBroker` → updates `accounts.is_plaid_linked = 1`, `plaid_item_id = <item_id>`. |

Register in `server/main.cpp`. Gate on `PLAID_CLIENT_ID`/`PLAID_SECRET`
present (same pattern as `PlaidSyncScheduler`).

### 8. Server: HTML Link page

In `PlaidLinkHandler.cpp`, serve a ~80-line HTML page either inline as
a string constant or from `data/link.html`. Skeleton:

```html
<!doctype html>
<html><head><title>GREYLOCK · Link Account</title></head>
<body>
  <div id="status">Loading Plaid Link…</div>
  <script src="https://cdn.plaid.com/link/v2/stable/link-initialize.js"></script>
  <script>
    const params = new URLSearchParams(location.search);
    const accountId = params.get('account_id');
    const linkToken = params.get('token');
    const handler = Plaid.create({
      token: linkToken,
      onSuccess: async (public_token, metadata) => {
        document.getElementById('status').innerText = 'Exchanging token…';
        const r = await fetch(`/accounts/${accountId}/link-plaid`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ public_token })
        });
        document.getElementById('status').innerText =
          r.ok ? 'Linked. You can close this tab.' : 'Link failed; check server logs.';
      },
      onExit: (err) => {
        document.getElementById('status').innerText =
          err ? `Exited: ${err.error_message}` : 'Exited.';
      }
    });
    handler.open();
  </script>
</body></html>
```

Note: the `fetch` to `/accounts/:id/link-plaid` carries the session
cookie/header. If you're using bearer tokens, server should accept
session via cookie OR add a session-token URL parameter (don't put the
bearer in the public Link URL).

### 9. Server: force-sync endpoint (debugging)

`server/data/AccountsHandler.cpp` — add:

`POST /accounts/:id/sync` — session-gated. Triggers
`PlaidSyncScheduler::sync_account(account_id)` synchronously, returns
`{ ok, new_transactions: N, latency_ms: M }`. Audit-log
`plaid_sync_started` / `plaid_sync_completed`.

Don't wire this to a periodic call from the TUI — debugging only. The
cron handles steady-state.

### 10. Client: rewrite `PlaidService::link_account`

`src/services/PlaidService.cpp` — replace the line-37 stub (currently
returns `"link_account: server endpoint not yet wired (4.D)"`).

New flow:

```cpp
bool link_account(const std::string& account_id, const std::string&) override {
    // 1. Ask server to mint a link token + build the URL.
    auto resp = backend_->post("/accounts/" + account_id + "/link/init", "{}");
    if (!resp.ok) { last_error_ = "link/init failed: " + resp.body; return false; }
    auto link_url = json::parse(resp.body)["link_url"].get<std::string>();

    // 2. Open default browser. Platform-specific.
    open_browser(link_url);

    // 3. Poll account status until is_plaid_linked or timeout.
    constexpr int kTimeoutSec = 300;
    for (int i = 0; i < kTimeoutSec / 2; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        auto a = backend_->get("/accounts/" + account_id);
        if (a.ok && json::parse(a.body)["is_plaid_linked"].get<bool>()) return true;
    }
    last_error_ = "link timed out after 5 min";
    return false;
}
```

`open_browser` helper — three branches:

```cpp
#ifdef _WIN32
  ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif __APPLE__
  std::string cmd = "open '" + url + "'";
  std::system(cmd.c_str());
#else
  std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1";
  std::system(cmd.c_str());
#endif
```

Sanitize the URL before passing to `system()` — only allow Plaid's
domain or your own server host. Don't let arbitrary strings reach the
shell.

### 11. Client: add "Link bank" action in AccountsView

`src/views/AccountsView.cpp` — bind `L` key when an account is selected
and `is_plaid_linked == false`. Triggers `plaid_->link_account(id, "")`.
Show modal-style "Linking… (browser opened)" state while polling.
On success refresh the row; on failure show `plaid_->last_error()`.

### 12. Tests

- `tests/test_plaid_api_client.cpp` — extend with the three new methods
  against a stub `IHttpClient` that returns canned Plaid responses.
- `tests/test_plaid_token_broker.cpp` — already exists; ensure the
  store-then-load round-trip still works after the link flow stores a
  new token. Add a test for the new code path.
- `tests/snapshot/` — add a snapshot for the "linking…" modal in
  AccountsView if your design has one.

---

## Track 1 + 2 merge

### 13. Set environment

On the server host:

```sh
cat > ~/.greylock.env <<'EOF'
PLAID_CLIENT_ID=...
PLAID_SECRET=...
PLAID_ENV=development          # or "sandbox" if Dev not yet active
TF_MASTER_KEY=...              # from openssl rand -base64 32; save elsewhere
EOF
chmod 600 ~/.greylock.env

# When starting server:
set -a; source ~/.greylock.env; set +a
```

### 14. Build + verify tests pass

```sh
scripts/generate-dev-cert.sh
cmake --build build --target TerminalFinanceServer
cmake --build build --target TerminalFinance
ctest --test-dir build --output-on-failure
```

Existing tests + the new `test_plaid_api_client` cases should all
pass before linking real money.

### 15. Back up the DB

```sh
cp data/data.db data/data.db.pre-plaid.bak
```

Restore point if anything corrupts during first-link debugging.

### 16. Start server

```sh
./build/TerminalFinanceServer
```

Watch log for:
- `PlaidApiClient: credentials present (env=development)`
- `PlaidSyncScheduler: started`
- `Server listening on https://0.0.0.0:8443`

### 17. Enroll + login (TUI)

```sh
./build/TerminalFinance --enroll
# Set passphrase, enroll TOTP, save the recovery code.
./build/TerminalFinance --login
```

### 18. Create account + Link

In TUI `AccountsView`:
1. Add a new account, name it after the bank.
2. Highlight it, press `L`. Browser opens.
3. Authenticate in the browser tab. Plaid Link shows "Success." Tab
   shows "Linked. You can close this tab."
4. TUI's poll notices, `is_plaid_linked` flips, view refreshes.

### 19. Force first sync (or wait 15 min)

```sh
# Find the session bearer from your TUI's stored session.
curl -k -X POST \
  -H "Authorization: Bearer $(cat ~/.config/TerminalFinance/session)" \
  https://localhost:8443/accounts/<account_id>/sync
```

Or just wait for the 15-min cron to fire.

### 20. Verify

```sh
sqlite3 data/data.db <<'SQL'
SELECT id, name, is_plaid_linked, length(encrypted_token) FROM accounts;
SELECT COUNT(*), MIN(posted_at_unix), MAX(posted_at_unix)
  FROM transactions WHERE account_id='<id>';
SQL
```

Expected:
- `is_plaid_linked = 1`
- `encrypted_token` length ~80-120 bytes (envelope-encrypted blob)
- transactions count > 0
- date range covers ~24 months back (Plaid Dev default look-back)

If anything's wrong: check the server log first, the audit log table
second. `plaid_public_token_exchanged` followed by `plaid_item_added`
should both be present.

---

## Phase 2 backlog (after tonight)

| Item | Why | Effort |
|---|---|---|
| `POST /plaid/webhook` with JWT signature verification | Real-time updates vs. 15-min cron; mandatory before any usable Production move | ~1 day |
| ITEM_LOGIN_REQUIRED re-auth flow | Banks rotate creds / MFA challenges; without this an item dies silently | ~0.5 day |
| Item-list endpoint + drill view in TUI | See which items are healthy, which are stale, which need re-auth | ~0.5 day |
| Extend `Drill_SyncStatus` per-item | Already on main; light extension to surface per-item state | ~2 hours |
| Tailscale Funnel for `/plaid/webhook` only | Public route for webhook ingress; rest of API stays tailnet-private | ~2 hours |
| Move from Dev to Production tier | Required if exceeding 100 items; Plaid security review ~2 weeks | (calendar, not effort) |

---

## Risk callouts

- **First time touching real money data.** Step 15's backup matters.
- **`TF_MASTER_KEY` is a single point of failure.** Two secure copies.
  Treat like a hardware wallet seed phrase.
- **Plaid Dev is rate-limited.** Don't loop sync calls while debugging.
- **OAuth banks need the redirect URI pre-registered in Plaid dashboard.**
  Step 4. Or pick a non-OAuth bank for the first Link.

## Secrets discipline (READ THIS — NON-NEGOTIABLE)

The following values **never** enter a commit message, issue body,
PR description, comment, session writeup, build report, status
update, or any markdown file — even in a private repo:

- `TF_MASTER_KEY` (any value)
- `PLAID_CLIENT_ID`, `PLAID_SECRET` (Sandbox, Dev, **and** Production)
- Any `access_token`, `public_token`, `link_token`, `session_token`,
  `enrollment_token`
- TOTP seeds, recovery codes
- Bank credentials, real account numbers, routing numbers
- Any raw hex string ≥32 characters from a `*_KEY` / `*_SECRET` env

**Bitwarden Send is the only sanctioned channel** for one-time
out-of-band sharing between Rory and Tristan. Not GitHub. Not Slack.
Not chat.

**Writeups use placeholders.** When documenting what got generated,
write `<redacted>` or `${TF_MASTER_KEY}`. Never the actual value.

**Generated → file → forgotten.** When you generate a key, write it
to `~/.greylock.env` (`chmod 600`), share via Bitwarden Send, then
never echo the value again — not in chat output, not in status
reports, not in build logs.

**When in doubt, redact.** There is no scenario where pasting a
secret into a GitHub artifact is the right call. Cheaper to redact
than to leak.

The `PCC-SharedOS` pre-commit hook pattern-blocks the obvious
variable names + raw long-hex strings. The hook is a backstop, not
a substitute for discipline.

---

## Operator quick reference

```sh
# Start server (after env is sourced)
./build/TerminalFinanceServer

# Tail key events
tail -f terminalfinance.log | grep -E 'plaid|link|sync'

# Inspect last sync per account
sqlite3 data/data.db "SELECT account_id, last_sync_unix, last_status FROM plaid_sync_state;"

# Force unlink (for testing)
curl -k -X DELETE -H "Authorization: Bearer $TOKEN" \
  https://localhost:8443/accounts/<id>/link-plaid
```
