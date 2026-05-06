# Greylock — Architecture

> Phase 1 deliverable. Source of truth for module boundaries and process model.
> If this doc disagrees with `docs/SPEC.md`, SPEC wins.

---

## 1. Directory tree

Every directory has one purpose. Cross-cutting concerns are forbidden — if a module needs something from another module, it imports the interface, not the implementation.

```
~/greylock/
├── app/                            # Next.js App Router. UI + server route handlers.
│   ├── (auth)/                     #   passkey enrollment / authentication pages
│   ├── (dashboard)/                #   the main grid (NW, cash, $1B, month-net)
│   ├── connect/                    #   Plaid Link bootstrap page
│   ├── admin/                      #   audit-log viewer (owner-only)
│   ├── api/
│   │   ├── auth/                   #   POST begin/complete enroll, begin/complete authn, logout
│   │   ├── plaid/                  #   POST link-token, exchange, sync, remove; GET items
│   │   ├── dashboard/              #   GET snapshot, series
│   │   ├── admin/                  #   POST enroll, revoke, revoke-all; GET audit-verify
│   │   └── healthz/                #   GET — checks crypto, db, keybridge readiness
│   ├── layout.tsx                  #   shell + global styles
│   └── globals.css                 #   IBM Plex Mono + Syne, OVRWCH terminal aesthetic tokens
├── components/                     # React components. CSS Modules colocated.
│   ├── chrome/                     #   nav, header, footer
│   ├── domain-toggle/              #   personal | pcc switcher (gated by PccMembership)
│   ├── stat-card/                  #   single number + delta + sparkline
│   ├── progress-bar/               #   $1B progress
│   ├── account-table/              #   account-level breakdown
│   └── empty-state/                #   no-data placeholders
├── lib/
│   ├── types/                      # AGENT-ARCH owns. Pure TS interfaces + Zod schemas.
│   │   ├── domain.ts               #   entities, brands, Result, error tagged unions
│   │   ├── services.ts             #   service interfaces (Crypto/Auth/Plaid/Audit/Repos)
│   │   ├── zod-schemas.ts          #   request/response shapes for every route
│   │   └── index.ts                #   barrel
│   ├── crypto/                     # AGENT-CRYPTO. AES-256-GCM, scrypt, HKDF.
│   │   ├── master-key.ts           #   Keychain fetch + TTY fallback + Master KEK derivation
│   │   ├── pcc-dek.ts              #   wrap/unwrap PCC DEK; rotation
│   │   ├── user-dek.ts             #   per-user KEK derivation + DEK wrap/unwrap
│   │   ├── envelope.ts             #   AES-256-GCM seal/open; AAD construction
│   │   ├── kdf.ts                  #   HKDF, scrypt; param contracts
│   │   ├── zeroize.ts              #   sodium_memzero / Buffer.fill(0)
│   │   └── index.ts                #   exports `CryptoService`
│   ├── auth/                       # AGENT-AUTH.
│   │   ├── allowlist.ts            #   email validation, placeholder rejection
│   │   ├── webauthn.ts             #   SimpleWebAuthn wrapper
│   │   ├── session.ts              #   iron-session config, sliding window, single-session
│   │   ├── rate-limit.ts           #   fixed-window in-DB
│   │   └── index.ts                #   exports `AuthService`
│   ├── db/                         # AGENT-DB.
│   │   ├── client.ts               #   Prisma client + SQLCipher adapter
│   │   ├── repositories/           #   one file per repo, all scope-by-construction
│   │   │   ├── user.ts
│   │   │   ├── passkey.ts
│   │   │   ├── session.ts
│   │   │   ├── item.ts
│   │   │   ├── account.ts
│   │   │   ├── transaction.ts
│   │   │   ├── snapshot.ts
│   │   │   ├── pcc-membership.ts
│   │   │   ├── pcc-key-wrap.ts
│   │   │   └── rate-limit.ts
│   │   └── migrate.ts              #   thin Prisma migrate wrapper
│   ├── plaid/                      # AGENT-PLAID.
│   │   ├── client.ts               #   Plaid SDK init
│   │   ├── token-broker.ts         #   the only place a plaintext access token exists
│   │   ├── service.ts              #   exports `PlaidService`
│   │   └── mappers.ts              #   Plaid types → domain types (Cents conversion lives here)
│   ├── sync/                       # AGENT-SYNC.
│   │   ├── orchestrator.ts         #   exports `SyncOrchestrator`
│   │   ├── keybridge-client.ts     #   IPC consumer (sync worker side)
│   │   └── snapshot-writer.ts      #   calls compute, writes NetWorthSnapshot
│   ├── compute/                    # AGENT-COMPUTE.
│   │   ├── net-worth.ts            #   pure: Account[] -> NetWorthResult
│   │   ├── month-net.ts            #   pure: Transaction[] + now -> MonthNetResult
│   │   ├── billion-progress.ts     #   pure: Cents -> BillionProgressResult
│   │   └── currency.ts             #   USD-only helpers; Cents <-> display string
│   ├── audit/                      # AGENT-AUDIT-LOG.
│   │   ├── service.ts              #   exports `AuditService`
│   │   ├── chain.ts                #   prevHash/entryHash byte-exact construction
│   │   └── sanitizer.ts            #   strips token-like values from `details`
│   ├── ipc/                        # Shared by web process AND sync worker.
│   │   ├── keybridge-protocol.ts   #   wire format + JSON shapes
│   │   ├── keybridge-server.ts     #   web-process side; binds /tmp/greylock-keybridge.sock
│   │   └── peer-cred.ts            #   macOS LOCAL_PEERCRED via getsockopt
│   └── runtime/                    # boot + process lifecycle
│       ├── boot.ts                 #   load Master KEK; unwrap PCC DEK; start keybridge
│       └── shutdown.ts             #   zeroize all keys; close socket; flush audit
├── prisma/
│   └── schema.prisma               # AGENT-ARCH wrote schema; AGENT-DB wires the driver.
├── scripts/                        # tsx entry points
│   ├── sync.ts                     #   the long-running sync worker (`pnpm sync`)
│   ├── admin-enroll.ts             #   passkey enrollment (one-shot URL)
│   ├── admin-revoke.ts             #   revoke a single user's sessions
│   ├── admin-revoke-all.ts         #   revoke all sessions
│   ├── admin-rotate-master.ts      #   rotate master passphrase + PCC DEK
│   └── admin-audit-verify.ts       #   walk hash chain
├── styles/                         # global CSS tokens (OVRWCH palette)
├── tests/
│   ├── unit/                       # Vitest — pure compute, crypto envelope, audit chain
│   ├── integration/                # Vitest + @prisma/client — repos, services
│   └── e2e/                        # Playwright — passkey flow (mocked authenticator)
├── docs/                           # canonical specs, threat model, retros
├── certs/                          # mkcert localhost.pem + key (gitignored)
└── .env.example                    # every required env var
```

**Why this layout.** The `lib/*` boundary is the contract. UI imports `lib/*`; route handlers compose `lib/*`. No `lib/*` module imports another `lib/*` implementation — only its interface from `lib/types/*`. AGENT-DB's repos are the only place Prisma is touched.

---

## 2. Process model

Greylock runs as **two long-lived processes** plus on-demand admin scripts.

```
+--------------------------+    Unix socket      +-----------------------+
|   web (Next.js)          |  <----------------> |   sync worker         |
|   `pnpm dev`             |  /tmp/greylock-     |   `pnpm sync`         |
|   pid file:              |  keybridge.sock     |   pid file:           |
|     pids/web.pid         |  mode 0600          |     pids/sync.pid     |
|                          |  LOCAL_PEERCRED     |                       |
|  - Owns Master KEK       |  authn              |  - Holds NO keys at   |
|  - Owns PCC DEK          |                     |    rest                |
|  - Holds per-user DEKs   |                     |  - Borrows DEKs       |
|    only while sessions   |                     |    per sync run       |
|    are active             |                     |  - Releases on        |
|  - Serves all HTTP       |                     |    completion          |
|  - Owns audit writer     |                     |                       |
|  - Owns Prisma client    |                     |  - Calls Plaid        |
|                          |                     |  - Writes to DB via   |
|                          |                     |    web-side repos by  |
|                          |                     |    HTTP-loopback or   |
|                          |                     |    its own Prisma     |
|                          |                     |    client (encrypted   |
|                          |                     |    file, key fetched   |
|                          |                     |    once at boot)       |
+--------------------------+                     +-----------------------+

       admin scripts (one-shot, exit on completion):
       `pnpm admin:enroll`, `:revoke`, `:revoke-all`, `:rotate-master`,
       `:audit-verify`
```

**State sharing.**

- The SQLCipher database file is the only durable shared state. Both processes open it; SQLite WAL mode guarantees readers don't block writers.
- The Master KEK and PCC DEK live ONLY in the web process's memory. The sync worker NEVER reads the master passphrase. When it needs to decrypt a PCC item token, it calls the keybridge with `requestDek({kind:'pcc', ...})`.
- Per-user DEKs live ONLY in the web process's memory while the user has an active session. The sync worker requests them per-run and releases on completion.

**Why the worker is separate.** SPEC.md §4 (decision 4): personal data syncs only while the user has an active session. The worker runs continuously, but each sync pass for a personal item is gated on `requestDek` succeeding for that user's session. PCC syncs run unconditionally because the PCC DEK is always loaded.

### IPC keybridge — Unix domain socket

| Property | Value |
|---|---|
| Path | `/tmp/greylock-keybridge.sock` (configurable via `KEYBRIDGE_SOCKET_PATH`) |
| Permissions | mode `0600`, owned by the user that started `pnpm dev` |
| Peer auth | macOS `getsockopt(LOCAL_PEERCRED)` — reject any peer whose `cr_uid` ≠ `getuid()`. Linux fallback: `SO_PEERCRED`. |
| Application auth | After accept, a hand-shake nonce + HMAC-SHA-256 over a session secret derived in process memory at boot (key = `HKDF(MasterKEK, info='greylock/keybridge/v1')`). Sync worker proves it has access to the same boot environment. |
| Protocol | newline-delimited JSON (line ≤ 16 KiB). Methods: `requestDek`, `releaseDek`, `ping`. |
| Lifecycle | Created in `lib/runtime/boot.ts` after Master KEK unwrap succeeds. Closed in `lib/runtime/shutdown.ts`. If `pnpm dev` dies, the worker's next `requestDek` returns `socket_unavailable`; the worker logs and skips the cycle. |
| Crash recovery | `boot.ts` `unlink`s the socket path before binding. If a stale socket exists from a previous crash, it is recreated. |

Detailed wire format: `docs/API.md` §Keybridge.

---

## 3. Two-tier encryption — key hierarchy

```
                         +------------------------------+
                         |  macOS Keychain              |
                         |  service: greylock-master    |
                         |  account: $USER              |
                         |  data: <master passphrase>   |
                         +---------------+--------------+
                                         |
                                         | (1) `security find-generic-password`
                                         v
                    +-------------------------------------+
                    |  Master Passphrase  (in memory)     |
                    +-------------------+-----------------+
                                        |
                                        | (2) scrypt(passphrase, salt=PccKeyWrap.kdfSalt
                                        |             + CRYPTO_PEPPER, N/r/p)
                                        v
                    +-------------------------------------+
                    |  Master KEK   (32 bytes, in memory) |
                    |  Lifetime: process lifetime         |
                    |  Persisted: NEVER                   |
                    +-----+----------------+--------------+
                          |                |
                          | (3) AES-GCM    | (4) HKDF(info='greylock/keybridge/v1')
                          | unwrap         v
                          v          +---------------------------+
                    +----------+     | Keybridge HMAC key        |
                    | PCC DEK  |     | (in memory, web process)  |
                    | 32 bytes |     +---------------------------+
                    | in mem   |
                    | persist: NEVER (only its wrap)
                    +-----+----+
                          |
                          | (5) AES-GCM with AAD bound to itemId + masterKekVersion
                          v
                    +-------------------------+
                    | Item.encryptedAccessToken   (DOMAIN=pcc rows) |
                    | persisted in SQLCipher-encrypted DB           |
                    +-----------------------------------------------+


                                     ─── per-user side ───

  +---------------------+
  |  Passkey credential |   (private key inside authenticator,
  |                     |    never leaves device; client returns
  |                     |    public credentialId + assertion)
  +-----+---------------+
        |
        | (a) WebAuthn assertion verified server-side
        v
  +-------------------------------------------------+
  |  HKDF(                                          |
  |     IKM = credentialId || CRYPTO_PEPPER,        |
  |     salt = Passkey.kekSalt,                     |
  |     info = utf8('greylock/userKek/v1/' + uid))  |
  +-----+-------------------------------------------+
        |
        v
  +---------------------+
  |  Per-user KEK       |   32 bytes; in memory; lifetime = active session
  +-----+---------------+
        |
        | (b) AES-GCM unwrap with AAD = "personal:userdek:" + userId
        v
  +---------------------+
  |  Per-user DEK       |   32 bytes; in memory; zeroized on logout/idle/expiry
  +-----+---------------+
        |
        | (c) AES-GCM with AAD = "personal:itemtoken:" + itemId + ":" + userDekVersion
        v
  +---------------------------------------------+
  | Item.encryptedAccessToken (DOMAIN=personal) |
  | persisted in SQLCipher-encrypted DB         |
  +---------------------------------------------+
```

### Persistence summary

| Data | Where it lives | Lifetime |
|---|---|---|
| Master passphrase | macOS Keychain (encrypted by user login + Secure Enclave) | indefinite, user-controlled |
| Master KEK | web process memory only | process lifetime |
| Wrapped PCC DEK | `PccKeyWrap.wrappedDek` (DB) | until rotated; old retired versions kept for replay until garbage-collected |
| PCC DEK (cleartext) | web process memory only | process lifetime |
| Per-user KEK | web process memory only | active-session lifetime |
| Wrapped per-user DEK | `User.wrappedUserDek` (DB) | until rotated |
| Per-user DEK (cleartext) | web process memory only | active-session lifetime; zeroized on logout / idle / absolute timeout / process exit |
| Plaid access token (encrypted) | `Item.encryptedAccessToken` (DB) | until item removed |
| Plaid access token (plaintext) | brokered into a `Buffer` for the duration of one Plaid SDK call only; `Buffer.fill(0)` immediately after | < 1 second per use |

### AAD scheme

Every AES-256-GCM `seal` and `open` binds the AAD to **(domain, row identity, key version)**. This is the property that prevents cross-domain ciphertext substitution.

| Protected blob | AAD (UTF-8) |
|---|---|
| Personal Plaid access token | `personal:itemtoken:<itemId>:<userDekVersion>` |
| PCC Plaid access token | `pcc:itemtoken:<itemId>:<masterKekVersion>` |
| Wrapped per-user DEK | `personal:userdek:<userId>` |
| Wrapped PCC DEK | `pcc:dekwrap:v<version>` |

A `personal` ciphertext copied into a `pcc` row will fail the GCM tag check on `open` because the AAD computed from the new context (`pcc:itemtoken:...`) differs from the AAD that was bound at encrypt time. SPEC §7 anti-pattern (mixing personal/PCC) is enforced cryptographically, not just by code review.

### Blob format (on disk)

```
byte 0       : version             (currently 0x01)
byte 1       : domain_tag          (0x01 personal, 0x02 pcc)
bytes 2..13  : nonce               (12 bytes, CSPRNG)
bytes 14..N  : ciphertext          (variable)
bytes N+1..N+16 : GCM tag          (16 bytes)
```

The `domain_tag` byte is redundant with the AAD prefix but provides an early sanity check during decode.

### Key derivation parameters (locked)

- **scrypt** for master passphrase: `N = 1 << 17 (131072)`, `r = 8`, `p = 1`, `dkLen = 32`. Per-wrap random `kdfSalt` is concatenated with the bytes of `CRYPTO_PEPPER` before being passed in.
- **HKDF-SHA-256** for everything else: 32-byte output. The `info` string is fixed per-purpose and includes a version tag (`/v1`) so derivations are unambiguous.
- **AES-256-GCM** for envelope. 12-byte nonce from `crypto.randomBytes`. 16-byte tag.
- **NEVER** reuse a nonce for the same key. Implementation must use `crypto.randomBytes` per call; tests must verify nonce uniqueness in the audit chain on a sample of N=10000.

---

## 4. Auth flow

### Enrollment (passkey)

1. `pnpm admin:enroll <email>` (run by Rory): validates email is in the allowlist AND is not the literal placeholder `cade-placeholder@greylock.invalid`. Mints a one-time enrollment URL with a short-lived token; prints it to stdout.
2. The user opens the URL on their device. The page calls `POST /api/auth/registration/begin` with `{email, displayName}`.
3. Server validates allowlist, rejects placeholder, generates WebAuthn registration options (`@simplewebauthn/server#generateRegistrationOptions`), stashes the challenge in an iron-session cookie scoped to the registration ceremony.
4. Browser calls `navigator.credentials.create()`. User taps Touch ID / Face ID / security key.
5. Browser POSTs the attestation to `/api/auth/registration/complete`. Server verifies via `verifyRegistrationResponse`, persists `User` + `Passkey`, derives a fresh per-user DEK with `crypto.randomBytes(32)`, wraps it under the per-user KEK derived from this credential's `credentialId + kekSalt`, persists `User.wrappedUserDek`, audits `passkey_enrollment`.
6. The enrollment URL token is invalidated.

### Authentication

1. Browser POSTs `{email}` to `/api/auth/authentication/begin`. Server checks email exists, returns options with `allowCredentials = [user's credentialId]` and stashes challenge in an ephemeral cookie.
2. Browser calls `navigator.credentials.get()`. User authenticates locally.
3. Browser POSTs assertion to `/api/auth/authentication/complete`. Server `verifyAuthenticationResponse` against the stored `credentialPublicKey`, bumps the counter (rejects if non-monotonic — replay defense).
4. **Single-session-per-user enforcement.** Server calls `SessionRepository.findActiveByUser(userId)`; if one exists, revokes it with reason `new_login` and unloads any prior in-memory user DEK if no active sessions remain.
5. Server creates new `Session`: `expiresAt = now + 8h`, `idleTimeoutAt = now + 30m`. Sets iron-session cookie with `SameSite=Strict; Secure; HttpOnly; Path=/`.
6. Server derives the user's KEK from `(credentialId, kekSalt)` in memory, unwraps `User.wrappedUserDek`, holds the DEK keyed by `userId`.
7. Audit `session_created`. Returns `{userId, sessionId}`.

### Session lifecycle

- Every authenticated request: middleware reads cookie, validates session, checks `expiresAt > now` and `idleTimeoutAt > now`. If valid, slides `idleTimeoutAt = now + 30m`.
- Idle timeout / absolute expiry: set `Session.status = 'expired'`, audit `session_expired`, unload that user's DEK if no other active session exists, return 401.
- Logout: `POST /api/auth/logout`. Revoke session, unload DEK, audit `session_revoked`.

---

## 5. Plaid flow

```
USER ──Link UI──> /api/plaid/link-token (POST)
                        |
                        v
                 PlaidService.mintLinkToken
                        |
                        v
                 plaid.linkTokenCreate -> link_token
                        |
                        v
USER taps bank, completes Link flow
                        |
                        v
USER ──> /api/plaid/exchange (POST publicToken, domain)
                        |
                        v
                 PlaidService.exchangePublicToken
                  |     |
                  |     +- plaid.itemPublicTokenExchange -> access_token
                  |     |
                  |     +- CryptoService.encrypt({
                  |          handle: domain==='pcc' ? {kind:'pcc', version: master.v}
                  |                                 : {kind:'user', userId, version: user.v},
                  |          aad: {kind:'item_token', itemId},
                  |          domain,
                  |          plaintext: utf8(access_token),
                  |        })
                  |     |
                  |     +- ItemRepository.create(...encryptedAccessToken)
                  |     |
                  |     +- buffer.fill(0) on the access_token Buffer
                  |
                  v
                 Returns itemId; plaintext token never leaves this scope.
```

### Sync cursor logic

- Each `Item` stores `syncCursor` (Plaid `transactions/sync` cursor) and `lastSyncAt`/`lastSyncOutcome`.
- `PlaidService.syncItem(itemId)` calls `transactions/sync` with the saved cursor, applies `added/modified/removed` to the DB inside a single transaction, and writes the new cursor only on success. Cursor advances ONLY on commit.
- `consecutiveFailures` increments on error and is read by the orchestrator: ≥5 → exponential back-off, surfaced in the UI as "needs reconnection".

### Item removal

- `POST /api/plaid/items/remove`. Server calls `plaid.itemRemove`, soft-deletes the row (`removedAt = now`), zeroizes the encrypted blob with `Buffer.fill(0)` (the row stays for audit history), audits `plaid_item_removed`.

---

## 6. Compute layer

`lib/compute/*` is **pure**. No Date, no I/O, no `crypto`. Every function takes everything it needs as input.

| Function | Inputs | Output |
|---|---|---|
| `netWorth({accounts})` | `Account[]` (already filtered by domain) | sum assets / liabilities, NW, cash subset, per-account contribution |
| `cashOnly({accounts})` | `Account[]` | `Cents` of `type === 'depository' && currentBalanceCents > 0` |
| `monthNet({transactions, now})` | `Transaction[]`, `Date now` | inflow / outflow / net for the rolling 30-day window |
| `billionProgress({netWorthCents})` | `Cents` | `{netWorthCents, goalCents=1e11n, progress in [0,1]}` |

**Sign convention.** Plaid uses `+amount` for outflow, `-amount` for inflow. `lib/plaid/mappers.ts` normalizes — domain `Cents` are Plaid-sign-preserved on `Transaction`, but compute functions document this and flip in `monthNet`.

### Fixture testing

Each compute function ships with `tests/unit/compute/<name>.test.ts`:

- A handful of golden fixtures captured as JSON in `tests/fixtures/compute/`.
- Property-style edge cases: empty input, single-account, mixed asset+liability, all-zero balances, negative NW.
- BigInt overflow is not a concern at v0.1 (max ≈ 9.2 × 10^18 cents; net worth is far below).

QA-TEST coverage gate for `lib/compute`: ≥80%.

---

## 7. Audit log

Append-only table `AuditLogEntry`. Every entry carries:
- `seq` (BigInt autoincrement) — chain order
- `ts` + `tsNanos` — precise timestamp
- `actorUserId`, `actorKind` — who/what
- `domain`, `subjectId`, `subjectKind` — context
- `action`, `outcome` — what happened
- `detailsJson` — structured payload (sanitized)
- `prevHash` — `entryHash` of the previous row (or 32 zero bytes for `seq=1`)
- `entryHash` — SHA-256 of fields below

### What's logged

- All auth events (enroll, authenticate, revoke, expire), success and failure.
- All Plaid events (link mint, exchange, sync start/end/fail, item add/remove, **decrypt-of-token**).
- All snapshot writes.
- All admin CLI invocations.
- All crypto lifecycle events (Master KEK load/unload, PCC DEK unwrap/zeroize, per-user DEK derive/zeroize).
- All keybridge denials.
- All rate-limit trips.

### What's NEVER logged

- Plaid access_token (plaintext or ciphertext), refresh_token, link_token contents.
- Master passphrase, Master KEK, PCC DEK, per-user DEKs, KEKs, kekSalt material.
- Session cookie value, iron-session secret.
- Raw WebAuthn assertion signatures, COSE keys.
- Transaction amounts of accounts the actor cannot otherwise see (audit details for `plaid_sync_completed` carry counts, not amounts).

`lib/audit/sanitizer.ts` enforces this with a deny-list of substring patterns over the `details` object before it's serialized; failure → `sanitizer_rejected_payload`. QA-SEC manually traces every audit append site.

### Hash chain — exact byte construction

```
prevHash    : 32-byte SHA-256 of previous entry's entryHash  (zero bytes for seq=1)
canonical    : seq          (uint64 BE)
            || tsUnixNanos  (int64 BE)                       // ts as ns since epoch
            || actorUserId  (utf8 || 0x00)                   // empty string => single 0x00
            || actorKind    (utf8 || 0x00)
            || domain       (utf8 || 0x00)                   // "" if null
            || subjectId    (utf8 || 0x00)
            || subjectKind  (utf8 || 0x00)
            || action       (utf8 || 0x00)
            || outcome      (utf8 || 0x00)
            || len32be(detailsJson) || detailsJson_bytes
            || prevHash     (32 bytes)
entryHash   : SHA-256(canonical)
```

`pnpm admin:audit-verify`:
1. Load all rows ordered by `seq` ASC.
2. Recompute `entryHash` from each row + the previous row's `entryHash`.
3. Compare against stored `entryHash`. Mismatch → return the offending `seq`.

---

## 8. Headers + transport

Already seeded in `next.config.mjs` (Phase 0):
- `X-Frame-Options: DENY`
- `X-Content-Type-Options: nosniff`
- `Referrer-Policy: strict-origin`
- `Permissions-Policy: camera=(), microphone=(), geolocation=()`
- `Strict-Transport-Security: max-age=63072000; includeSubDomains; preload`
- `poweredByHeader: false`

**Phase 5** adds the full Content-Security-Policy. Provisional outline (locked in Phase 5):

```
default-src 'self';
script-src  'self' 'wasm-unsafe-eval';
style-src   'self';
img-src     'self' data:;
font-src    'self';
connect-src 'self';
frame-ancestors 'none';
form-action  'self';
base-uri    'none';
object-src  'none';
upgrade-insecure-requests;
```

mkcert serves HTTPS at `https://localhost:3000`. `next dev --experimental-https` consumes `certs/localhost.pem` + `certs/localhost-key.pem`. `mkcert -install` is a one-time interactive step (Phase 0 build log notes this is pending operator action).

---

## 9. Operational view (steady state)

### Terminal 1 — `pnpm dev`

```
$ pnpm dev

> next dev --experimental-https ...

[runtime] reading master passphrase from Keychain…  OK
[runtime] loaded Master KEK (v3) into memory
[runtime] unwrapped PCC DEK (v3) into memory
[runtime] keybridge bound /tmp/greylock-keybridge.sock (mode 0600)
[runtime] audit chain head: seq=18421, hash=4f2c…
- ready - started server on https://localhost:3000

(every authenticated request)
[auth] session abc123 touched (idle reset to +30m)
[plaid] link-token minted for userId=usr_… domain=personal
[audit] seq=18422 plaid_link_token_minted success

(on logout / idle / expiry)
[auth] session abc123 revoked (reason=manual)
[crypto] zeroized DEK for userId=usr_…
[audit] seq=18423 session_revoked success
```

### Terminal 2 — `pnpm sync`

```
$ pnpm sync

[sync] connecting to /tmp/greylock-keybridge.sock…  OK
[sync] handshake: peer uid=501 (matches), HMAC verified
[sync] cycle interval: 15m

== sync cycle 1 ==
[sync] PCC items: 4 to sync
  [sync] item itm_a1: requestDek pcc v3 OK
  [sync] item itm_a1: transactions/sync (cursor=… ) added=12 mod=0 rem=0
  [sync] item itm_a1: snapshot written (NW=…)
  …
[sync] personal items (active sessions: 1)
  [sync] userId usr_rory: requestDek user v2 OK
    [sync] item itm_b3: added=0 mod=2 rem=0
    [sync] item itm_b3: snapshot written
  [sync] userId usr_tristan: requestDek user → DENIED (no active session) — skip
[sync] cycle complete: items=5/5, snapshots=5, dur=4.2s
[sync] sleeping 15m
```

---

## 10. Testing strategy (Phase 1 contract; tests built in Phase 2-4)

| Suite | Tooling | Coverage gate | What it covers |
|---|---|---|---|
| Unit — compute | Vitest | ≥80% | pure functions, fixtures, edge cases |
| Unit — crypto | Vitest | ≥90% | envelope encrypt/decrypt round-trip, AAD mismatch rejection, nonce uniqueness, KDF param sanity, zeroize verification |
| Unit — audit | Vitest | ≥90% | hash chain construction byte-for-byte, sanitizer deny-list |
| Integration — auth | Vitest + virtual authenticator | ≥80% | full passkey ceremony with `@simplewebauthn/server` against fixture credentials; counter monotonicity; allowlist; placeholder rejection |
| Integration — repos | Vitest + tmp SQLCipher DB | overall ≥60% | scope-by-construction: User A query never returns User B rows; PCC visibility through PccMembership |
| E2E | Playwright + virtual authenticator | n/a | enroll → authenticate → connect Plaid sandbox → see snapshot → log out → DEK zeroized |

**QA-PRIVACY** owns a dedicated suite that asserts: (a) for every repository method, calling it with `RepoScope = personal:userA` against a row where `userId=userB` returns `not_found` (not a permission error — must be indistinguishable); (b) PCC reads with a non-member scope return `not_found`.

---

## 11. Open architectural choices passed through to later agents

These are intentionally left open here so the right agent can decide:

1. **SQLCipher Prisma binding.** AGENT-DB picks between `@journeyapps/sqlcipher` (driver-adapter-style) and `better-sqlite3-multiple-ciphers` with a custom Prisma proxy. The schema and the repos don't care which.
2. **Sync worker DB connection.** Two options: (a) the worker opens its own SQLCipher connection (key obtained once via keybridge at boot), or (b) the worker proxies all writes through web-process HTTP. Option (a) is faster but requires giving the worker the DB key; the keybridge gates this. AGENT-DB + AGENT-SYNC decide jointly.
3. **iron-session secret rotation.** v0.1 keeps a single `SESSION_SECRET`. Rotation flow deferred.
4. **Audit log size.** SQLite TEXT column for `detailsJson` is fine to ~1 MB. If a single payload exceeds 64 KiB the sanitizer hard-rejects (`sanitizer_rejected_payload`).
