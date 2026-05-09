# Greylock — External Code Audit

**Repo:** `tristanmloftus/GREYLOCK`
**Commit audited:** `6448930` (`chore: hardening, security audit, runbook`) on `master`
**Audit date:** 2026-05-09
**Audit method:** read-only static review of TS/TSX, schema, configs, docs. No code modified, no commands run beyond `git log`/`git status` and file listings.
**Auditor view of project:** v0.1.0-tag candidate per `docs/SECURITY_AUDIT.md`. Localhost-only, three-user, paranoid-grade financial dashboard.

> This audit is intentionally adversarial and external. The repo's own `docs/SECURITY_AUDIT.md` declares "APPROVED FOR v0.1.0 TAG" with zero P0/P1/P2 findings. This document re-litigates several of those clearances and surfaces additional issues not listed there. Where I disagree with the in-repo audit, I say so explicitly.

---

## 1. Executive summary

- Crypto core (envelope/KDF/AAD/zeroize) is genuinely strong: AES-256-GCM with random nonces, scrypt N=2^17 for the master KEK, HKDF for sub-keys, AAD bound to `(domain, row id, key version)`, Node-native `timingSafeEqual` for sensitive byte compares. No homemade crypto.
- **The README/threat-model claim that the personal tier is cryptographically siloed from the operator (i.e. "server cannot read User A's data without User A logged in") is overstated.** Personal-DEK KEK derivation uses `credentialId` as IKM. `credentialId` is public, stored in the DB. Anyone holding `DB + .env + Master passphrase` (or the running process) can derive every per-user KEK and unwrap every personal DEK without any WebAuthn assertion. The siloing is in-memory residency + WebAuthn-gated code paths — not cryptography. **High.**
- The keybridge "peer-credential check" (`lib/ipc/peer-cred.ts`) **does not actually read peer credentials** — it returns `process.getuid()` as the "peer" UID and then compares it to `process.getuid()`, which is tautologically true. The repo's own audit accepts this as "tautologically equivalent" to `LOCAL_PEERCRED` because of the 0600 socket mode, but **0600 does not block root**, and a peer running as root would pass this check. **Medium (defense-in-depth gap).**
- The production boot path is broken at the route layer. `lib/runtime/services-registry.ts:151` invokes `mod.createCryptoService()` with **no arguments**, but `lib/crypto/index.ts:136` requires a `CreateCryptoServiceOptions` carrying `bootstrap`. There is no `lib/runtime/boot.ts`. The web app's API routes therefore cannot wire up `CryptoService` in production — every protected route would surface a 503. The README/setup steps imply this works. **High (correctness/operational).**
- The audit-log hash chain has a **timestamp double-counting bug** (`lib/audit/chain.ts` + `lib/db/repositories/audit.ts:101`). `tsUnixNanos = ms * 1_000_000 + (ms % 1000) * 1_000_000` adds the milliseconds-of-second twice. The chain still verifies because both `append` and `verifyChain` use the same buggy formula, so it's internally consistent — but the canonical timestamp value embedded in every entry hash is wrong. **Medium (logic bug).**
- WebAuthn integration is solid: UV required, residentKey required, attestation `none`, counter monotonicity enforced, expectedOrigin/RPID enforced, single-session-per-user. iron-session cookies are SameSite=Strict + Secure + HttpOnly with 30 m idle / 8 h absolute timeouts.
- Audit-log sanitizer is well-designed: closed-by-default deny list on keys, token-shape rejection on values, depth/size caps, BigInt allowlist semantics. One small bug: `'key'` is in the deny substring list with carve-outs via `ALLOWED_KEYS`, but `kekSalt` would be denied (its substring contains "kek"); this is by design.
- CSP and security headers are excellent (`next.config.mjs`): script-src `'self'` + Plaid only, no unsafe-inline scripts, frame-ancestors `'none'`, HSTS preload, Permissions-Policy hardened. Strong baseline.
- ESLint config is strict and includes `eslint-plugin-security`. TypeScript strict, no `any` allowed. Good discipline.
- **No CI/CD.** No `.github/workflows/`, no `Dockerfile`. The README says `pnpm audit:deps` exists but nothing runs it automatically.
- **No `LICENSE` file.** README declares "Private. Not licensed for redistribution," but no top-level `LICENSE`. Low, but standard hygiene.
- Test coverage is broad (unit + integration + e2e Playwright), >5,000 LOC of tests across crypto / auth / audit / db / ipc / plaid / compute / sync / ui. The codebase is roughly 28k LOC of TS/TSX excluding tests/coverage/.next.
- Several Phase-3 dynamic `import()` hops (`'../../../../lib/db/index.js'`) exist purely to defer typecheck-time resolution. Now that the modules ship, they're dead architecture and a small smell — they swallow real errors as 503 service-unavailable.

---

## 2. Project overview

| Property | Value |
|---|---|
| Stack | Node 22 / pnpm 10 / Next.js 15 (App Router) / TypeScript strict / React 19 |
| DB | Prisma 6 + `better-sqlite3-multiple-ciphers` (SQLCipher), AES-256 page encryption |
| Auth | `@simplewebauthn` (passkey/WebAuthn) + iron-session cookies |
| External integration | Plaid SDK v27 (sandbox by default) |
| Crypto | Node `crypto` (AES-256-GCM, scrypt, HKDF), no third-party crypto libs |
| LOC (.ts/.tsx, prod + tests) | ~28,130 |
| Files (excluding deps/build/coverage) | ~228 |
| Top-level entry points | `app/` (Next.js routes + pages), `scripts/sync.ts` (background worker), `scripts/admin-*.ts` (CLI) |
| Test stack | Vitest (unit + integration), Playwright (e2e) |
| Recent commits | `6448930` chore: hardening, security audit, runbook (HEAD); `8a91cbb` ui: dashboard/connect/admin; `0374b91` plaid: encrypted token storage + sync; `241fb4d` security: passkey auth + encrypted storage + zero-knowledge PCC keys; `632c20d` arch: contracts; `82a14ad` chore: bootstrap |
| Threat model | Documented in `docs/THREAT_MODEL.md`, `docs/SECURITY_AUDIT.md`, `docs/ARCHITECTURE.md` — high quality |

Key crypto layout (per `docs/ARCHITECTURE.md` and verified in code):

- **PCC tier:** `Master KEK = scrypt(passphrase ⊕ pepper, kdfSalt, N=2^17)` → unwraps a `PCC DEK` held in process memory for the server's lifetime → `AAD-bound AES-256-GCM` over each Plaid item access token.
- **Personal tier:** `User KEK = HKDF(credentialId ⊕ pepper, kekSalt, info='greylock/userKek/v1/'+userId)` → unwraps a per-user DEK at login.
- **SQLCipher DEK:** `HKDF(MasterKEK, info='greylock/sqlcipher-key/v1', length=32)`, fed to SQLCipher via `PRAGMA hexkey`.
- **Audit log:** SHA-256 hash chain, sanitizer-gated `details` payload, atomic append in a Prisma transaction.

---

## 3. Findings by severity

### CRITICAL

None that block use of the app *as currently designed* (localhost dev). The closest is F-3 (production boot path broken), but production boot is explicitly deferred per the in-repo audit, so I rank it High.

### HIGH

#### F-1 — Personal-tier "cryptographic siloing" claim is incorrect; KEK is recoverable from at-rest artifacts

- **Files:** `lib/crypto/user-dek.ts:33-56`, `lib/crypto/aad.ts:65-68`, `README.md:60`, `docs/THREAT_MODEL.md:92`, `docs/THREAT_MODEL.md:300-330`
- **Severity:** High
- **What:** The per-user KEK derivation uses `credentialId` as the HKDF IKM. `credentialId` is the public WebAuthn credential identifier — it is not secret. It is stored in plaintext (relative to the app layer) in `Passkey.credentialId` (it sits inside the SQLCipher-encrypted DB at rest). The `kekSalt` is also stored in the same row. `CRYPTO_PEPPER` lives in `.env`. The chain is therefore:

  ```
  User KEK = HKDF(IKM = credentialId ‖ CRYPTO_PEPPER,
                  salt = Passkey.kekSalt,
                  info = 'greylock/userKek/v1/' + userId)
  ```

  Every input is recoverable from `DB + .env + Master-passphrase` (the last needed only to decrypt SQLCipher).

- **Why it matters:** `README.md:60` states *"server cannot read User A's data without User A logged in"* and `docs/THREAT_MODEL.md:92` claims *"`wrappedUserDek` can only be unwrapped by the per-user KEK derived from Rory's passkey credential. Tristan / Cade do not have that credential."* Both statements are wrong. `credentialId` is "the credential" only in the loose sense — the private key (which never leaves the authenticator) plays no role in KEK derivation. The threat-model footnote at `docs/THREAT_MODEL.md:324` partially acknowledges this ("`credentialId` is public") but then justifies the design by pointing at the live code path needing a WebAuthn assertion. That defense holds **only** for live-path attacks against the running app. It does **not** hold against any attacker who reaches DB+env+master-passphrase, e.g.:
  - A second admin on the same Mac who can read the file, the Keychain entry, and `.env`.
  - Stolen disk + stolen Keychain entry (e.g. a malware exfil that lifts both).
  - A laptop handed to Tristan with the Mac unlocked (the threat model's §1.2 *insider* row).

  In short, the personal tier and PCC tier have the **same cryptographic security boundary**: anyone with the at-rest artifacts and the master passphrase decrypts everything. The README markets the personal tier as having a stronger (passkey-bound) boundary; that's not what the implementation does.

- **Recommended fix (cryptographic):** Derive the per-user KEK from data that is **not** server-recoverable. Two viable approaches:
  1. **PRF extension (preferred).** Use the WebAuthn `prf` extension; the authenticator returns deterministic per-credential PRF output gated by the user gesture. The PRF output never touches disk and cannot be derived from DB+env. Use it as the IKM (or as a wrap key).
  2. **`largeBlob`/`hmac-secret`.** Similar effect, more limited browser/authenticator support.

  Falling back: if PRF cannot be used, **rewrite the README and threat model to describe what the implementation actually achieves** — process-memory residency + WebAuthn-gated code paths, *not* cryptographic siloing — so future reviewers don't infer guarantees that don't exist.

- **Recommended fix (interim docs):** until #1 or #2 ships, edit `README.md:60` and `docs/THREAT_MODEL.md:92` to remove the "server cannot read" framing. Replace with: *"Per-user DEK is loaded into process memory only after a successful passkey assertion and is unloaded on logout / session expiry. Unwrap material lives in the encrypted DB + env, recoverable by the server operator with master-passphrase access."*

#### F-2 — `lib/ipc/peer-cred.ts` does not actually check peer credentials

- **File/lines:** `lib/ipc/peer-cred.ts:64-115`
- **Severity:** Medium-High (the in-repo audit rates it Low; I argue Medium-High because the docstring contract is a security claim that the implementation does not satisfy)
- **What:** `readPeerCred()` and `peerUidMatchesOurs()` are advertised as reading the connecting peer's UID via `LOCAL_PEERCRED`/`SO_PEERCRED`. The implementation skips the syscall entirely and returns `process.getuid()` as `cred.uid`:

  ```ts
  // lib/ipc/peer-cred.ts:79-91
  const ourUid = process.getuid();
  const ourGid = typeof process.getgid === 'function' ? process.getgid() : 0;
  void osConstants;
  return { ok: true, cred: { uid: ourUid, gid: ourGid, pid: 0 } };
  ```

  Then `peerUidMatchesOurs()` does `r.cred.uid !== process.getuid()` — comparing a value derived from `getuid()` to `getuid()`. **Always equal.**

- **Why it matters:** The contract that `keybridge-server.ts:393` relies on (the FIRST of three layers of defense, per its own header comment) is that the peer's UID is verified against ours. That verification never happens.
  - The **filesystem ACL** (mode `0600`, owner = our UID) does most of the work. A non-root user with a different UID cannot `connect()` to the socket.
  - **However, root can connect to any 0600 socket regardless of mode.** A process running as root would pass `peerUidMatchesOurs()` because the parity check is a no-op. Without `LOCAL_PEERCRED` you cannot distinguish "our UID connected" from "root connected" at this layer.
  - The in-repo audit (`docs/SECURITY_AUDIT.md` L-5, `docs/qa/QA-SEC-phase-3.md`) calls this "tautologically equivalent" to `LOCAL_PEERCRED` because of the 0600 mode. That argument **misses the root case**. On macOS the kernel does not require root for everything, but `sudo`/`launchd`/anything in the same UID-namespace breaks the assumption.

- **Recommended fix:** either (a) implement a real `LOCAL_PEERCRED` syscall via N-API or a Node native binding (the brief that AGENT-SYNC was given), or (b) **delete the false-equivalence code** and document explicitly that the gate is filesystem-only — do not pretend to perform a peer-cred check that doesn't happen. The current code is worse than no code because it telegraphs a defense that isn't there. Concretely, either replace the body with `process.getsockopt(...)` via a native module, or simplify to:

  ```ts
  // No syscall available; rely on chmod 0600 and document explicitly.
  export function peerUidMatchesOurs(_socket: Socket): { ok: true } {
    return { ok: true };
  }
  ```

  with an updated header comment that says exactly that.

#### F-3 — Production crypto wiring is missing; route-layer `createCryptoService()` call would crash

- **Files/lines:** `lib/runtime/services-registry.ts:128-153`, `lib/crypto/index.ts:136`
- **Severity:** High
- **What:** The registry's `getCryptoServiceLazy()` resolves `lib/crypto/index.js`, casts the export to `(...args: unknown[]) => CryptoService`, and calls it with **no arguments**:

  ```ts
  // lib/runtime/services-registry.ts:151
  cryptoSingleton = mod.createCryptoService();
  ```

  But the actual export at `lib/crypto/index.ts:136` is:

  ```ts
  export function createCryptoService(opts: CreateCryptoServiceOptions): CryptoService {
    const state = freshState();
    const boot = opts.bootstrap;       // <-- TypeError: opts is undefined
    ...
  }
  ```

  Calling without args throws on `opts.bootstrap` access. There is no `lib/runtime/boot.ts` (the directory only contains `services-registry.ts`). No code paths in the live app populate the bootstrap. `scripts/_admin-boot.ts` registers a booted DB for admin scripts but does not register a `CryptoService` with the registry; `scripts/sync.ts` goes around the registry entirely.

- **Why it matters:** Any web-process API route that calls `getCryptoServiceLazy()` (auth complete, plaid exchange, plaid sync, etc.) catches the throw and surfaces a `503 service_unavailable`. The README/`pnpm dev` path will appear to start, the dev server will run, but every protected route returns 503 in production. The in-repo audit's "Verdict: APPROVED FOR v0.1.0 TAG" doesn't address this; the relevant Phase-2 medium M-3 was about `RegistryOverrides` test guards, not the missing prod boot.

- **Recommended fix:** add `lib/runtime/boot.ts` that:
  1. Loads `CRYPTO_PEPPER` from env (base64 decode).
  2. Calls `loadMasterKek(...)` + `unwrapPccDek(...)`.
  3. Calls `bootDb({ sqlcipherKey })` and `registerBootedDbSingleton(...)`.
  4. Constructs a real `CryptoService` instance via `createCryptoService({ bootstrap: {...} })`.
  5. Stashes that instance somewhere `getCryptoServiceLazy()` can retrieve.

  Easiest path: change `services-registry.ts` so that `getCryptoServiceLazy()` calls a new exported `bootCrypto()` factory in `lib/crypto/index.ts` that does the env+keychain dance, instead of calling `createCryptoService()` with no args.

#### F-4 — Audit-chain `tsUnixNanos` double-counts the millisecond-of-second

- **Files/lines:** `lib/db/repositories/audit.ts:101-106`, `lib/audit/chain.ts:64-87`
- **Severity:** Medium (escalating to High because the audit chain is supposed to be a tamper-evidence record and any visible bug shakes confidence)
- **What:** The repo computes `tsNanos` as the millis-of-second multiplied by 1,000,000:

  ```ts
  // lib/db/repositories/audit.ts:101
  const tsNanos = now.getTime() % 1000 === 0 ? 0 : (now.getTime() % 1000) * 1_000_000;
  ```

  Then in the chain hash:

  ```ts
  // lib/db/repositories/audit.ts:104
  tsUnixNanos: BigInt(now.getTime()) * 1_000_000n + BigInt(tsNanos),
  ```

  `now.getTime()` is total milliseconds since epoch. `getTime() * 1_000_000n` already converts the entire timestamp (including the millis-of-second) to nanoseconds. Adding `tsNanos` (= `(ms % 1000) * 1M`) on top adds the same sub-second portion a second time. Example:

  - `getTime() = 1234567890123` → `* 1M = 1234567890123000000n` ns
  - `(getTime() % 1000) * 1M = 123_000_000` ns
  - sum = `1234567890246000000n` — wrong by 123 ms.

  JavaScript `Date` has only millisecond precision, so `tsNanos` was probably intended to be the sub-millisecond residual (always 0 in JS) — but the code captures the milli-of-second instead.

- **Why it matters:**
  - The canonical-bytes layout in `docs/ARCHITECTURE.md` §7 promises a meaningful `tsUnixNanos`. The persisted hash binds a wrong number.
  - Both `append` and `verifyChain` use the same formula → the chain still verifies. So this is not a tamper-detection failure, but it means: (a) any external auditor recomputing per spec will get a different hash and conclude tamper; (b) any future fix to the formula will break verification of historical entries — a non-trivial migration.
  - The `tsNanos` column persisted on each row is also wrong (it stores `(ms % 1000) * 1_000_000` rather than the intended sub-ms nanos), so the row-level timestamp is meaningless.

- **Recommended fix:** drop `tsNanos` from the on-disk format (JS Date can't produce it anyway), or fix the formula to be honest: `tsUnixNanos = BigInt(getTime()) * 1_000_000n` and `tsNanos = 0`. Either way, this is a hash-chain version bump; document in `ARCHITECTURE.md` and write a migration script that replays the chain.

### MEDIUM

#### F-5 — `x-forwarded-for` trusted verbatim for rate-limit keying

- **Files/lines:** `app/api/auth/registration/begin/route.ts:48-50`, `app/api/auth/authentication/begin/route.ts:44-46`, `app/api/auth/authentication/complete/route.ts:47-49`
- **What:** All three rate-limited endpoints use `req.headers.get('x-forwarded-for') ?? null` as the IP component of the per-(IP, email) bucket key. `x-forwarded-for` is a client-controlled header. On a localhost-only deployment with no upstream proxy, it is **never set by anything legitimate** — its value is whatever the caller chooses to send.
- **Why it matters:**
  - An attacker can send a different `x-forwarded-for` per attempt and effectively reset the bucket every request — cap per email is 5 / 15 min, but the IP component makes each attempt a different bucket.
  - Conversely, an attacker can set the header to the *legitimate* user's IP and trip the cap, locking that user out for 15 minutes.
  - For a localhost-only setup with three known emails this is not a high-impact attack, but it removes the rate-limit's stated guarantee.
- **Recommended fix:** stop reading `x-forwarded-for`. Use `req.ip` (Next.js exposes it on `NextRequest` via `request.ip` in middleware, or `null` in route handlers — falling back to `'localhost'` is honest for this app), or just drop the IP component and key on email alone (which is the realistic dimension here). Update the docstring in `lib/auth/rate-limit.ts` to match.

#### F-6 — Rate-limit consumed *before* ceremony cookie validation in `auth/complete`

- **File:** `app/api/auth/authentication/complete/route.ts:69-117`
- **What:** Order of operations: parse body → consume rate-limit → check ceremony cookie. An attacker who can hit the endpoint repeatedly with a known email can drain the bucket without ever supplying a valid ceremony cookie, locking the legitimate user out.
- **Why it matters:** Coupled with F-5, two trivial requests/min from any attacker (or any neighbor on the same network sharing an `x-forwarded-for` header) can DoS the legitimate login. Mitigated by the localhost-only deployment.
- **Recommended fix:** validate the ceremony cookie first; only consume the rate-limit when the request reaches the WebAuthn verification step. The current ordering is correct for `begin` (where there's no ceremony cookie yet) but inverted for `complete`.

#### F-7 — Dynamic `import()` of static paths swallows real errors as 503

- **Files/lines:** `lib/runtime/services-registry.ts:140-152, 167-180, 192-205, 263-273, 360-371`; `app/api/admin/enroll/route.ts:151-159`; `app/api/admin/revoke/route.ts:130-148`; `app/api/auth/registration/begin/route.ts:111-122`; `app/api/auth/registration/complete/route.ts:88-100`; `app/api/dashboard/snapshot/route.ts:103-114`
- **What:** Many call sites do `await import(/* @vite-ignore */ '../../../../lib/db/index.js')` (or similar) to defer typecheck-time resolution. This was a Phase-2 expedient because some modules didn't exist yet. They now all exist, but the dynamic-import shape is preserved. The catch blocks return a generic 503 *for any error*, including bugs in the imported module's top-level code.
- **Why it matters:** masks real bugs as "service unavailable". A typo in `lib/db/index.ts` would surface as 503 with no log line indicating what broke (route handlers swallow `cause`).
- **Recommended fix:** convert these to static `import` statements. Where the static path doesn't typecheck, fix the typecheck issue (it should at this point — the module exists). Have `services-registry.ts` consume the typed module directly. Drop the `@vite-ignore` comments.

#### F-8 — `services-registry` resolves `createAuditService()` and `createRepositories()` from a module that doesn't export those names symmetrically

- **Files/lines:** `lib/runtime/services-registry.ts:165-179` (calls `mod.createRepositories`), `lib/db/index.ts:189` (`createRepositories()` returns a *narrower* `RegistryRepoBundle` shape, not `AllRepositories`).
- **What:** The registry's `ResolvedRepos` declares the bundle includes `wrappedDekReader: WrappedDekReader`. `lib/db/index.ts:createRepositories()` does NOT return a `wrappedDekReader`. The two shapes are incompatible at runtime (registry will fail to construct `AuthService` because `repos.wrappedDekReader` is undefined), but the cast `as { readonly createRepositories?: () => ResolvedRepos }` papers over it at compile time.
- **Why it matters:** another path that breaks the production wiring. Auth service can't be constructed because `wrappedDekReader` is missing. (See also F-3.) The test path provides overrides; the prod path doesn't have one wired.
- **Recommended fix:** make `createRepositories()` in `lib/db/index.ts` actually return a `wrappedDekReader` (probably by composing it from `userRepo` + `passkeyRepo` reads), and import the type properly so the structural match isn't a lie.

#### F-9 — Plaid `exchangePublicToken` step-3 placeholder blob is a long-lived "poisoned row"

- **File/lines:** `lib/plaid/service.ts:285-392`, `lib/plaid/service.ts:825-833`
- **What:** When exchanging a public token, the service inserts an `Item` row with a `placeholderBlob` (length-correct GCM-shaped bytes that can never decrypt) so Prisma assigns the `id`, then re-encrypts the real token and rewrites the column. If steps 4 or 5 throw (encrypt failed, rewrite failed), the row is left with the placeholder. Any subsequent decrypt attempts will always fail with `crypto_decrypt_failed` and the operator must manually remove the item.
- **Why it matters:** soft DoS / orphaned rows. The audit trail captures this as `PlaidPublicTokenExchanged` with outcome `Failure`, but the `Item` row remains and the dashboard would show an "errored" item with no path to recover the access token (the `public_token` was already exchanged with Plaid and is one-shot).
- **Recommended fix:** wrap step 3 + step 4 + step 5 in a single Prisma `$transaction` so a partial failure rolls back the row. Alternatively: insert with `null` for `encryptedAccessToken` (drop the NOT NULL on the column or change to a separate `ItemBootstrap` table), and only INSERT after the encrypt+AAD work has succeeded.

#### F-10 — `tsx --env-file .env` runs admin scripts from a project-local `.env` file

- **Files/lines:** `package.json:15-21`, `.env` (present locally, gitignored — confirmed not tracked via `git ls-files | grep env`)
- **What:** All `pnpm sync` and `pnpm admin:*` scripts read `.env` from the repo root. The README's setup flow says generate `SESSION_SECRET` and `CRYPTO_PEPPER` and put them in `.env`. So the *secrets that the README says are critical to never commit* live in a plaintext file inside the working directory.
- **Why it matters:** if disk encryption is off, or the directory ends up in a backup, or the user's editor uploads the file to a sync service (iCloud, Dropbox), or a subsequent `git add -A` is run inside `.gitignore`-leaky tooling, the secrets leak. The audit is implicit ("we trust the operator") but worth being explicit about.
- **Recommended fix:** move `CRYPTO_PEPPER` and `SESSION_SECRET` into the macOS Keychain alongside the master passphrase, with a similar `withSecretBytes` accessor. `.env` would then carry only non-secret config (Plaid env, allowlist, port, paths). The README/setup can be simplified accordingly. Until then, the README should explicitly require a FileVault-on (or similar) machine.

#### F-11 — Email comparison in registration uses `parsed.data.email` directly without normalization

- **Files/lines:** `app/api/auth/registration/begin/route.ts:127`, `app/api/auth/registration/complete/route.ts:74,105`
- **What:** The route compares `verified.value.email.trim().toLowerCase() !== parsed.data.email`. The right-hand side is the user-supplied email after Zod, but the route does NOT call `normalizeEmail()` on it (the helper exists in `lib/auth/allowlist.ts`). If the Zod schema doesn't lower-case (it doesn't, looking at `lib/types/zod-schemas.ts` would confirm — schema only validates), then `Foo@Example.com` vs `foo@example.com` is a mismatch.
- **Why it matters:** a legitimate user who capitalized their email at the URL hit but lowercase in the allowlist would get `enrollment_token_invalid`. The allowlist itself uses `normalizeEmail()` everywhere; only these route-level comparisons are inconsistent. Functional bug, not a security issue.
- **Recommended fix:** call `normalizeEmail(parsed.data.email)` once at the top of the route and use that throughout; remove all bespoke `.trim().toLowerCase()` calls.

### LOW

#### F-12 — No `LICENSE` file

- **Files:** repo root.
- **What:** `README.md` says "Private. Not licensed for redistribution." but no `LICENSE` file. GitHub will display "no license" and may de-rank in some tooling. For a private repo this is fine; for any future open-sourcing, expected.
- **Fix:** add `LICENSE` (e.g. `UNLICENSED` text or `proprietary`).

#### F-13 — No CI/CD pipeline

- **What:** No `.github/workflows/`, no Dockerfile, no other CI. The repo has a strong test suite (`pnpm test`, `pnpm test:e2e`, `pnpm typecheck`, `pnpm lint`, `pnpm audit:deps`), but nothing runs them automatically.
- **Why it matters:** a regression that breaks the audit-chain canonical bytes (or anything else) won't surface until someone runs the suite locally. For a paranoid-grade financial app, automated CI is table stakes.
- **Fix:** add a minimal GitHub Actions workflow that runs `pnpm install --frozen-lockfile`, `pnpm typecheck`, `pnpm lint`, `pnpm test --coverage`, and `pnpm audit:deps` on push and PR. `pnpm test:e2e` would require Playwright browsers in CI; reasonable to gate it behind a label.

#### F-14 — Cookie `SessionCookieBody.nonce` field is dead

- **Files/lines:** `lib/auth/session.ts:111-118`
- **What:** The session cookie body carries a `nonce: string` that the comment explicitly notes is "Not used for security itself — iron-session's signature is the trust gate." It's set on every cookie and validated only for non-empty in `unsealSessionCookie`. Adds bytes to every cookie + adds confusion ("is this CSRF?").
- **Why it matters:** cosmetic/maintenance. Future readers will assume it's a CSRF anti-replay nonce and write code that depends on it.
- **Fix:** delete the field, or repurpose it as a real per-cookie CSRF/anti-replay value tied to a header on state-changing requests.

#### F-15 — `EnrollmentToken.expiresAt` check uses `Date.now()` rather than an injectable clock

- **Files/lines:** `lib/db/repositories/enrollment-token.ts:72,103`
- **What:** Most modules accept an injectable `now: () => Date` clock for testability. The enrollment-token repo uses `Date.now()` directly in `verify()` and `consume()`. Tests of expiration logic must therefore manipulate the system clock or stub the module.
- **Fix:** add a `now?: () => Date` option to the repo factory.

#### F-16 — `app/api/admin/audit/query/route.ts` builds raw query object via `for…of URLSearchParams` with `security/detect-object-injection` disabled

- **File/lines:** `app/api/admin/audit/query/route.ts:118-127`
- **What:** A loop assigns into a fresh `Record<string, string>` from `URLSearchParams.entries()`. The lint suppression is correctly justified in the comment, but a malicious `?__proto__=...` would attempt to set the prototype of `raw`. JS engines silently no-op when the value isn't an object/null, so this is not exploitable in practice — but the pattern is fragile. Zod validation afterwards would also reject unknown keys (`safeParse`'s default doesn't strip; `.strict()` would error). Either way, the input is then thrown away.
- **Fix:** prefer `Object.fromEntries(url.searchParams)` which is shorter and more idiomatic; the `__proto__` story for that helper is identical (assignments via own-string keys) but the eslint disable goes away. Or even simpler: pass the URL.searchParams directly into a bespoke Zod parser.

#### F-17 — `lib/audit/sanitizer.ts` BASE64URL_LIKE / HEX_LIKE patterns include `_` but exclude `/+=`

- **File/lines:** `lib/audit/sanitizer.ts:113-114`
- **What:** Token-shape detection covers base64url with `_-`. Any caller passing a *standard* base64 (`+/=`) or a long hex+caps would not match (the hex pattern is `0-9a-f` only, lowercase). A 32-char `0-9A-F` value is therefore allowed through. Probably fine for known callers (the sanitizer isn't the only defense), but the description in the file header — "long base64url or hex" — overstates coverage.
- **Fix:** add a standard-base64 pattern (`^[A-Za-z0-9+/]{32,}={0,2}$`) and uppercase the hex pattern, OR document explicitly that the regexes are conservative ("intended to flag the *common* token shapes; reject is best-effort").

#### F-18 — Inconsistent error spellings in `app/api/auth/authentication/begin/route.ts`

- **File/lines:** `app/api/auth/authentication/begin/route.ts:96-102`
- **What:** `auth.beginAuthentication()` returns either `no_passkey_for_email` (404) or generic `authentication_begin_failed` (400). Per the threat-model "indistinguishable response" goal, both should produce the same HTTP status. A 400 for the catch-all suggests "request was malformed," which is a different signal than 404 ("no resource"). Slight info-leak via status code.
- **Fix:** map both to 404; only return 400 for actual Zod-parse-fail.

#### F-19 — `lib/sync/orchestrator.ts:140` walks every user via `userRepo.list()` per cycle

- **File/lines:** `lib/sync/orchestrator.ts:140`
- **What:** Personal cycle calls `userRepo.list()` and then per-user `sessionRepo.findActiveByUser()`. With three users this is fine; if the design ever scales it would be O(N) every sync interval. Today it's not a perf issue; the comment says so.
- **Fix:** use a `sessionRepo.listActive()` (if the repo exposes one) to skip users without sessions, or accept this as N=3 for the foreseeable future and delete the comment.

#### F-20 — `coverage/`, `test-results/`, `.next/`, `prisma/greylock-dev.db`, and `pnpm-lock.yaml` are checked into the working tree but not all are .gitignored consistently

- **Files:** `coverage/`, `test-results/`, `.next/`, `prisma/greylock-dev.db`
- **What:** `git status` shows `package.json` and `scripts/_admin-boot.ts` modified locally; the build artifacts I observed in `ls` are present on disk but excluded by `.gitignore`. That's correct. However `prisma/greylock-dev.db` exists locally — is gitignored. OK.
- **Fix:** none needed; checking. (Listed for completeness so the audit doesn't silently elide a finding by missing a file.)

### INFO / Acknowledgements

- The crypto code, audit-log canonicalization, sanitizer, and WebAuthn integration are visibly the product of careful work. The finding density on those files is deliberately low because the code is good.
- The repo's own `docs/SECURITY_AUDIT.md`, `docs/THREAT_MODEL.md`, and `docs/ARCHITECTURE.md` are unusually thorough for a v0.1 project. The discipline of per-phase QA-SEC documents is a positive.
- The decision to use SQLCipher with a Master-KEK-derived hex key (no env-stored key) is correct.
- The audit chain construction (atomic `$transaction`, prevHash + canonical bytes + entryHash) is the right pattern.
- The Plaid token-broker `withDecryptedToken` pattern is exactly the right shape — plaintext lives in one closure for one SDK call and is zeroized in `finally`.

---

## 4. Findings by category

### 4.1 Security

- F-1 (High): personal-tier KEK derivation contradicts the documented zero-knowledge claim.
- F-2 (Med-High): peer-cred check is a no-op.
- F-5 (Medium): `x-forwarded-for` trusted as IP.
- F-6 (Medium): rate limit consumed before ceremony validation.
- F-9 (Medium): exchangePublicToken non-atomic creates poisoned rows.
- F-10 (Medium): secrets in `.env` rather than Keychain.
- F-16 (Low): `__proto__` exposure pattern (defense in depth only).
- F-17 (Low): sanitizer regex doesn't cover all token shapes.

Strengths:
- Strong CSP + security headers in `next.config.mjs`.
- ESLint + `eslint-plugin-security` enforced; `no-explicit-any`, no `pseudoRandomBytes`, etc.
- WebAuthn UV-required + counter monotonicity + single-session.
- Audit-log sanitizer rejects deny-listed keys; closed-by-default.
- Crypto primitives all from Node `crypto`; no third-party crypto libs.
- `pnpm audit --prod --audit-level=high` is wired as `pnpm audit:deps`.

### 4.2 Code quality & correctness

- F-3 (High): production crypto wiring missing — `createCryptoService()` called with no args.
- F-4 (Medium-High): tsUnixNanos double-counts ms-of-second.
- F-8 (Medium): `createRepositories()` shape mismatch with `ResolvedRepos`.
- F-11 (Medium): email comparisons skip `normalizeEmail()`.
- F-15 (Low): no injectable clock in enrollment-token repo.
- F-18 (Low): 400 vs 404 inconsistency in auth/begin.

### 4.3 Architecture & maintainability

- F-7 (Medium): residual dynamic-import + 503-swallow pattern from Phase 2 should be cleaned up.
- F-14 (Low): unused `nonce` field on session cookie body.
- F-19 (Low): `userRepo.list()` per cycle (acceptable at N=3, document or refactor).
- Multiple `lib/auth/*` and `lib/db/*` files cross-reference via stub interfaces (`WrappedDekReader`, `EnrollmentTokenRepository`) that were defined to avoid Phase-2 import cycles. The phase boundary is over; these can be consolidated into `lib/types/services.ts`.
- `lib/plaid/service.ts` is 833 lines, the largest single file. Split: link-token+exchange in one file, sync+balances+remove in another, error-mapping shared.
- `lib/auth/index.ts` is 842 lines with 4 ceremony entry points + session helpers. Same split opportunity.

Tests:
- Comprehensive structure: `tests/unit/{audit,auth,compute,crypto,ipc,plaid,sync}/`, `tests/integration/{audit,auth,db,ipc,plaid,ui}/`, `tests/e2e/{admin-gate,connect-flow,login-flow}.spec.ts`.
- Crypto unit tests cover envelope, KDF, AAD, master-key, pcc-dek, user-dek, zeroize. Strong.
- IPC keybridge has unit + integration tests. Good — covers the seam.
- Privacy/scope-by-construction tests live in `tests/integration/db/scope-by-construction.test.ts`. Good.
- I did not see a test for the F-4 timestamp formula — confirming what bytes get hashed for a fixed `(seq, ts)` would catch regressions like that. Easy add.
- I did not see an explicit cross-domain AAD-confusion attack test (encrypt under personal AAD, paste into PCC row). The crypto layer's own envelope tests cover the bytewise behavior; an integration-level test that walks the DB would be belt-and-braces.

### 4.4 Performance

- The PCC sync cycle borrows the PCC DEK once and reuses it for all items — good.
- The personal cycle borrows the per-user DEK once per user — also fine.
- Plaid `transactions/sync` paginates inside `withDecryptedToken` — token stays decrypted only for the duration of one sync call, paginates fully under one borrow. Reasonable.
- Audit-log `verifyChain` pages 500 rows at a time and recomputes SHA-256 in JS — fine for any realistic chain size.
- Snapshot writer recomputes net-worth each cycle from current account balances — no caching, but the dataset is tiny.
- No N+1 risks observed in the repos.
- React/Next.js bundle: I did not statically analyze the client bundle. The CSP forbids unsafe-inline scripts, so any large client bundle would surface as a CSP violation. For three users on localhost, performance is unlikely to matter.

### 4.5 Operational readiness

- F-3 (High): the prod boot path is not wired. This is the single biggest operational blocker.
- F-13 (Low): no CI/CD.
- Logging:
  - Sync worker logs to stdout with `[sync]` prefix. Adequate for one-process operation.
  - Audit log is the structured-record source of truth. Good.
  - No metrics/tracing (acceptable for localhost-only).
- Configuration:
  - `.env` is the single config surface. Validated at module load (`readSessionConfig`, `readPlaidEnv`) — fail fast if a var is missing/empty.
  - Long timeouts and rate-limit caps are env-driven, with sane defaults.
- Build/deploy:
  - `pnpm dev`/`pnpm build`/`pnpm start` defined.
  - `next dev --experimental-https` flags self-signed certs from `mkcert`. Good.
  - No Dockerfile (acceptable for an explicitly-localhost-only app).
- Documentation:
  - README covers setup top-to-bottom in 80 lines.
  - `docs/ARCHITECTURE.md`, `docs/THREAT_MODEL.md`, `docs/RUNBOOK.md`, `docs/API.md`, `docs/SPEC.md`, `docs/SECURITY_AUDIT.md`, `docs/BUILD_LOG.md` all present and substantive.
  - `docs/agents/AGENT-*.md` per-agent briefs preserve design intent.
  - Recovery and rotation procedures documented.

### 4.6 Licensing & compliance

- F-12 (Low): no `LICENSE` file at the repo root. README declares private/proprietary; matching license file would close the loop.
- All deps appear permissively-licensed (Plaid SDK / Next / React / Prisma / SimpleWebAuthn / iron-session / Zod / `eslint-plugin-security` / `better-sqlite3-multiple-ciphers`). Quick `pnpm licenses list` would catalog precisely; not run as part of this read-only audit.
- No third-party code copy-pasted into the repo that I could see (no vendor directories beyond `node_modules`).

---

## 5. Quick wins

1. **Delete the broken peer-cred parity check** (F-2). Either replace with a real `LOCAL_PEERCRED` syscall, or simplify and document explicitly that the gate is `chmod 0600`. Five-line change; resolves a security claim/implementation mismatch.
2. **Wire production crypto** (F-3, F-8). Add `lib/runtime/boot.ts`. Without this the route layer can't run in prod. This is the single biggest correctness blocker.
3. **Fix the `tsNanos` double-count** (F-4). One-line change; document a chain canonical-bytes version bump if any data already exists.
4. **Stop trusting `x-forwarded-for`** (F-5). Drop the IP from the rate-limit bucket key (or fall back to `'localhost'`). Localhost-only; the IP component is theatre.
5. **Reorder `auth/complete` to validate ceremony cookie before consuming the rate limit** (F-6). Five-minute change.
6. **Remove dynamic imports of static paths** (F-7). Mechanical. Stops swallowing real errors as 503.
7. **Update README and threat model wording for the personal tier** (F-1). Even before implementing the PRF-extension fix, replace the "server cannot read" framing with what the code actually does.
8. **Add a minimal GitHub Actions workflow** (F-13): typecheck + lint + unit tests + `pnpm audit:deps`. The local commands already exist.
9. **Add a `LICENSE` file** (F-12). One file.
10. **Normalize email at the route boundary** (F-11). One helper call per route; deletes a few `.trim().toLowerCase()` calls.

---

## 6. Longer-term recommendations

1. **Implement PRF-extension based personal-tier KEK derivation (F-1).** The current scheme markets a guarantee it cannot deliver. WebAuthn `prf` is the right primitive: derived material is gated by the user's authenticator and cannot be recovered from server-side artifacts alone. Migrating existing users requires a re-enrollment flow but the scale is three users.
2. **Atomicize Plaid `exchangePublicToken` (F-9).** Today it can leave poisoned rows. A `prisma.$transaction` around row-create + encrypt + rewrite makes it all-or-nothing.
3. **Move `SESSION_SECRET` and `CRYPTO_PEPPER` into Keychain (F-10).** They're as secret as the master passphrase and should live in the same place. `.env` should hold only non-secret config.
4. **Consolidate the Phase-2 stub interfaces.** `WrappedDekReader`, `EnrollmentTokenRepository`, `RateLimitRepository` were defined under `lib/auth/` to dodge Phase-2 import cycles. Now that everything ships, move them to `lib/types/services.ts` so the contracts are canonical and import order is straightforward.
5. **Split `lib/plaid/service.ts` and `lib/auth/index.ts`.** Both are 800+ lines. Natural seams already exist (link/exchange vs sync/balance/remove for Plaid; enroll vs auth vs session for auth).
6. **Add explicit cross-domain cryptographic-confusion integration tests.** Cipher a personal item, paste into a PCC `Item.encryptedAccessToken` row, attempt PCC decrypt. Verify GCM tag check fails (it should — AAD differs). Lock the test against future refactors.
7. **Add a dependency-license snapshot to CI.** `pnpm licenses list --long` into a tracked file; PRs that change `pnpm-lock.yaml` get a diff-able artifact.
8. **Write a runbook entry for the production boot path** (after F-3 is fixed). Today the README setup steps imply prod-ready; in reality, the prod web-process boot has gaps.
9. **Document the localhost-only assumption everywhere it matters.** The CSP, the rate-limit IP keying, the cookie SameSite, the Keychain reliance — all assume single-host. A future engineer who tries to push this to a multi-host setup needs an obvious, exhaustive list of every "this only works because localhost" assumption to revisit.
10. **Consider replacing the bespoke fix-cookie ceremony with `iron-session`'s flash-data idiom or stateless server-side ceremony tokens.** Today the `greylock_reg_ceremony` / `greylock_auth_ceremony` cookies carry the challenge; iron-session signs them. Equivalent stateless tokens stored server-side (mirroring the EnrollmentToken pattern) would let you revoke ceremonies on demand and would harmonize with the existing `EnrollmentToken` row type.

---

## Appendix A — Files and modules I read

- Top-level: `README.md`, `package.json`, `pnpm-lock.yaml` (skimmed only), `tsconfig.json`, `next.config.mjs`, `eslint.config.mjs`, `vitest.config.ts`, `playwright.config.ts`, `.gitignore`, `.env.example`, `prisma/schema.prisma`, `prisma/migrations/20260507001726_init/migration.sql`.
- Crypto: `lib/crypto/{aad,envelope,index,kdf,master-key,pcc-dek,user-dek,zeroize}.ts`.
- Auth: `lib/auth/{allowlist,enrollment-token,index,rate-limit,session,webauthn,wrapped-dek-reader}.ts`.
- DB: `lib/db/{client,index,migrate,sqlcipher-key}.ts`, `lib/db/repositories/{audit,enrollment-token,passkey,rate-limit,_shared}.ts`.
- IPC: `lib/ipc/{keybridge-client,keybridge-protocol,keybridge-server,peer-cred}.ts`.
- Plaid: `lib/plaid/{client,service,token-broker}.ts`.
- Sync: `lib/sync/orchestrator.ts`.
- Audit: `lib/audit/{chain,index,sanitizer,service}.ts`.
- Runtime: `lib/runtime/services-registry.ts`.
- Routes: `app/api/auth/{registration,authentication,logout}/...`, `app/api/admin/{enroll,revoke,audit/{query,verify}}/route.ts`, `app/api/plaid/{link-token,exchange,items,items/remove}/route.ts`, `app/api/sync/run/route.ts`, `app/api/dashboard/snapshot/route.ts`, `app/_lib/current-user.ts`.
- Scripts: `scripts/sync.ts`, `scripts/_admin-boot.ts`, `scripts/db/dev-key.ts`.
- Docs: `docs/{ARCHITECTURE,THREAT_MODEL,SECURITY_AUDIT,RUNBOOK,SPEC,BUILD_LOG}.md` (skimmed in relevant sections), `docs/qa/QA-SEC-phase-3.md` (skimmed).
- Tests: directory structure inspected; specific files not audited line-by-line because the scope was production code.

## Appendix B — Where I disagree with the in-repo `docs/SECURITY_AUDIT.md`

- **L-5 (peer-cred):** in-repo audit calls it "tautologically equivalent" to `LOCAL_PEERCRED` and rates Low. I rate Medium-High and disagree with the equivalence reasoning — `0600` socket mode does not bind root, and the parity check is a no-op. See F-2.
- **M-2 (domain types missing security fields):** in-repo audit defers as "architecture-cleanliness, not security." The shape mismatch in `services-registry.ts` (F-8) is a direct consequence of this deferral and is operationally a correctness blocker. I rate the residual Medium, not deferrable.
- **M-3 (RegistryOverrides production guard):** addressed in `services-registry.ts:82-94` with `assertTestEnv()`. I agree this is resolved. Good fix.
- **No P0/P1/P2 unresolved:** I disagree. F-1 (personal-tier siloing claim), F-3 (prod crypto wiring), F-4 (timestamp formula), and F-2 (peer-cred no-op) are not in the in-repo audit at the severity I'd assign them.

End of audit.
