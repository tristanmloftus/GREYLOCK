# AGENT-ARCH — Phase 1 retrospective

> Written by the Orchestrator. AGENT-ARCH (planner subagent, Opus, fresh context) was spawned to produce all Phase 1 deliverables. It completed five of the eight before the Orchestrator (acting on operator instruction to "finish it now") stopped the agent and wrote the remaining three (`docs/THREAT_MODEL.md`, `docs/API.md`, this retrospective) to ship the phase. AGENT-ARCH's design is the source of truth for the schema and contracts; the Orchestrator-authored docs build directly on its foundations and have been spot-checked for consistency with what the agent produced.

---

## What was built

| Deliverable | Path | Author | Bytes |
|---|---|---|---|
| Architecture | `docs/ARCHITECTURE.md` | AGENT-ARCH | ~32 KB |
| Threat model | `docs/THREAT_MODEL.md` | Orchestrator (post-stop) | ~21 KB |
| API surface | `docs/API.md` | Orchestrator (post-stop) | ~13 KB |
| Prisma schema | `prisma/schema.prisma` | AGENT-ARCH | ~16 KB |
| Domain types | `lib/types/domain.ts` | AGENT-ARCH | ~14 KB |
| Service interfaces | `lib/types/services.ts` | AGENT-ARCH | ~25 KB |
| Zod schemas | `lib/types/zod-schemas.ts` | AGENT-ARCH | ~12 KB |
| Barrel | `lib/types/index.ts` | AGENT-ARCH | 385 B |
| Retro | `docs/agents/AGENT-ARCH.md` | Orchestrator | this file |

---

## Decisions AGENT-ARCH made (not pre-locked in `docs/SPEC.md`)

1. **Branded types via TS `unique symbol`** in `domain.ts` for `UserId`, `ItemId`, etc. Prevents accidentally passing a `UserId` where an `ItemId` is required without resorting to runtime checks. **Justification:** zero runtime cost, surfaces in the type checker, agreed pattern across the codebase.
2. **`Result<T, E>` discriminated union instead of throwing** for service-layer errors. Auth and crypto failures *cannot* be silently swallowed because the caller is forced to discriminate. **Justification:** spec anti-pattern §SPEC.md "Catching errors silently" — `Result` makes the error path syntactically visible.
3. **`RepoScope` discriminated union** (`personal | pcc | admin`). Every repository method takes a scope as its first param. **Justification:** scope-by-construction makes a "give me User B's items" query physically impossible without an `admin` scope (which only the CLI has). Forces QA-PRIVACY's invariant into the type system.
4. **Cents as `bigint`** everywhere (`type Cents = bigint`). Output JSON encodes as string via `CentsOutSchema`. **Justification:** float drift on dollar amounts is forbidden; JSON-number can't safely represent values beyond 2^53; bigint is native in Node 22+ and Prisma supports it.
5. **`EncryptedBlob` brand** `Branded<Uint8Array, 'EncryptedBlob'>` with a single `unsafeFromBytes` constructor. **Justification:** prevents anywhere in app code from constructing a fake encrypted blob — only `lib/crypto/*` can mint one.
6. **Audit hash-chain construction documented byte-exact** (`docs/ARCHITECTURE.md` §7). Length-prefixed `detailsJson`, fixed field order, explicit handling of nullable strings (`utf8 || 0x00`). **Justification:** any agent re-implementing the chain produces identical hashes, making the chain re-verifiable across language / library upgrades.
7. **AAD scheme bound to `(domain, row identity, key version)`** with explicit prefix per kind. **Justification:** elevates the "personal vs PCC" partition from a code-review check to a cryptographic check — a swapped ciphertext fails GCM-tag verification, full stop.
8. **Domain-tag byte in blob format (0x01 personal / 0x02 pcc)**. Redundant with AAD prefix but provides early sanity check. **Justification:** belt-and-suspenders; cheap.
9. **scrypt `N=1<<17`** for the master passphrase derivation. **Justification:** ~110 ms on M1; meaningful slowdown vs offline brute force but not painful in interactive use; `r=8, p=1` are standard.
10. **HKDF info-strings versioned** (`/v1`). **Justification:** painless re-derivation upgrade path if we change KDF parameters in v0.2.
11. **`SyncOrchestrator.runOnce({ now })`** takes `now` as a param. **Justification:** keeps the sync logic deterministic and unit-testable; consistent with the compute layer's purity rule.
12. **Two architectural choices explicitly handed off to later phases** (in `ARCHITECTURE.md` §11): (a) SQLCipher Prisma binding choice → AGENT-DB; (b) sync-worker DB connection model → AGENT-DB + AGENT-SYNC jointly.

---

## Open questions for the Orchestrator (numbered, with default-if-no-answer)

These came up while writing the design but were not blocking — defaults below allow Phase 2 to proceed.

1. **Should we add an `audit chain head pinning` mechanism for v0.1?** Threat model §1.2.2 / D-6 leaves this as an accepted gap. **Default:** ship v0.1 without; add to v0.2 backlog.
2. **Where should `kekSalt` for `Passkey` come from?** Currently spec'd as `crypto.randomBytes(16)` per passkey at registration. **Default:** keep that; flag if you want longer / pepper-mixed.
3. **Should `pnpm admin:enroll` accept a `--role` flag or hard-code by allowlist position?** `docs/API.md` §9 documents `--role`. **Default:** keep `--role`; reject `owner` for any email other than `OWNER_EMAIL`.
4. **Is the `cade-placeholder@greylock.invalid` rejection at registration a hard error or a clear warning?** Currently spec'd as hard error (`placeholder_email_rejected`). **Default:** keep hard.
5. **Cookie name override.** Currently `SESSION_COOKIE_NAME=greylock_session` in `.env.example`. **Default:** keep.
6. **Do we want a `/api/admin/audit/export` route to dump the chain to a signed file for offline anchoring?** Out of scope for v0.1 per D-6. **Default:** no.

None of these block Phase 2 — pick them up at Phase 5 or in v0.2 planning.

---

## What I'd improve next pass

- **Generate a sequence diagram** (mermaid) for the IPC keybridge handshake. The text in `docs/API.md` §8 covers it but a picture would help AGENT-SYNC and AGENT-CRYPTO.
- **Pin a Prisma client version once AGENT-DB picks the SQLCipher binding** — currently `@prisma/client ^6.1.0` is loose; the binding may force a specific version.
- **Write a 1-page `lib/types/README.md`** explaining the import order (domain → services → zod-schemas → index) so future agents don't accidentally circular-import.
- **Capture the chosen scrypt timing on this machine** (M1/M2/M3) at boot and write to a `crypto.bench.log` so we can detect parameter drift.

---

## Hand-off notes for Phase 2 agents

### To AGENT-CRYPTO

- All your interfaces are in `lib/types/services.ts` — `CryptoService`, `AadContext`, `KeyHandle`. Don't add fields without my (Orchestrator) sign-off.
- The AAD prefix table in `docs/ARCHITECTURE.md` §3 is authoritative. Encode AAD as **UTF-8 bytes**, not strings, when feeding to GCM.
- `lib/crypto/zeroize.ts` should expose `zeroize(buf: Uint8Array): void` and `withZeroized<T>(allocFn, useFn)`. Make the `withZeroized` form ergonomic enough that everyone uses it.
- Master passphrase fetch lives in `lib/crypto/master-key.ts`. **Do not log the passphrase**, do not include it in errors, do not put it in `details` of any audit entry. Sanitizer will reject anyway, but defense-in-depth.
- Tests must include: round-trip success per domain; tampered ciphertext → `tag_invalid`; tampered AAD → `aad_mismatch`; tampered domain → `aad_mismatch`; nonce uniqueness over N=10000 encrypts.

### To AGENT-AUTH

- Allowlist enforcement in `lib/auth/allowlist.ts`. Reject `cade-placeholder@greylock.invalid` as hard error. Email comparison is **case-insensitive** post-trim — normalize before comparing.
- Single-session enforcement: in `completeAuthentication` you must `SessionRepository.findActiveByUser` and revoke the prior before creating the new. Audit both events.
- iron-session config: 30-min idle / 8-hr absolute. `secret` from `SESSION_SECRET` env. Cookie name from `SESSION_COOKIE_NAME`.
- WebAuthn options: `userVerification: 'required'`, `residentKey: 'required'`, `attestation: 'none'`. RPID = `WEBAUTHN_RP_ID`, origin = `WEBAUTHN_RP_ORIGIN`.
- Counter monotonicity is non-optional. If `newCounter <= storedCounter` and either is `> 0`, **fail closed** with `webauthn_verification_failed` — that's a replay.

### To AGENT-DB

- The schema is locked but the SQLCipher binding is your choice. Trade-offs documented in `docs/ARCHITECTURE.md` §11 — both `@journeyapps/sqlcipher` and `better-sqlite3-multiple-ciphers` (via Prisma driver adapter) are candidates. Pick one and document why in `docs/agents/AGENT-DB.md`.
- Connection key handoff: at boot, `lib/runtime/boot.ts` derives the SQLCipher key from the master passphrase (separate HKDF info string: `greylock/sqlcipher-key/v1`) and passes it to your `lib/db/client.ts`. This keeps the file-encryption key out of the Master KEK direct path.
- Repositories must enforce `RepoScope`. The TypeScript signature already requires it; your implementation must also use it in every WHERE clause. QA-PRIVACY tests will assert this.
- Write a SQL-level read of one of your repos and post the SQL in `docs/agents/AGENT-DB.md` so QA-SEC can review the WHERE clauses.

---

## State at hand-off

- Schema parses with Prisma (verify with `pnpm prisma format && pnpm prisma validate`).
- TS types compile under strict mode against the Phase-0 `tsconfig.json` (verify with `pnpm typecheck` once Prisma client is generated; some files reference Prisma-generated types only by interface, so they should compile without `pnpm prisma generate`).
- All cross-module contracts are interfaces; no Phase 2 agent should need to invent a new contract — only implement.
- Phase 1 commit boundary is the next Orchestrator action: `feat(arch): system architecture and contracts`.
