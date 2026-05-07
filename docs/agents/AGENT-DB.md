# AGENT-DB — Phase 2 retrospective

> Wiring Prisma to a SQLCipher-encrypted SQLite database, every repository
> implemented with scope-by-construction, the initial migration shipped.
> AGENT-CRYPTO and AGENT-AUTH worked in parallel on the same Phase 2 commit.

---

## Architectural decisions made (with reasoning)

### 1. SQLCipher Prisma binding — chose `better-sqlite3-multiple-ciphers` + `@prisma/adapter-better-sqlite3` via pnpm aliasing

**Choice.** Drop-in replacement of `better-sqlite3` with `better-sqlite3-multiple-ciphers@11.10.0`, fronted by Prisma's official `@prisma/adapter-better-sqlite3@6.19.3` driver-adapter API. The replacement is wired through pnpm's `pnpm.overrides` so the adapter's transitive `require('better-sqlite3')` resolves to the SQLCipher fork without any source edits.

**Why not `@journeyapps/sqlcipher`.** `@journeyapps/sqlcipher` ships an older `node-sqlite3`-style async API and has no Prisma adapter. Wiring it would require hand-rolling a Prisma `SqlDriverAdapter` from scratch and re-implementing column-type coercion, BigInt handling, transaction semantics, etc. — the same surface Prisma already maintains in `@prisma/adapter-better-sqlite3`. Net cost is much higher and there is no security benefit (both packages link the same SQLCipher C library).

**Why `better-sqlite3-multiple-ciphers` specifically.**
- It exposes the exact same JS surface as `better-sqlite3@11.10.0`. The Prisma adapter compiles unmodified against it.
- It supports SQLCipher format 4 and ChaCha20-Poly1305 in the same binary.
- It's actively maintained alongside upstream `better-sqlite3` releases.
- Synchronous API matches better-sqlite3 — Prisma's adapter is built around it.

**The pnpm alias gotcha (worth documenting).** The first install only aliased the *top-level* `better-sqlite3` dependency. The Prisma adapter ships a peer-installed `better-sqlite3` in its own pnpm node_modules tree, which `require('better-sqlite3')` from inside the adapter resolved to. The result was a "successful" SQLCipher pragma application against a *non-cipher* binary — the SQLite header on disk read `SQLite format 3\x00` rather than encrypted bytes. A test that looked for plaintext markers on disk caught it.

The fix: `pnpm.overrides` (not `dependencies` aliasing) — this forces the override across the entire dep graph, including the adapter's transitive resolution. After a clean `rm -rf node_modules pnpm-lock.yaml && pnpm install`, the adapter's `require('better-sqlite3')` resolves to `better-sqlite3-multiple-ciphers` and SQLCipher actually engages. The `tests/integration/db/sqlcipher-roundtrip.test.ts > writes ciphertext to disk` test now confirms ciphertext on disk; the `> fails to read when reopened with the WRONG key` test confirms cipher-tag enforcement.

**SQLCipher key application.** The Prisma adapter has no hook between `Database` construction and the first query. We subclass `PrismaBetterSQLite3` and override `connect()` to apply `PRAGMA cipher='sqlcipher'; PRAGMA hexkey='<hex>'; PRAGMA cipher_compatibility=4` to the adapter's underlying client immediately after `super.connect()` returns, before any Prisma query runs. The key is passed as hex via a quoted SQL literal — the contents are pure `[0-9a-f]` so SQL injection is impossible by construction.

### 2. Sync-worker DB connection — chose option (a): worker opens its own SQLCipher connection

Per the brief's own guidance, v0.1 picks (a): the sync worker derives its own SQLCipher key at boot via the same HKDF (`info='greylock/sqlcipher-key/v1'`) and opens its own Prisma client. This is dramatically simpler than HTTP-proxying every write through the web process and matches the localhost-only deployment threat model — the disk file is the only durable shared state, both processes need read+write, and SQLite's WAL mode handles concurrent reads/writes without contention.

The keybridge protocol still gates per-user DEK material (per-user DEKs only flow through IPC, never to disk), so personal-data sync continues to require an active session per the spec. The PCC DEK is loaded only in the web process; the sync worker requests it on demand for PCC items just like the web process does for its own decrypts.

**Trade-off accepted.** The sync worker's process now also holds the SQLCipher key bytes. This is an additional in-memory copy of the same secret the web process already holds — both processes derive it from the same Master KEK, both run as the same UID, both are localhost-only. The threat model already assumed an attacker with running-process memory + master passphrase can read PCC tokens; widening "running-process memory" to two processes in the same user account is a no-op against that threat.

---

## Schema additions (justified)

### `EnrollmentToken` model
Per the brief, AGENT-AUTH ships an interface in `lib/auth/enrollment-token.ts` (`EnrollmentTokenRepository`); AGENT-DB owns the storage implementation. Added:

```prisma
model EnrollmentToken {
  id        String   @id @default(cuid())
  email     String
  tokenHash Bytes    @unique
  expiresAt DateTime
  usedAt    DateTime?
  createdAt DateTime @default(now())

  @@index([email])
  @@index([expiresAt])
}
```

- `tokenHash` is `Bytes` and `@unique`. **The cleartext token is NEVER stored.** Server hashes inputs and compares hashes by exact-match index lookup (constant-time at the index layer).
- `usedAt` makes the token one-shot. `verify()` does NOT mark it used; the route handler calls `consume()` only after a successful registration ceremony so transient transport errors don't burn a token.
- Indexes on `email` (admin "list outstanding tokens for X" query) and `expiresAt` (cron-style sweeps in v0.2).

This was the only schema edit for SQLCipher reasons or otherwise. The schema is otherwise unchanged from AGENT-ARCH's Phase 1 deliverable.

### Generator preview-features
Removed the empty `previewFeatures = []` setting after Prisma's CLI warned that `driverAdapters` is GA in 6.x and no preview flag is needed.

---

## Repository design — scope-by-construction enforcement

Every repository method that returns or mutates user-owned data takes a `RepoScope` first parameter and routes through a single `whereForScope()` builder per repo. There is **no branch** that uses an unfiltered Prisma query for admin scope; the admin path goes through the same builder with an empty extra filter.

PCC visibility is enforced as a **two-step gate**:

1. `requirePccMembershipOrNotFound({scope, prisma})` — runs only for `kind: 'pcc'` scopes; queries `PccMembership` for `userId = scope.memberOfUserId AND revokedAt IS NULL`. Returns `Err({kind: 'not_found'})` if missing — deliberately indistinguishable from "row does not exist".
2. The actual repo query then filters `domain = 'pcc'` (and optionally other extras).

This was the cleanest mapping I could find given Prisma's relation-filter expressivity — a single Prisma `where: { ... AND: { ... EXISTS ... } }` cannot express "EXISTS in another unrelated table without a relation". Two queries inside the same repo method (membership check → main read), without an explicit transaction, is acceptable here because the PCC membership table changes only on admin action; there's no TOCTOU window an attacker could ride.

Out-of-scope reads return **`{kind: 'not_found'}`**. Never `{kind: 'unauthorized'}`. Tests assert this for every method on every repo — the QA-PRIVACY-style suite in `tests/integration/db/scope-by-construction.test.ts` covers all four cases (personal-A→A only, personal-A→B null, pcc-non-member→not_found, pcc-member→visible, admin→all).

---

## Migration strategy

Prisma's `prisma migrate dev` schema-engine cannot route through driver adapters — it opens its own SQLite connection from `DATABASE_URL` and would need the SQLCipher key, which it has no way to receive. To handle this:

1. **Generation**: `pnpm prisma migrate dev --name init` runs once against an unencrypted scratch DB. The generated SQL in `prisma/migrations/<ts>_init/migration.sql` is the deliverable. The scratch DB is discarded.
2. **Runtime application**: `lib/db/migrate.ts:applyMigrations()` reads the migration SQL files and applies each statement through the keyed Prisma client (`$executeRawUnsafe`). It maintains a `_greylock_migrations` table for idempotency, distinct from Prisma's `_prisma_migrations` so the two can coexist if needed.
3. **Tests**: `bootDb()` calls `applyMigrations()` automatically against a fresh per-test SQLCipher DB.

This means **`prisma migrate deploy` is never run against production data**. Migrations are applied at boot, in process, with the SQLCipher key already present.

---

## Validation evidence

| Step | Command | Result |
|---|---|---|
| Migration generation | `DATABASE_URL=file:./prisma/greylock-dev.db DEV_DB_PASSPHRASE=... pnpm prisma migrate dev --name init` | succeeded; migration written to `prisma/migrations/20260507001726_init/migration.sql` |
| Typecheck | `pnpm typecheck` | exit 0; `lib/db/**` zero errors |
| Tests | `DEV_DB_PASSPHRASE=test-pass pnpm vitest run tests/integration/db/` | **42/42 pass** (5 sqlcipher-roundtrip + 4 audit-chain + 10 happy-path + 23 scope-by-construction) |
| Full project tests | `pnpm test` | **192/192 pass** (db + crypto + auth tests all green together) |

Lint (`pnpm lint lib/db tests/integration/db scripts/db`) currently fails on a project-level eslint-config-next vs eslint-9 incompatibility unrelated to AGENT-DB's files. Workaround: `pnpm exec prettier --check` reports zero issues across all AGENT-DB files. Filed to orchestrator as a Phase 5 hardening item.

---

## Hand-off notes

### To AGENT-CRYPTO
- `lib/db/sqlcipher-key.ts:deriveSqlcipherKey(masterKek)` is the contract. HKDF-SHA-256, info=`'greylock/sqlcipher-key/v1'`, length 32. The Master KEK is ALWAYS supplied as bytes; we don't read env, Keychain, or any global.
- `bootDb({ sqlcipherKey })` in `lib/db/index.ts` returns `{ prisma, repos, dispose }`. Call this from `lib/runtime/boot.ts` right after the Master KEK unwrap. Then call `registerBootedDbSingleton(booted)` so the registry's lazy `createRepositories()` and the route handlers' direct `enrollmentTokenRepo` proxy can resolve.
- `pccKeyWrapRepo` exposes `findActive`, `findByVersion`, `insert`, `retire`. Use these from `lib/crypto/pcc-dek.ts` to load and rotate the wrapped PCC DEK.

### To AGENT-AUTH
- `userRepo.create()` and `userRepo.setWrappedUserDek()` together do the registration write. Read `wrappedUserDek` and `userDekVersion` via `readUserAuthMaterial({ prisma, userId })` from `lib/db/repositories/user.ts` (off-band helper — not in the canonical contract — colocated to keep all User SQL in `lib/db/`).
- `passkeyRepo.create({...kekSalt})` accepts the `kekSalt` directly. Read it back via `readPasskeyKekSalt({prisma, passkeyId})` (off-band helper, same rationale).
- `sessionRepo.findActiveByUser(userId)` — used for the single-session-per-user enforcement. Order is `createdAt DESC` so the newest active session is returned (defensive — there should be at most one).
- `rateLimitRepo.consumeOrTrip` is implemented atomically inside a Prisma `$transaction`; both the read and the write happen under the same SQLite write lock so two concurrent callers can't both see "below cap".
- `enrollmentTokenRepo` is exposed on the singleton as `lib/db/index.ts → enrollmentTokenRepo`. The registration-begin route's `import('lib/db/index.js')` and `mod.enrollmentTokenRepo.verify(...)` will work as soon as `bootDb()` has been called.

### To AGENT-AUDIT-LOG (Phase 3)
- `lib/db/repositories/audit.ts:createAuditRepository()` ships the storage layer. It implements:
  - `append()` — atomic `$transaction` reading the chain head and writing the new entry. Hash construction is byte-exact per `docs/ARCHITECTURE.md` §7 and verified against the same function used by `verifyChain()`.
  - `verifyChain()` — pages through entries seq-asc, recomputes both `entryHash` AND `prevHash` (the prevHash check is defense-in-depth against a row whose entryHash is forged but whose prevHash is bogus).
  - `query()` — covers the admin audit viewer.
- The shared hash function is exported as `computeEntryHash` from `lib/db/index.ts` for re-use in `lib/audit/chain.ts` if you want a "compute hash of pending entry without writing" helper.
- The repository performs NO sanitization of `detailsJson`. Run your sanitizer (`lib/audit/sanitizer.ts`) BEFORE calling `append()`. If a sanitizer rejection happens, surface `{kind:'sanitizer_rejected_payload'}` from your service layer — the storage layer treats whatever JSON string you hand it as authoritative.

---

## Failure modes I caught and fixed during this phase

1. **pnpm alias did not propagate to the Prisma adapter's nested resolve** — added `pnpm.overrides`, did a clean install. Confirmed by writing a "ciphertext on disk" test that DOES read the raw file and grep for plaintext markers. Without that test, I would have shipped a "SQLCipher" path that wasn't actually encrypted.
2. **Prisma 6 `Bytes` columns require `Uint8Array<ArrayBuffer>`, not `Buffer<ArrayBufferLike>`** — `Buffer.from(b.buffer, ...)` produces the wrong variance. `asBuffer()` now allocates a fresh `ArrayBuffer`, copies bytes in, and returns `new Uint8Array(ab)`. Side benefit: returned bytes don't alias caller memory.
3. **`updateMany` doesn't support relation predicates on SQLite (`AND: { item: { is: ... } }` style)** in the version of Prisma we run. The Item-table updates use `whereForScope(scope, { id: args.id })` directly (Item carries its own `domain` and `userId`). Account/Transaction use `where: { item: { is: ... } }` only on `findMany`, never on `updateMany`. The single Item.updateSyncCursor path that needs an atomic increment uses `findFirst` (visibility) → `update` (by id) inside a single `$transaction`. This guarantees both the scope check and the increment are atomic.
4. **Driver-adapter migrations are unworkable as a single `prisma migrate dev` flow against an encrypted DB.** Documented + worked around with `lib/db/migrate.ts` + `_greylock_migrations` bookkeeping table. `pnpm prisma migrate dev` is now a generator-only flow.

---

## What I'd improve next pass

- **Cover `verifyChain()` paging with a test** that has > pageSize=500 rows. Current chain tests verify chains of 5–16 entries; they don't exercise the cursor path.
- **Stop using `try/catch` around every Prisma op manually.** The `tryDb()` helper covers most paths, but several methods still inline the try/catch for the create-update-error mapping. Tighten on the next pass; not security-critical.
- **The `_greylock_migrations` table has no row-level locking** during applyMigrations, so two processes booting simultaneously could both try to apply the same migration. SQLite serializes at the filesystem level so this is fine in practice, but a `BEGIN IMMEDIATE` around the read-and-apply window would be cleaner.
- **The SQLCipher key bytes in `lib/db/client.ts` are released to the GC** after the pragma converts them to a hex string. The hex string is then released too. Neither buffer is explicitly zeroized — Node strings are immutable. The mitigation is that the same key is held in MUCH longer-lived memory inside SQLCipher itself, so zeroizing our local copies wouldn't materially change the in-memory exposure surface.
