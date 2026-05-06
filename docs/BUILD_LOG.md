# Greylock — Build Log

Append-only record of orchestration decisions, agent spawns, QA verdicts, and phase boundaries. New entries go at the **top**.

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
