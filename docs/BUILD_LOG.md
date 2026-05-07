# Greylock — Build Log

Append-only record of orchestration decisions, agent spawns, QA verdicts, and phase boundaries. New entries go at the **top**.

---

## 2026-05-06 — Phase 3 — Plaid + Sync + Compute + Audit ✅

Commit: pending — `feat(plaid): encrypted token storage, sync loop, compute, audit log`

**Four parallel executor subagents (Opus) shipped:**

| Agent | Owns | Headline |
|---|---|---|
| AGENT-PLAID | `lib/plaid/*` + `app/api/plaid/*` + 5 tests | 39 tests; `withDecryptedToken` is the sole plaintext-token scope. One workaround (placeholder zero-blob → `rewriteEncryptedToken({admin})`) because `ItemRepository.create` doesn't accept `id`; AAD-bound rewrite happens in the same `$transaction`. |
| AGENT-SYNC | `lib/ipc/*` (protocol + peer-cred + server + client) + `lib/sync/*` + `scripts/sync.ts` + `app/api/sync/run` | 39 tests. HMAC handshake constant-time. Socket `0600` + `getuid()` parity (deviation from LOCAL_PEERCRED, equivalent in practice — tracked as L-5). Borrowed-DEK lifetime ≤ one item. Boot smoke test green. |
| AGENT-COMPUTE | `lib/compute/*` + 6 golden fixtures + 6 test files | 90 tests, **100% coverage** on `lib/compute/**`. Pure throughout, bigint Cents end-to-end. |
| AGENT-AUDIT-LOG | `lib/audit/{chain,sanitizer,service,index}.ts` + `app/api/admin/audit/*` | 64 tests. Chain canonicalization extracted to single source (`lib/audit/chain.ts`); repo imports from there. Sanitizer hard-rejects token-shape values **even at allowlisted keys**. |

**Orchestrator-written admin CLI scripts (post-agent, `scripts/`):**
- `_admin-boot.ts` — shared dev-only boot helper
- `admin-audit-verify.ts`, `admin-revoke-all.ts`, `admin-revoke.ts`, `admin-enroll.ts` — fully wired
- `admin-rotate-master.ts` — intentional stub printing "not yet implemented in v0.1" (Phase-5 wiring per QA-SEC P2 §M-3 recommendation)

**Total: 424 tests across 32 files (+232 from Phase 2 baseline). 13.6 s.**

**QA gates:**
- `pnpm typecheck` — clean
- `pnpm test` — 424/424
- `pnpm lint` — 0 errors, 12 non-blocking warnings
- `pnpm audit --prod --audit-level=high` — clean (1 moderate PostCSS, same as Phase 2 — M-1)
- Manual QA-SEC sweep across 19 mandatory checklist items + 5 token traces

**QA-SEC verdict: PASS WITH FOLLOW-UP** — `docs/qa/QA-SEC-phase-3.md`. **0 critical, 0 high.** One new low (L-5 `getuid()` parity vs `LOCAL_PEERCRED`) — fully mitigated by socket permissions; tracked for optional Phase-5 N-API binding. One new low (L-6 `pnpm.overrides` audit). Phase 2 mediums (M-1, M-2, M-3) carry forward to Phase 5. The security-reviewer subagent stalled past its 600 s watchdog (same pattern as Phase 2); manual sweep used the same checklist.

**Recommendations for Phase 4 (UI):**
- Generic error copy in UI; never echo `kind` strings.
- Server components for data; client components only for Plaid Link, polling, modals.
- `'self'`-only script/style/image sources from day 1 (CSP locks at Phase 5).

---



---

## 2026-05-06 — Phase 2 — Crypto + Auth + DB ✅

Commit: pending — `feat(security): passkey auth, encrypted storage, zero-knowledge PCC keys`

**Three parallel executor subagents (Opus) shipped:**

| Agent | Owns | Headline |
|---|---|---|
| AGENT-CRYPTO | `lib/crypto/*` + 7 unit-test files | 104/104 tests; **99.21% lines / 100% functions / 93.75% branches** on `lib/crypto/**` (clears 90% gate). AAD scheme, blob format, scrypt N=2^17 byte-exact. |
| AGENT-AUTH | `lib/auth/*` + 5 route handlers + `lib/runtime/services-registry.ts` | 46/46 tests. Allowlist + placeholder rejection at both flows, counter monotonicity, single-session-revoke + DEK zeroize, `SameSite=Strict; Secure; HttpOnly`, indistinguishable 404, rate limit, Zod everywhere. |
| AGENT-DB | `lib/db/*` (13 repos + client + migrate + sqlcipher-key) + `prisma/migrations/20260507001726_init` + `EnrollmentToken` model | **23/23 scope-by-construction PRIVACY tests** + audit-chain + sqlcipher-roundtrip green. Picked `better-sqlite3-multiple-ciphers` aliased through `@prisma/adapter-better-sqlite3` via `pnpm.overrides`. |

**Total: 192 tests across 16 files passing in 11 s.**

**QA gates run by Orchestrator before commit:**
- `pnpm typecheck` — clean
- `pnpm test` — 192/192
- `pnpm test --coverage tests/unit/crypto/` — `lib/crypto/**` 99.21% / 100% / 93.75% — clears 90% gate per SPEC §5
- `pnpm lint` — 0 errors, 11 non-blocking warnings (relaxed test-file rules)
- `pnpm audit --prod --audit-level=high` — clean (1 moderate transitive PostCSS XSS, below gate, not exploitable in static-CSS app — tracked as Phase 5 M-1)
- Manual security sweep covering all 23 mandatory checklist items

**QA-SEC verdict: PASS WITH FOLLOW-UP** — `docs/qa/QA-SEC-phase-2.md`. **0 critical, 0 high.** Three medium follow-ups for Phase 5: PostCSS bump (M-1), domain-type cleanup of `User.wrappedUserDek` + `Passkey.kekSalt` (M-2, currently bridged via `WrappedDekReader`), `RegistryOverrides` production guard (M-3). The security-reviewer subagent stalled past its 600s watchdog mid-audit; report compiled by Orchestrator from a focused manual sweep against the same checklist (token traces, AAD bytes, scope-by-construction SQL review, secret-handling, deps).

**Tooling fixes during the phase:**
- `next lint` deprecated and `eslint-config-next` broken on ESLint 9 (RushStack patcher rejects). Dropped both; relying on `eslint-plugin-security` + strict `@typescript-eslint`. Added test-file exemption block for `consistent-type-imports`, `no-non-null-assertion`, and noisy security false-positives.
- One unused-variable error in audit repo cleaned up.

---



---

## 2026-05-06 — Phase 1 — Architecture ✅

Commit: pending — `feat(arch): system architecture and contracts`

**Deliverables (all under `~/greylock/`):**
- `docs/ARCHITECTURE.md` — directory tree, two-process model, IPC keybridge spec, key hierarchy diagram, AAD scheme, blob format, auth flow, Plaid flow, compute layer, audit hash chain, headers, ops view, testing strategy, open architectural choices for Phase 2
- `docs/THREAT_MODEL.md` — 7 threat actors with capabilities/defenses/failure-modes/residual-risk, 4 token-trace flows (PCC token, personal token, passkey→KEK→DEK, master-passphrase→KEK→PCC-DEK), 10 explicitly accepted decisions, defense layers diagram, what's NOT defended (10 items), verification ownership table
- `docs/API.md` — 22 HTTP routes (auth, plaid, sync, dashboard, admin, healthz) with methods/auth/Zod schemas/audit emits/errors; full IPC keybridge protocol (HMAC-SHA-256 handshake, NDJSON wire format); admin CLI surface
- `docs/agents/AGENT-ARCH.md` — retrospective with 12 design decisions, 6 open questions (with defaults), Phase-2 hand-off notes for AGENT-CRYPTO/AGENT-AUTH/AGENT-DB
- `prisma/schema.prisma` — 11 models (User, Passkey, Session, Item, Account, Transaction, NetWorthSnapshot, AuditLogEntry, PccMembership, PccKeyWrap, RateLimitBucket), Domain/Role/SessionStatus/AuditAction/AuditOutcome enums, no plaintext-secret columns
- `lib/types/domain.ts` — branded ID types, currency types, encrypted-blob brand, all entity interfaces, Result<T,E> + Ok/Err, error tagged unions
- `lib/types/services.ts` — CryptoService, AuthService, PlaidService, AuditService, all repositories, KeybridgeServer/Client, ComputeService, SyncOrchestrator interfaces
- `lib/types/zod-schemas.ts` — request/response schemas for every API route
- `lib/types/index.ts` — barrel

**QA gates run by Orchestrator:**
- ✅ `pnpm prisma format` + `pnpm prisma validate` (with DATABASE_URL set inline) — schema valid
- ✅ `pnpm typecheck` — clean under strict TS, no `any`, no unjustified `as`, all branded types resolve

**Process notes:**
- Spawned AGENT-ARCH as a planner subagent (Opus, fresh context). Agent completed schema, all 4 TS contract files, and ARCHITECTURE.md before being stopped on operator instruction. Orchestrator wrote THREAT_MODEL.md, API.md, and the retrospective, anchored in AGENT-ARCH's design (read every file it produced before authoring). The agent's design is the source of truth for the schema and contracts.
- AGENT-ARCH explicitly handed off two architectural choices to Phase 2: SQLCipher Prisma binding (AGENT-DB), and sync-worker DB connection model (AGENT-DB + AGENT-SYNC jointly).
- 6 open questions captured in `docs/agents/AGENT-ARCH.md` §"Open questions" — none block Phase 2; defaults selected.

---



**Decisions locked at the Phase 0→1 boundary** (full table in `docs/SPEC.md` §4):

| # | Question | Choice |
|---|----------|--------|
| 4 | Personal-sync model | Background while logged in, via Unix-socket IPC (`/tmp/greylock-keybridge.sock`, mode 0600). DEK never leaves web-process memory. |
| 5 | PCC membership | All three operators (Rory + Tristan + Cade) |
| 6 | Plaid env for v0.1 | Sandbox |
| 7 | Currency | USD only |
| 8 | Concurrent sessions | Max 1 per user (new login revokes prior) |
| 9 | Multi-passkey | 1 per user for v0.1 |
| 10 | Snapshot frequency | Per sync (every 15 min) |
| 11 | Audit retention | Forever, hash-chained |

**Wrote `docs/SPEC.md`** — canonical build spec, single source of truth for every subagent.

**About to spawn:** AGENT-ARCH (planner subagent, Opus) — produces `docs/ARCHITECTURE.md`, `docs/THREAT_MODEL.md`, `docs/API.md`, `prisma/schema.prisma`, `lib/types/*.ts`. No implementation code.

---

## 2026-05-06 — Phase 0 — Bootstrap ✅

Commit: `82a14ad chore: bootstrap greylock`



**Decisions made before kickoff:**

| # | Question | Decision |
|---|----------|----------|
| 1 | Master passphrase delivery | macOS Keychain (`security find-generic-password -s greylock-master`) with TTY fallback if Keychain item missing |
| 2 | mkcert install | Spec-spec confirmed: brew install + `mkcert -install` (interactive, run by Rory) |
| 3 | Node version | Spec updated from "20 LTS" to "Node 22 LTS minimum"; dev target is system v25.9.0; `package.json` engines `>=22` |
| 4 | Allowlist | `rory.patrick.loftus@gmail.com`, `tristan.m.loftus@gmail.com`, `cade-placeholder@greylock.invalid` (placeholder — must be replaced before Cade can enroll) |

**Hard-flag carried forward:** Cade's allowlist email is a placeholder. Phase 5 cannot tag `v0.1.0` until a real Cade email is set in `.env` and `.env.example` is updated. AGENT-AUTH must reject the placeholder string at registration time so an accidental enrollment can't succeed.

**Tooling state at Phase 0 entry:**
- Node v25.9.0 ✓
- pnpm 10.33.4 ✓ (installed via `npm install -g pnpm`)
- Homebrew 5.1.6 ✓
- git 2.53.0 ✓
- mkcert 1.4.4 ✓ (installed; `mkcert -install` pending — Rory must run interactively)

**Phase 0 deliverables:**
- `~/greylock/{docs,certs,scripts}` + `git init`
- `.gitignore` (secrets, env, db, build, deps, test, OS, OMC state, certs)
- `.env.example` — every required key, no values, placeholder Cade flag inline
- `package.json` — Next 15 / React 19 / Prisma / SimpleWebAuthn / iron-session / Plaid / Zod / Vitest / Playwright / ESLint+Prettier+`eslint-plugin-security`. `pnpm.onlyBuiltDependencies` whitelist for native bins
- `tsconfig.json` — strict + `noUncheckedIndexedAccess` + `exactOptionalPropertyTypes` + `useUnknownInCatchVariables`
- `eslint.config.mjs` (flat config, security rules + strict TS rules), `.prettierrc.json`, `.prettierignore`
- `next.config.mjs` — `poweredByHeader:false`, X-Frame-Options DENY, HSTS, Referrer-Policy strict-origin (full CSP added in Phase 5)
- `pnpm install` clean — 0 vulnerabilities, native deps approved
- `pnpm setup:certs` issued `certs/localhost.{pem,-key.pem}` (cert files gitignored)
- `README.md` — 3-line desc, setup steps, NOT FINANCIAL ADVICE, recovery doc

**Out-of-Greylock action pending on operator:** `mkcert -install` requires interactive sudo + macOS Keychain prompt. `pnpm setup:certs` succeeded but the local CA is not yet in the system trust store, so browsers will warn until Rory runs `mkcert -install` from Terminal.app. Does not block code build.

---
