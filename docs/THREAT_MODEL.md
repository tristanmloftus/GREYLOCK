# Greylock — Threat Model

Originally Phase 1 deliverable; finalized at Phase 5 (v0.1.0 tag) on 2026-05-09. The honest list of what we defend, what we don't, and where the trade-offs sit. Pair with `docs/ARCHITECTURE.md` (mechanism), `docs/API.md` (surface), `docs/SECURITY_AUDIT.md` (verification), `docs/RUNBOOK.md` (operations).

---

## 0. Scope and method

**System under threat:** the Greylock localhost deployment described in `docs/ARCHITECTURE.md`. One macOS host, two Node processes (web + sync worker), one SQLCipher DB on disk, three users, no inbound network beyond `localhost:3000`. Three operators are trusted to varying degrees (see §1.2).

**Goal of the model.** Make the residual-risk picture explicit so future changes can be evaluated against it. Every "accepted" risk is a decision, not a forgotten check.

**Method.** For each threat actor we answer: (a) what capabilities they have, (b) what they can read or do *if those capabilities were unmitigated*, (c) what defends against them, (d) how those defenses fail, (e) what residual risk we accept and why.

**Assets, ranked by impact-of-compromise.**

| # | Asset | Why |
|---|---|---|
| A1 | Plaid access tokens (PCC) | Live read-write credentials to PCC's bank data. |
| A2 | Plaid access tokens (personal) | Same, scoped per operator. |
| A3 | Master passphrase | Compromise unlocks A1 if the box is on (PCC DEK in process memory) or off (re-derives Master KEK from disk-stored wrap). |
| A4 | Per-user passkey | Compromise of a *physical* authenticator is the only way to decrypt that user's personal data. |
| A5 | Audit log integrity | Detection of compromise after the fact. |
| A6 | Session cookies | A live session cookie + same-host network access = impersonation. |
| A7 | DB on disk | Contains everything in encrypted form. |

---

## 1. Threat actors

### 1.1 Laptop thief

#### 1.1.1 — Powered off

**Capabilities.** Physical possession; unrestricted disk access; no boot password.

**Unmitigated impact.** Read DB → access encrypted Plaid tokens → if Master KEK can be re-derived, decrypt PCC tokens.

**Defenses.**
1. **macOS FileVault** (operator-managed; documented in `RUNBOOK.md`). Without the FV unlock, the disk reads as random bytes.
2. **SQLCipher** at the application layer. Even with FV unlocked, the DB file is encrypted with a key derived from the master passphrase; without that passphrase, the DB is opaque.
3. **macOS Keychain**. The master passphrase is stored in the user's login Keychain, encrypted at rest by the user's macOS account password (and Secure Enclave on Apple Silicon). Without the macOS account password, the Keychain item is opaque.

**Failure modes.**
- FileVault disabled by the operator. (Out of band — `RUNBOOK.md` makes FV setup a precondition; QA-SEC verifies once.)
- macOS account password is weak or known. (Operator hygiene, outside Greylock's control.)

**Residual risk: LOW.** Three independent layers must fail. We accept that someone with all three (FV off, weak Mac password, weak passphrase) can read everything; this is not a bar Greylock can raise alone.

#### 1.1.2 — Locked screen

**Capabilities.** As above, plus the machine is running, processes still loaded.

**Unmitigated impact.** If the machine is unlocked (e.g. by Touch ID coercion), Plaid tokens are readable from process memory in seconds (`vmmap`, `lldb` attach, etc.). The web process holds the Master KEK + PCC DEK + any active per-user DEK in plaintext.

**Defenses.**
1. **Lock screen**, FileVault active during sleep, Touch ID timeout configured.
2. **Process isolation.** Greylock processes run as the user, not as root, so without privilege escalation a different macOS account on the same host cannot attach a debugger.
3. **No password autofill.** Greylock has no password to autofill; passkey assertions require user verification (Touch ID).

**Failure modes.**
- Attacker forces Touch ID with the operator present. macOS itself does not defend against this.
- Attacker has the operator's macOS password. Same outcome.

**Residual risk: ACCEPTED.** A thief who can unlock the laptop can read the keys. SPEC §2 #5 explicitly accepts this trade-off in exchange for a 24/7 PCC sync loop. The compensating control is the audit log: every PCC decrypt is logged, so post-incident triage can identify what was read.

#### 1.1.3 — Powered on, unlocked, operator absent

**Capabilities.** As §1.1.2 but no coercion needed.

**Unmitigated impact.** Same as §1.1.2.

**Defenses.** Same. Plus: 30-minute idle timeout on the iron-session cookie (mitigates *passive* attacker-on-shoulder; does not mitigate the keys-in-process-memory issue).

**Residual risk: ACCEPTED.** Identical to §1.1.2. Operator hygiene (auto-lock on sleep, screen lock when stepping away) is the primary control.

---

### 1.2 Insider — one of the three operators acting maliciously

The three operators are not a uniform trust set:
- **Rory** has `role='owner'`, can run admin scripts, knows the master passphrase, has a passkey.
- **Tristan** and **Cade** are `role='member'`. Each has a passkey; both are PCC members.

#### 1.2.1 — Tristan or Cade reads Rory's personal data

**Capabilities.** Has their own valid session.

**Unmitigated impact.** Reads Rory's accounts, transactions, snapshots.

**Defenses.**
1. **Cryptographic siloing.** Rory's `wrappedUserDek` can only be unwrapped by the per-user KEK derived from Rory's passkey credential. Tristan / Cade do not have that credential. AAD on every personal item token is bound to `userId` — even if Tristan exfiltrates the encrypted token blob, decrypting it under his own DEK fails the GCM tag check.
2. **Repository scope-by-construction.** Every `Repository<T>` method takes a `RepoScope`. The `scope.kind === 'personal'` injects `WHERE userId = scope.userId` on every read. There is no admin override path in app code that bypasses this filter.
3. **No raw Prisma access** outside `lib/db/repositories/`. ESLint / code review catches violations.
4. **Indistinguishable 404.** Out-of-scope reads return `not_found`, not `unauthorized` — they cannot enumerate other users' resources by probing IDs.

**Failure modes.**
- A repo method that ignores its scope (bug). QA-PRIVACY's integration tests assert User-A-cannot-read-User-B for *every* repo method.
- Direct DB access (someone with shell on the host runs `sqlite3 greylock.db`). SQLCipher at-rest mitigates, but in-band attackers with FV unlocked can install their own decrypted client. Insider attack reduces to §1.1.2.

**Residual risk: LOW.** Cryptographic + repo-layer siloing means the only path is "compromise the host AND know the master passphrase" — which is §1.1.2.

#### 1.2.2 — Tristan or Cade tampers with PCC data

**Capabilities.** As above; PCC member.

**Unmitigated impact.** Modifies PCC accounts/transactions to misrepresent state.

**Defenses.**
1. **Audit log hash chain.** Every PCC mutation appends an entry; the hash chain is verified by `pnpm admin:audit-verify`. Tampering requires editing every subsequent entry, recomputing every hash. Detectable by the next verify run.
2. **No write-without-audit code path.** Every repo write that mutates PCC data emits an `AuditService.append`; QA-SEC traces this manually for every write.
3. **Plaid as ground truth.** Sync re-pulls from Plaid every 15 min; an in-DB tamper that does not match Plaid is detectable on next sync.

**Failure modes.**
- Insider edits the DB directly with the master passphrase, including the audit log, including hash chain rewrite. **This is detectable** if any party verified the chain head before the tamper (we keep `chainHead` returned by `keybridge ping` in operational logs as a trust anchor) — but pre-tamper external chain anchoring is out of scope for v0.1.
- Insider stops the sync worker and edits before next pull.

**Residual risk: MEDIUM.** Two PCC members other than the owner are full-trust on PCC data integrity. v0.1 does not implement external chain anchoring (publishing the chain head somewhere out-of-band). For v0.2 we may add periodic chain-head pinning.

#### 1.2.3 — Rory acting maliciously toward Tristan or Cade

**Capabilities.** Owner; can revoke passkeys, rotate the master passphrase, run any admin script.

**Unmitigated impact.** Cannot read Tristan's or Cade's personal data — owner has no path to unwrap their per-user DEK without their passkey credential. Can lock them out (revoke passkeys), can wipe PCC, can manipulate audit log entries (edit chain).

**Defenses.** Cryptographic asymmetry — owner role does NOT come with cryptographic privileges over personal data.

**Residual risk: ACCEPTED.** Owner trust is a v0.1 axiom. Tristan and Cade are accepting that the box owner (Rory) controls availability, not confidentiality, of their personal data.

---

### 1.3 Malicious npm dependency

#### 1.3.1 — Postinstall script

**Capabilities.** Code execution at `pnpm install` time, with the user's privileges.

**Unmitigated impact.** Anything the user can do. Read source, exfiltrate `.env`, install persistent daemons, modify `lib/crypto/*`.

**Defenses.**
1. **`pnpm.onlyBuiltDependencies` whitelist** in `package.json`. Only `@prisma/client`, `@prisma/engines`, `esbuild`, `prisma`, `sharp`, `unrs-resolver` may run install scripts. New packages with postinstall are blocked until explicitly approved.
2. **`pnpm-lock.yaml` integrity hashes.** All dependencies are pinned by content hash; `pnpm install --frozen-lockfile` in CI / repeatable installs.
3. **`pnpm audit --audit-level=high`** Phase 5 gate. Zero high/critical at tag time.
4. **Minimal dependency surface.** Direct deps in `package.json` are deliberately small: Next.js, React, Prisma, SimpleWebAuthn, iron-session, Plaid SDK, Zod. Each transitive subtree is reviewed by QA-SEC at Phase 5.

**Failure modes.**
- A whitelisted package's maintainer is compromised (Prisma, esbuild, sharp). High-impact, low-probability. We accept this and rely on community detection.
- A non-whitelisted dep is added without QA-SEC review.

**Residual risk: MEDIUM.** Supply-chain attacks on major packages are a known unsolvable for npm-class ecosystems. Mitigations are best-effort.

#### 1.3.2 — Runtime malicious code

**Capabilities.** Imported and executed inside the Node process. Same address space as `lib/crypto/*`.

**Unmitigated impact.** Reads Master KEK, PCC DEK, per-user DEKs straight out of `Buffer`s. Steals tokens. Persists itself.

**Defenses.**
1. Same as §1.3.1 — minimal surface, lockfile, audit gate.
2. **No `eval`, no `Function()`-from-string anywhere in `lib/*`.** `eslint-plugin-security` `detect-eval-with-expression`.
3. **No dynamic `import()` of untrusted paths.** Code review enforces.

**Failure modes.** Same as §1.3.1.

**Residual risk: MEDIUM, accepted with mitigations.**

---

### 1.4 Forensic disk recovery

**Capabilities.** Attacker has the disk image at rest (e.g. discarded SSD) and time. May or may not have the master passphrase.

**Unmitigated impact.** Read DB file, swap files, hibernation files, temp files. Recover deleted files (in case of imperfect zeroize).

**Defenses.**
1. **FileVault** (operator-managed) — disk-level.
2. **SQLCipher** — DB-level.
3. **No plaintext token persistence.** Anywhere. The `Item.encryptedAccessToken` is the only token storage; `lib/plaid/token-broker.ts` is the only place a plaintext token materializes, and it `Buffer.fill(0)`s immediately after each Plaid SDK call.
4. **`Buffer.fill(0)` discipline.** All key/token buffers are zeroized via a central `lib/crypto/zeroize.ts` helper. Tests assert that the buffer reads as zeros after release. Note: V8/libuv may copy buffers internally; we treat zeroize as best-effort, not guarantee.
5. **No swap-leak mitigation in app code** — relying on FV for at-rest swap protection.
6. **Audit log carries no token bytes.** `lib/audit/sanitizer.ts` rejects any payload matching token-shape patterns.

**Failure modes.**
- Hibernation file written before zeroize completes (unavoidable in user-space Node).
- V8 internal copies of token buffers we never zeroize.
- Filesystem snapshots / Time Machine backups carrying old DB states. **Mitigation:** `RUNBOOK.md` documents excluding `~/greylock/prisma/*.db*` from Time Machine.

**Residual risk: LOW for ciphertext at rest, MEDIUM for ephemeral memory artifacts in swap/hibernation.** The latter is unavoidable in any user-space app on macOS without root.

---

### 1.5 Malicious browser extension

**Capabilities.** Reads/writes DOM and JS context on `https://localhost:3000`.

**Unmitigated impact.** Inject a passkey-relay attack? Read in-page secrets? Exfiltrate dashboard data? Submit forged requests?

**Defenses.**
1. **WebAuthn binds to `rpId='localhost'` and `rpOrigin='https://localhost:3000'`.** A malicious extension cannot trigger an assertion that succeeds against a different origin. Browser enforces this.
2. **`SameSite=Strict; Secure; HttpOnly`** on the session cookie. JS can't read it.
3. **CSP (Phase 5)** — `default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; connect-src 'self'; frame-ancestors 'none'; ...` Extensions inject via the content-script API, which CSP cannot block at the page level. Browsers grant extensions special powers; Greylock cannot fully mitigate this.
4. **No third-party scripts.** No analytics, no fonts from CDN (IBM Plex Mono / Syne are bundled), no error-reporting SaaS. Lighthouse `script-src 'self'` is true.
5. **Audit-trace.** Every Plaid call audits with the `userAgent`; an unfamiliar extension fingerprint may show up in retro-analysis.

**Failure modes.**
- An extension with `tabs` or `webRequest` permission can effectively MITM the user. CSP doesn't block this. **Mitigation:** `RUNBOOK.md` recommends a clean browser profile or dedicated browser for Greylock; QA-SEC manually checks browser hardening once.

**Residual risk: MEDIUM.** Operator browser hygiene is the primary control. Greylock cannot force a user-side guarantee.

---

### 1.6 LAN attacker

**Capabilities.** On the same Wi-Fi as the laptop. May attempt to reach `localhost:3000` (no), spoof DNS, MITM the WebAuthn ceremony (no — passkey origin binding).

**Unmitigated impact.** None — `localhost` is not routable off-host. The attacker would need to be a local process on the same machine, which folds into §1.1.2 / §1.3.

**Defenses.**
1. **No external bind.** `next dev` listens only on localhost; the app surface is unreachable from the LAN. Confirmed by QA-SEC at Phase 5 with `lsof -iTCP:3000`.
2. **mkcert local CA** — the localhost cert is unique per machine; not exfiltrated to a CA. An attacker MITMing the DNS / network would not be able to forge it without access to the local CA root, which lives in the macOS Keychain.

**Residual risk: NEGLIGIBLE.**

---

### 1.7 Plaid backend compromise

**Capabilities.** Adversary inside Plaid: can issue arbitrary `transactions/sync` responses, can read tokens we issued.

**Unmitigated impact.** Greylock would faithfully ingest fraudulent transactions and reflect them in the dashboard. This is a data-integrity threat, not a confidentiality threat against Greylock itself.

**Defenses.**
1. **Sandbox first** for v0.1 — no real data exposed to a Plaid bug-impact window during development.
2. **Plaid TLS pinning** — out of scope for v0.1; the Plaid SDK uses standard CA roots. Browser-level TLS is the only layer.
3. **`/transactions/sync` is idempotent + cursor-driven** — a single bad response can be replayed with a previous cursor by `pnpm admin:rollback-sync` (planned, post-v0.1).

**Failure modes.** Greylock has no defense against a Plaid backend telling it lies. We accept this; the dashboard is for the operator's situational awareness, not for legal record-of-truth.

**Residual risk: ACCEPTED.** Greylock is downstream of Plaid by design.

---

## 2. Token traces

The following traces walk an entire credential / token lifecycle with every step mapped to a code module.

### 2.1 — PCC Plaid access token

```
[plaid.itemPublicTokenExchange]
        │  returns plaintext access_token (string in Plaid SDK return)
        ▼
[lib/plaid/token-broker.ts]                                    process memory only
        │  copies into Buffer.from(token,'utf8') = `tokenBuf`
        │  immediately calls:
        ▼
[CryptoService.encrypt({ handle:{kind:'pcc',version:V},
                          aad:{kind:'item_token', itemId},
                          domain:'pcc',
                          plaintext: tokenBuf })]              process memory
        │  → AAD = utf8("pcc:itemtoken:" + itemId + ":" + V)
        │  → AES-256-GCM(PccDEK, nonce=randomBytes(12), aad, tokenBuf)
        │  → blob = version || domain_tag(0x02) || nonce || ct || tag
        ▼
[ItemRepository.create({ encryptedAccessToken: blob })]         SQLCipher DB
        ▼                                                       file at rest
[token-broker.ts] runs `tokenBuf.fill(0)` and dereferences      memory cleared
                  (V8 may have copied — best effort)
```

To **use** the token (sync run):

```
[SyncOrchestrator] needs to call plaid.transactionsSync for itemId
        ▼
[lib/sync/keybridge-client.ts] requestDek({kind:'pcc'})        IPC over /tmp/...
        ▼
[KeybridgeServer] checks: PCC DEK loaded? Yes. Sends 32B.
        ▼
[lib/plaid/token-broker.ts] withDecryptedToken({itemId,...}):
        1. ItemRepository.readEncryptedToken(scope=admin, itemId) → blob
        2. CryptoService.decrypt({handle:'pcc',
                                  aad:{kind:'item_token', itemId},
                                  domain:'pcc',
                                  blob}) → tokenBuf
        3. await use(tokenBuf as PlaidAccessTokenInMemory)
            (this is the only place plaintext exists; lifetime = one Plaid call)
        4. tokenBuf.fill(0)
        ▼
[Audit] append plaid_token_decrypted (success), with itemId only — no token bytes
```

### 2.2 — Personal Plaid access token

Same as §2.1, with substitutions:
- `handle = {kind:'user', userId, version: U}`
- AAD prefix = `personal:itemtoken:`
- DEK source: per-user DEK, loaded only while session is active. If the user is logged out, `withDecryptedToken` returns `crypto_unavailable` and the sync skips.

### 2.3 — Passkey credential → per-user KEK → DEK unwrap

```
[browser] user taps Touch ID; @simplewebauthn/browser produces an assertion
        ▼ HTTPS
[POST /api/auth/authentication/complete]
        ▼
[AuthService.completeAuthentication]
        1. lookup Passkey row by credentialId
        2. verifyAuthenticationResponse — checks signature against credentialPublicKey
        3. assert newCounter > storedCounter (replay defense)
        ▼
[CryptoService.loadUserDek({ userId, credentialId, kekSalt, wrappedUserDek, version })]
        1. KEK = HKDF(IKM = credentialId || CRYPTO_PEPPER,
                      salt = kekSalt,
                      info = utf8("greylock/userKek/v1/" + userId),
                      L=32)
        2. AES-256-GCM(KEK).open(wrappedUserDek,
                                 aad=utf8("personal:userdek:" + userId)) → dek32B
        3. cryptoState.userDeks[userId] = dek32B (in memory, non-exported)
        ▼
[Audit] per_user_dek_derived (success)
```

The `credentialId` used in step 1 is the one returned by the browser in the assertion. **Note:** `credentialId` is public — the secret is the authenticator's private key, which signs the assertion. We use `credentialId` as the KEK IKM because (a) it is unique per credential and (b) only a holder of the *physical authenticator* can produce a successful assertion that proves the *current login* is bound to that credential. An attacker with `credentialId` alone (e.g. read from the DB) cannot produce a valid assertion → cannot trigger this code path. The assertion is the gate; `credentialId` is the lookup key for the right KEK derivation.

### 2.4 — Master passphrase → Master KEK → PCC DEK

```
[lib/runtime/boot.ts] (process start)
        ▼
[lib/crypto/master-key.ts] readFromKeychain('greylock-master')
        ▼ (security find-generic-password -s greylock-master -a $USER -w)
        passphrase: utf8 string in process memory
        ▼
[query DB for active PccKeyWrap row] → { kdfSalt, kdfN, kdfR, kdfP, version, wrappedDek }
        ▼
[scrypt(passphrase || CRYPTO_PEPPER_BYTES, kdfSalt, N, r, p, dkLen=32)]
        → MasterKEK (32 bytes, in memory)
        ▼
[passphrase Buffer.fill(0)]                                    plaintext gone
        ▼
[AES-256-GCM(MasterKEK).open(wrappedDek,
                              aad = utf8("pcc:dekwrap:v" + version))]
        → PccDEK (32 bytes, in memory)
        ▼
[derive keybridge HMAC key]
        K_kb = HKDF(MasterKEK, info = utf8("greylock/keybridge/v1"), L=32)
        ▼
[Audit] master_kek_loaded (success), pcc_dek_unwrapped (success)
```

On shutdown / `pnpm admin:revoke-all`, all four buffers (`MasterKEK`, `PccDEK`, `K_kb`, any per-user DEKs) are `Buffer.fill(0)`'d.

---

## 3. Decisions — explicit accepted trade-offs

These are choices the model exposes. Each is a decision, not a bug.

| ID | Decision | Why we accepted it | What changes if we reverse it |
|----|----------|--------------------|--------------------------------|
| D-1 | PCC DEK lives in process memory continuously | Enables 24/7 PCC sync without operator interaction | PCC sync only runs when Rory is logged in; staler PCC dashboard |
| D-2 | Single passkey per user in v0.1 | Simpler enrollment; no multi-device key handoff to design | Lose-laptop = `pnpm admin:revoke-all` and re-enroll; for v0.2 we may add multi-passkey |
| D-3 | Localhost-only, no remote access | Eliminates ~95% of network attack surface | Without this we'd need a VPN, CSRF tokens, much stronger per-request auth |
| D-4 | No password fallback | A second credential mechanism is a second attack surface | Operator hygiene + admin re-enroll is the only recovery path; documented in `RUNBOOK.md` |
| D-5 | Master passphrase from macOS Keychain | Operator UX (no TTY prompt at boot) | Key lives only as long as the macOS login session is unlocked; matches `mkcert -install` trust model |
| D-6 | Audit log not externally anchored | v0.1 simplicity | An insider with master passphrase + DB access can rewrite history without external detection; v0.2 candidate |
| D-7 | Plaid backend trusted | We are downstream of Plaid by design | Defending requires independent ground-truth source, out of scope |
| D-8 | Best-effort `Buffer.fill(0)` | V8/libuv may copy buffers internally | A native binding for `mlock`/`memzero` would harden this; not in v0.1 |
| D-9 | Owner can lock out members | Owner role asymmetry by intent | Distributed multi-party admin (m-of-n) deferred; v0.2+ |
| D-10 | Indistinguishable 404 for out-of-scope reads | Prevents resource enumeration | Slightly noisier debugging; QA-PRIVACY tests verify this |
| D-11 | Plaid Link script loaded from `https://cdn.plaid.com` (the only third-party origin) | Plaid Link UX requires their hosted bundle; self-hosting is brittle across Plaid version changes | A compromise of Plaid's CDN can inject script into the `/connect` page only. CSP `script-src 'self' https://cdn.plaid.com` minimizes the surface. Bank balances themselves are unaffected (they're computed server-side from already-encrypted Plaid token responses). |
| D-12 | Master-passphrase rotation is deferred to v0.2 (`admin-rotate-master` is a stub in v0.1) | Full rotation requires multi-step Keychain + DB orchestration with atomic rollback that needs proper integration testing | If a rotation is forced (lost laptop with passphrase known, etc.), the v0.1 mitigation is "shred the DB and rebuild from Plaid" per `RUNBOOK.md` §4. Plaid is the source of truth; the local DB is reconstructible. |

---

## 4. Defenses by layer

```
+---------------------------------------------------------------+
|  Browser                                                       |
|  - WebAuthn origin binding (rpId='localhost', rpOrigin=        |
|    'https://localhost:3000')                                   |
|  - SameSite=Strict; Secure; HttpOnly cookie                    |
|  - CSP (Phase 5: default-src 'self', no inline)                |
|  - No third-party scripts, no analytics                        |
+----------------+----------------------------------------------+
                 │ HTTPS via mkcert local CA
+----------------v----------------------------------------------+
|  Next.js web process                                          |
|  - Zod validation at every route                              |
|  - iron-session (signed+encrypted; 30m idle / 8h absolute)    |
|  - Single-session-per-user enforcement                        |
|  - Repository scope-by-construction (no admin bypass in app)  |
|  - Rate limit on auth, plaid-link, sync, healthz              |
|  - Hardening headers (HSTS, X-Frame DENY, etc.)               |
+----------------+----------------------------------------------+
                 │ in-memory keys
+----------------v----------------------------------------------+
|  CryptoService                                                |
|  - AES-256-GCM (12B nonce, 16B tag)                            |
|  - AAD bound to (domain, row identity, key version)            |
|  - HKDF-SHA-256 derivations with versioned info strings        |
|  - scrypt (N=2^17, r=8, p=1) for passphrase                    |
|  - Buffer.fill(0) on all key/token buffers post-use            |
+----------------+----------------------------------------------+
                 │ Prisma queries
+----------------v----------------------------------------------+
|  SQLCipher database (file)                                    |
|  - Page-level AES-256                                         |
|  - Key derived from master passphrase                          |
|  - Encrypted Plaid access tokens; no plaintext column         |
|  - Hash-chained AuditLogEntry                                 |
+----------------+----------------------------------------------+
                 │ macOS filesystem
+----------------v----------------------------------------------+
|  FileVault (operator-managed; runbook-gated)                  |
+---------------------------------------------------------------+
```

Each layer is independently sufficient against a different attacker. Compromise requires defeating multiple layers.

---

## 5. What is NOT defended (explicit list)

We list these to make it impossible for a future commit to accidentally drift the model.

1. **Compromise of the host while running and unlocked.** Anyone who can run code as the user can read all keys. § 1.1.2.
2. **Operator coercion.** Touch ID under duress.
3. **macOS Keychain compromise.** Whatever can read the Keychain item can derive the Master KEK.
4. **Plaid backend integrity.** Greylock trusts Plaid's responses.
5. **Forensic recovery from swap / hibernation.** Best-effort only.
6. **Malicious browser extension on the operator's browser.** Out-of-scope at the page level.
7. **Maintainer compromise of a whitelisted npm dependency.** Best-effort detection.
8. **Side-channel attacks on local crypto** (timing, cache, RowHammer, etc.). Out of scope.
9. **External anchoring of the audit chain.** v0.1 leaves chain integrity verifiable only locally.
10. **Multi-device account portability.** v0.1 has one passkey per user.
11. **Compromise of Plaid's CDN delivering the Link script.** Mitigated by CSP `script-src 'self' https://cdn.plaid.com` (cannot escape that page). Bank balances are server-computed from already-encrypted tokens, so a CDN compromise cannot exfiltrate balances directly.
12. **Master-passphrase rotation in v0.1.** Deferred to v0.2 — see D-12. v0.1 lost-laptop response is documented in `RUNBOOK.md` §4.

---

## 6. Verification ownership

| Defense | Verified by |
|---|---|
| AAD prevents cross-domain ciphertext use | QA-SEC + AGENT-CRYPTO unit tests (round-trip + tampered-AAD rejection) |
| Repository scope on every read | QA-PRIVACY integration tests, every method, every kind |
| No plaintext token persisted | QA-SEC manual code trace, `grep -ri "access_token"` audit |
| Hash chain unbroken | `pnpm admin:audit-verify`; QA-SEC on test fixtures + tamper-injection |
| Sanitizer rejects token-shape payloads | AGENT-AUDIT-LOG unit tests |
| Single-session enforcement | AGENT-AUTH integration tests |
| Counter monotonicity | AGENT-AUTH unit + e2e |
| Headers correct | QA-SEC `curl -I` checklist at Phase 5 |
| `lsof` shows localhost-only bind | QA-SEC at Phase 5 |
| `pnpm audit` clean | Phase 5 gate |

End of model.
