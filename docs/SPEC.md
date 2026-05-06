# Greylock — Canonical Build Spec

This is the immutable source of truth for what we are building and what is non-negotiable. Every subagent must read this file before producing any deliverable. If something below is ambiguous, **stop and ask the Orchestrator** — do not invent.

---

## 1. What Greylock is

A private, paranoid-grade financial operating dashboard for Platinum Creek Capital (PCC) and its operators. Three users only: Rory (owner), Tristan, Cade. Localhost-only, passkey-authenticated, two-tier encryption — passkey-derived keys for personal data, server-held key for PCC (so the 15-min sync loop runs 24/7).

## 2. Hard constraints (violations = STOP and ask Orchestrator)

1. **Build location:** `~/greylock/` only. **Never** read, write, or git-touch anything inside `~/Desktop/Rory/` or `/Volumes/PersonalOS/`.
2. **No secrets in code or git.** All keys (Plaid client_id, Plaid secret, session secret, encryption pepper) live in `.env`. `.env` is gitignored from commit zero. `.env.example` is the only env file in git.
3. **No production deploy code.** Localhost only. No Vercel configs, no Dockerfiles for prod, no cloud SDKs. `next dev` and `next start` against `localhost:3000` with HTTPS via mkcert.
4. **No password fallback. Ever.** Passkey-only. If WebAuthn fails, the user retries — there is no email magic link, no password reset, no recovery email loop. Account recovery = Rory manually re-registers a passkey from the admin CLI.
5. **Two-tier encryption is non-negotiable.**
   - **Personal tokens (per-user):** encrypted with a per-user KEK derived from that user's passkey credential; decryptable only during an authenticated session for that user. Server cannot read User A's personal data without User A logged in.
   - **PCC tokens (shared):** encrypted with a server-held PCC key. The PCC key itself is encrypted at rest with a key derived from the master passphrase fetched from macOS Keychain at server start. This key is loaded into process memory at boot and used by the 15-min sync loop to refresh PCC data 24/7. Trade-off accepted: anyone with the running server's process memory + the master passphrase can read PCC tokens. Mitigated by: localhost-only deploy, encrypted disk, audit log on every PCC decrypt, automatic key rotation on master-passphrase rotation.
6. **Failed QA → reject and respawn the agent.** Orchestrator does not patch failing work itself.

## 3. Stack (locked)

- **Runtime:** Node 22+ (dev: 25.9.0); pnpm 10.x
- **Framework:** Next.js 15 (App Router) + TypeScript strict mode
- **DB:** SQLite via Prisma, with **SQLCipher** for at-rest encryption
- **Auth:** `@simplewebauthn/server` + `@simplewebauthn/browser` (passkey / WebAuthn)
- **Session:** `iron-session` (encrypted, signed cookies; **30-min idle timeout, 8-hr absolute max**)
- **Plaid:** `plaid` npm SDK
- **Crypto:** Node `crypto` only (AES-256-GCM, scrypt, HKDF). **No homemade crypto, no npm crypto-helper packages.**
- **Validation:** Zod on every API boundary (request and response)
- **HTTPS local:** mkcert-issued cert
- **Testing:** Vitest (unit) + Playwright (e2e)
- **Linting:** ESLint flat config + Prettier + TypeScript strict + `eslint-plugin-security`

**No Tailwind** unless an agent justifies it in writing. Default to CSS Modules + the OVRWCH terminal aesthetic (dark `#0e0f11` bg, IBM Plex Mono + Syne fonts, green/red/amber/blue accents).

## 4. Locked decisions (Phase 0 → 1 boundary)

| # | Decision | Choice | Notes |
|---|----------|--------|-------|
| 1 | Master passphrase delivery | macOS Keychain (`security find-generic-password -s greylock-master`) with TTY fallback | Helper module in `lib/crypto/master-key.ts` |
| 2 | Allowlist | `rory.patrick.loftus@gmail.com`, `tristan.m.loftus@gmail.com`, `cade-placeholder@greylock.invalid` | Cade's address is a **placeholder** — auth must reject the literal `cade-placeholder@greylock.invalid` string at registration time. Real Cade email TBD before v0.1.0. |
| 3 | Node engines | `>=22` (dev target v25.9.0) | Spec was originally "Node 20 LTS" — updated. |
| 4 | Personal-sync model | **Background sync while user has active session** via local IPC | Web process holds DEK in memory; exposes a localhost-only Unix socket (`/tmp/greylock-keybridge.sock`, mode 0600); sync worker calls socket every 15 min for users with active sessions. **DEK never persists outside web-process memory.** |
| 5 | PCC membership | Rory + Tristan + Cade — all three are PCC members | Personal data still strictly siloed per user. |
| 6 | Plaid env for v0.1 | Sandbox | Move to Development/Production only after Phase 5 hardening passes. |
| 7 | Currency | USD only for v0.1 | Multi-currency deferred. |
| 8 | Concurrent sessions per user | Max 1 (new login revokes prior session) | Paranoid default. |
| 9 | Multi-passkey per user | 1 per user for v0.1 | Multi-device deferred to v0.2. |
| 10 | Net-worth snapshot frequency | Per sync (every 15 min) for both domains | Snapshots are timestamped; downsampling to daily can be added later. |
| 11 | Audit log retention | Forever (append-only with hash chain) | `pnpm admin:audit-verify` checks chain integrity. |

## 5. Agent topology

### Build agents
- **AGENT-ARCH** — System architect. Directory layout, module contracts, DB schema, API surface, threat model. **No implementation code.** Output: `docs/ARCHITECTURE.md`, `docs/THREAT_MODEL.md`, `docs/API.md`, `prisma/schema.prisma`, `lib/types/*.ts`.
- **AGENT-AUTH** — Passkey + session. Owns `lib/auth/`, `app/api/auth/*`.
- **AGENT-CRYPTO** — Encryption layer. Most security-critical agent. Owns `lib/crypto/`.
- **AGENT-DB** — Prisma + SQLCipher + repositories. Owns `lib/db/`, `prisma/`.
- **AGENT-PLAID** — Plaid SDK wrapper, encrypted token persistence. Owns `lib/plaid/`, `app/api/plaid/*`.
- **AGENT-SYNC** — Background poll loop, snapshot writer, manual sync endpoint. Owns `lib/sync/`, `scripts/sync.ts`.
- **AGENT-COMPUTE** — Pure functions. NW / cash / $1B / month-net. Fully unit-tested. Owns `lib/compute/`.
- **AGENT-UI** — App Router pages. OVRWCH aesthetic. Owns `app/(dashboard)/*`, `app/connect/*`, `app/admin/*`, `components/*`, `styles/*`.
- **AGENT-AUDIT-LOG** — Hash-chained append-only audit log. Owns `lib/audit/`.

### QA agents
- **QA-SEC** — Security auditor. **Veto power.** Reviews crypto, auth, session, secrets handling. Manually traces every Plaid-token code path.
- **QA-TYPES** — `tsc --noEmit --strict`, Zod coverage at boundaries, no `any`/`as` casts.
- **QA-TEST** — Coverage gates: `lib/crypto` ≥90%, `lib/auth` and `lib/compute` ≥80%, overall ≥60%.
- **QA-PRIVACY** — User-A-cannot-read-User-B integration tests; PCC visibility is correctly gated by `PccMembership`.
- **QA-UX** — OVRWCH aesthetic checklist (fonts, color tokens, layout, empty/error/loading states).

## 6. Phases

0. **Bootstrap** (Orchestrator): repo init, deps, mkcert, README, `.env.example`. ✅ committed at `82a14ad`.
1. **Architecture** (AGENT-ARCH): docs + Prisma schema + TS contracts + API surface + threat model.
2. **Crypto + Auth + DB** (parallel: AGENT-CRYPTO, AGENT-AUTH, AGENT-DB).
3. **Plaid + Sync + Compute + Audit** (parallel: AGENT-PLAID, AGENT-SYNC, AGENT-COMPUTE, AGENT-AUDIT-LOG).
4. **UI** (AGENT-UI).
5. **Hardening + Docs** (Orchestrator + QA-SEC final pass). Tag `v0.1.0`.

Each phase ends with: a git commit, a `docs/BUILD_LOG.md` entry, all relevant QA gates green.

## 7. Anti-patterns to reject (universal)

- `any` types or unjustified `as` casts.
- Logging Plaid tokens or session keys at any level (including `debug`).
- Storing secrets in code, comments, or commit history.
- Homemade crypto. Custom random sources. Math.random for security.
- Catching errors silently. Errors must propagate or be explicitly handled with audit-log context.
- Database access outside the repository layer.
- Mixing personal and PCC data without explicit `domain` tagging.
- Production / cloud configs.
- Password reset flows or any non-passkey credential mechanism.
- Plaintext access tokens persisted to disk for any reason.

---

End of SPEC. If a subagent's brief contradicts this file, the brief loses.
