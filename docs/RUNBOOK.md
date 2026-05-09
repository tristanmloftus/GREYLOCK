# Greylock — Operator Runbook

> v0.1.0 · last updated 2026-05-09

This is the operator's manual. Three audiences: Rory (owner), Tristan, Cade. Every procedure assumes you're on macOS with FileVault enabled and a strong account password — those are operator hygiene preconditions, not Greylock's job.

If something here disagrees with `docs/SPEC.md`, SPEC wins.

---

## 0. Preconditions (one-time per machine)

1. **macOS FileVault is on.** `System Settings → Privacy & Security → FileVault`. Greylock's defenses assume the disk is encrypted at rest.
2. **macOS account password is strong** (≥ 16 chars, not in any breach list).
3. **Auto-lock is enabled.** `System Settings → Lock Screen → Require password after screen saver`. Lower is better.
4. **Time Machine backups exclude `~/greylock/prisma/*.db*`.** The encrypted DB on disk is fine to back up encrypted, but you don't want Time Machine pulling old DB states that contain stale audit chains.

If any of these are off, Greylock still works but the "laptop thief, powered off" defense weakens. See `docs/THREAT_MODEL.md` §1.1.

---

## 1. First-time install

```bash
git clone <this repo> ~/greylock
cd ~/greylock

# 1. Tooling
brew install mkcert
mkcert -install                    # interactive sudo + Keychain prompt — adds local CA
corepack enable
pnpm install

# 2. Local TLS cert
pnpm setup:certs

# 3. Master passphrase → macOS Keychain
#    Pick a strong passphrase. ≥ 24 chars, not used elsewhere, not in your password manager
#    if you can avoid it (login keychain is the storage layer).
security add-generic-password -s greylock-master -a "$USER" \
  -w '<your-strong-master-passphrase-here>' -U

# 4. .env
cp .env.example .env
# Generate the per-deployment secrets:
node -e "console.log('SESSION_SECRET=' + require('crypto').randomBytes(32).toString('base64'))"
node -e "console.log('CRYPTO_PEPPER='   + require('crypto').randomBytes(32).toString('base64'))"
# Paste each into .env. Set ALLOWED_EMAILS to the three operator addresses.
# Set OWNER_EMAIL to Rory's address.
# Get PLAID_CLIENT_ID and PLAID_SECRET from your Plaid dashboard (sandbox tier
# is free; no review needed).

# 5. DB migration (one-shot)
DEV_DB_PASSPHRASE='<some-dev-passphrase>' pnpm prisma migrate dev --name init
#    (Note: DEV_DB_PASSPHRASE is the dev SQLCipher key derivation seed. It can
#     be anything; just keep it the same across runs of the same DB. In v0.1
#     dev mode this is the only path; production boot via Keychain ships in v0.2.)
```

After this, the `~/greylock` install is ready. The next step is enrolling your first passkey.

---

## 2. Enrolling a passkey

Greylock has no password reset flow and no "forgot my account" path. Enrolling a passkey is the only way in. There are two ways to mint enrollment URLs:

### 2a. CLI (preferred — owner runs this)

```bash
DATABASE_URL="file:./prisma/greylock.db" \
  DEV_DB_PASSPHRASE='<your-dev-passphrase>' \
  pnpm admin:enroll rory.patrick.loftus@gmail.com --name "Rory Loftus" --role owner
```

The script:
1. Validates the email is in `ALLOWED_EMAILS` AND not the placeholder.
2. Creates or updates the User row.
3. Mints a one-shot enrollment token (random 32 bytes, only the SHA-256 hash persisted).
4. Prints a URL to stdout.

**Send the URL to the recipient via a private channel.** It expires in 30 minutes. The cleartext token is **never** stored in the DB.

### 2b. In-app (after the owner is enrolled)

Owner navigates to `/admin/enroll`, fills in the form. Same effect.

### Completing the ceremony

Recipient opens the URL in a browser they trust on the device they want to use:

1. Page calls `/api/auth/registration/begin` with the `email` + `token`.
2. Browser pops the platform passkey UI (Touch ID / Face ID / security key).
3. Recipient authenticates locally; browser POSTs the attestation.
4. Server verifies, persists the `Passkey` row, derives + wraps a fresh per-user DEK, audits `passkey_enrollment success`.
5. Recipient is logged in.

---

## 3. Daily operation

```bash
# Terminal 1
cd ~/greylock
DATABASE_URL="file:./prisma/greylock.db" DEV_DB_PASSPHRASE='<dev-passphrase>' pnpm dev
# https://localhost:3000 is now serving the dashboard.

# Terminal 2
cd ~/greylock
DATABASE_URL="file:./prisma/greylock.db" DEV_DB_PASSPHRASE='<dev-passphrase>' pnpm sync
# Background sync worker. Runs every 15 min.
```

That's it. Either terminal can be killed and restarted independently. Killing the web process stops personal sync (it gates on active sessions); PCC sync is unaffected (the worker has its own PCC DEK).

To **stop** Greylock:
- `Ctrl-C` in each terminal. Both processes zeroize their in-memory keys before exit.

---

## 4. Lost laptop drill

If a Greylock-running laptop is lost or compromised:

```bash
# On a different machine you control:
# 0. Confirm the lost machine cannot reach a network OR is wiped.

# 1. Revoke every active session (idempotent; safe to run multiple times).
DATABASE_URL=".../greylock.db" DEV_DB_PASSPHRASE='...' \
  pnpm admin:revoke-all --reason "lost-laptop-2026-05-09"

# 2. Revoke every passkey for every user. v0.1 does this per-user:
pnpm admin:revoke rory.patrick.loftus@gmail.com --reason "lost-laptop-2026-05-09"
pnpm admin:revoke tristan.m.loftus@gmail.com  --reason "lost-laptop-2026-05-09"
# (Cade is on placeholder address until set; see Cade-onboarding section.)

# 3. Re-enroll on a known-good machine.
pnpm admin:enroll rory.patrick.loftus@gmail.com --role owner
# Repeat for each user.

# 4. Verify the audit chain is intact (paranoia — has the lost laptop tampered?):
pnpm admin:audit-verify
# Expect: OK verifiedCount=<N>
```

**What's NOT in v0.1**: master-passphrase rotation. Even though every operator's passkey is revoked above, an attacker with the lost laptop's filesystem + Keychain unlocked still has the passphrase and can open the encrypted DB. Mitigations:

- macOS FileVault → without your account password, nothing on disk is readable.
- iCloud Keychain → if iCloud is enabled, "remote sign-out" purges the device's local Keychain copy.
- For the truly paranoid v0.1.0 case: **shred the DB**. `rm -f ~/greylock/prisma/greylock.db*` and rebuild from Plaid. Plaid balances are still authoritative; your transaction history is rebuildable from the next sync.

Master rotation lands in v0.2. Track in the issues backlog.

---

## 5. Verifying the audit log

```bash
DATABASE_URL=".../greylock.db" DEV_DB_PASSPHRASE='...' pnpm admin:audit-verify
# OK verifiedCount=18421
```

Or in-app: `/admin/audit/verify`. Owner-only.

The chain construction is documented byte-exact in `docs/ARCHITECTURE.md` §7. Tampering any past row breaks the chain at the tampered seq + every subsequent row; `pnpm admin:audit-verify` returns `BROKEN at seq=<N>` on first mismatch.

If chain is broken and you didn't tamper: that's a forensic event. Snapshot the DB file, do not delete it. Sources of legitimate breakage: filesystem corruption, partial DB writes during a hard crash. Both are rare on SQLite WAL.

---

## 6. Cade's email is a placeholder

Until a real Cade email is configured, `cade-placeholder@greylock.invalid` is wired in the allowlist. **The auth layer rejects this exact address at registration time** — see `lib/auth/allowlist.ts:isPlaceholderEmail`. So an accidental enrollment can't succeed.

When the real address is known:

```bash
# 1. Update .env
#    ALLOWED_EMAILS=rory.patrick.loftus@gmail.com,tristan.m.loftus@gmail.com,<cade-real>@gmail.com

# 2. Update .env.example similarly (so the placeholder isn't reintroduced).

# 3. Restart pnpm dev + pnpm sync.

# 4. Enroll:
pnpm admin:enroll <cade-real>@gmail.com --name "Cade ..."

# 5. Add to PCC:
#    There's no admin route for adding a member yet — manually:
#    DATABASE_URL=... DEV_DB_PASSPHRASE=... pnpm tsx -e "
#      import('./lib/db/index.js').then(async (db) => {
#        const { booted } = await ...; // or use prisma directly
#        await booted.repos.pccMembershipRepo.add({ userId: '<cade-user-id>' });
#      });
#    "
#    (Phase 5 carry-forward: `pnpm admin:add-pcc-member <email>` script is a v0.2 deliverable.)
```

---

## 7. What's deferred to v0.2

These are documented carry-forwards. They do NOT block v0.1.0 operation but should ship before any production-equivalent use:

| ID | Item | Why deferred |
|---|---|---|
| M-2 | Move `wrappedUserDek` + `kekSalt` into public `domain.ts` types (drop `WrappedDekReader` shim) | Architecture cleanliness; not a security or correctness issue |
| L-5 | N-API binding for `LOCAL_PEERCRED` (currently mode 0600 + `getuid()` parity, equivalent on macOS) | Defense-in-depth; current implementation is sound for the threat model |
| C-1 | `pnpm admin:rotate-master` production wiring (currently a v0.1 stub) | Complex multi-step flow needs proper integration testing |
| C-2 | `pnpm admin:add-pcc-member <email>` script | Manual DB workaround documented in §6 |
| C-3 | Production boot path in `scripts/sync.ts` (currently dev-only) | Needs Keychain wiring; web-app boot path takes precedence in v0.1 |
| C-4 | Multi-passkey-per-user (laptop + iPhone enrollment) | One passkey per user is the v0.1 model; lose-device = re-enroll |

Tracked in the GitHub issues backlog.

---

## 8. Troubleshooting

### `pnpm dev` fails with "Failed to patch ESLint"
Pre-existing `eslint-config-next` incompat with ESLint 9. We dropped it in Phase 2 — if you see this, you're on an old branch. `git pull && pnpm install`.

### Browser shows "Not Secure" / cert warning on `https://localhost:3000`
You haven't run `mkcert -install`. See §1.

### Passkey ceremony fails with `webauthn_verification_failed`
- Check the ceremony cookie didn't expire (5 min default).
- Check the origin matches `WEBAUTHN_RP_ORIGIN` in `.env`.
- For replay attacks (counter regress): the authenticator's counter went backwards. This can be a real attack or a known issue with cloned passkeys (iCloud → new device). If it's the latter, revoke + re-enroll.

### Plaid Link won't load
- CSP requires `script-src 'self' https://cdn.plaid.com`. Verify `next.config.mjs` is current.
- Check the browser console for the actual CSP violation.

### Sync worker prints `[sync] keybridge unavailable`
The web process isn't running. Personal sync skips; PCC sync continues. Restart `pnpm dev`.

### Audit-verify returns `BROKEN at seq=<N>`
Treat as a forensic event. Do NOT delete the DB. Snapshot it (`cp greylock.db greylock.db.forensic-<timestamp>`) and investigate. The break is at row `<N>`; check what was written there in the surrounding window.

### `pnpm admin:rotate-master` says "not yet implemented"
That's expected for v0.1. See §4 lost-laptop drill for the v0.1 mitigations.
