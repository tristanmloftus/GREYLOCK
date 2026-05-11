# Threat Model (v0.2, off-machine backend)

This document states what TerminalFinance v0.2 protects, against whom, with
what mechanisms, and where the gaps are. It is the contract pull requests
should reference when proposing security-relevant changes.

Architecture context is in [ARCHITECTURE.md](ARCHITECTURE.md); operator-side
controls are in [RUNBOOK.md](RUNBOOK.md).

## Threat actors

| Actor | Capabilities assumed | In scope? |
|-------|----------------------|-----------|
| **Local attacker (unlocked machine)** | Reads the client process's address space; reads files in the user's home directory; reads OS secret stores under the user's privilege. | Partially — OS-secret-store secrets resist offline copy; in-memory plaintext is bounded by `withDecryptedToken` / `ZeroizingBuffer` scope. |
| **Network attacker** | Sees IP packets between client and server; can attempt MITM. | Yes — TLS 1.2+ with HTTPS-only protocol allowlist in `CurlHttpClient` and cert verification against the host trust store mitigate this. |
| **Backend compromise (root on server)** | Reads memory of `TerminalFinanceServer`; reads the SQLCipher data file; reads env vars (`TF_MASTER_KEY`). | Acknowledged — the master key being in process memory is the irreducible cost of Option A (per `V0_2_PLAN.md` §c). Compromise reads everything in scope of v0.2 (no HSM, no per-session DEK). |
| **Plaid compromise** | Plaid leaks our access tokens or sees our request/response data. | Out of scope mitigation-wise. We trust Plaid's TLS chain. |
| **Repository / build chain attacker** | Submits malicious code via a PR. | Out of scope. Defended by review; not by code-signing or attestation in v0.2. |

## Assets (in priority order)

1. **User passphrase** — credential; not stored plaintext anywhere.
   Argon2id-hashed at rest (`server/auth/PassphraseHash.cpp:30-47`),
   zeroed in the TUI after use (`src/main.cpp:387-400,433-434`).
2. **Master encryption key** — sourced from `TF_MASTER_KEY`. Used by
   SQLCipher for at-rest DB encryption and by `PlaidTokenBroker` to derive
   the DEK. Held in process memory; zeroed after consumption
   (`server/main.cpp:339-342`, `PlaidTokenBroker.cpp:74,140,150-153`).
3. **Plaid access tokens** — envelope-encrypted under the DEK with
   `AAD = account_id` bytes (`PlaidTokenBroker.cpp:158-190`); plaintext
   exists only inside one `withDecryptedToken<F>` scope, then zeroed
   (`PlaidTokenBroker.h:212-240`).
4. **Session tokens** — 32 random bytes per session
   (`Session.cpp:77-82`). The plaintext (base64url) is sent to the client
   exactly once and cached in `ISecretStore`; only the BLAKE2b-256 hash is
   stored server-side. Never logged.
5. **Transaction data** — at-rest via SQLCipher (AES-256, OpenSSL).
   `transactions.description_encrypted` is BLOB but **not** independently
   wrapped at the handler layer in v0.2 (Option A chosen over Option C);
   confidentiality of that column depends on SQLCipher only.
6. **Audit chain integrity** — append-only, BLAKE2b-256 hash chain
   (`server/audit/CanonicalBytes.cpp:54-118`,
   `server/audit/SqlAuditLog.cpp`). A verifier (`AuditReplayTests`,
   `CMakeLists.txt:1103-1121`) shipped before the writer.

## AUDIT.md guardrails

`AUDIT.md` itself is not present in this repository; the four guardrails
referenced by the brief (F-1, F-2, F-4, F-5) are encoded directly into the
v0.2 source and verified by regression tests. Each subsection below names
the guardrail, paraphrases its requirement, and points at the code that
satisfies it.

### F-1 — No KEK derived from public input

**Requirement.** No key-encryption key may be derived from a value that the
attacker can choose or enumerate (e.g., a WebAuthn `credential.id`, a user
email, a session token).

**v0.2 implementation.**

- The server master key is sourced exclusively from the `TF_MASTER_KEY`
  environment variable. `server/main.cpp:81-87` reads it; the comment
  block at `server/main.cpp:192-196` calls out the guardrail explicitly.
- The Plaid DEK is derived via `crypto_kdf_derive_from_key(subkey_id=1,
  ctx="tf-plaid", master_key)` — `PlaidTokenBroker.cpp:32-55`. The
  `subkey_id` and `ctx` are constants; the master key is not derived from
  any caller-visible input. See header comment at
  `PlaidTokenBroker.h:19-29,47-48`.
- Session tokens are 32 bytes from `randombytes_buf`
  (`Session.cpp:77-78`). Comment at `Session.h:6-9,55-57` documents the
  contract.
- Plaid creds (`PLAID_CLIENT_ID`, `PLAID_SECRET`) are read once at server
  boot inside `PlaidApiClient`'s constructor; never logged, never stored
  outside the object (`server/main.cpp:386-388`).

### F-2 — Security checks must actually check

**Requirement.** A function whose name implies a security check must
perform the check it claims. No empty pass-through; no "TODO verify" that
returns success; no constant-time compare that is not constant-time.

**v0.2 implementation.**

- `BackendClient` refuses to construct with a non-HTTPS `base_url`
  (`src/services/BackendClient.cpp:16-19`). The handshake is then
  validated by libcurl against the system trust store with the HTTPS-only
  protocol allowlist on both initial requests and redirects
  (`CurlHttpClient.cpp:165-166`).
- `tests/test_server_healthz.cpp` includes `Healthz_TLSVerificationActuallyChecks`
  (described in `BUILD.md:112`): the TLS handshake **fails** when the
  client is configured with the wrong CA file — confirming verification
  isn't silently disabled.
- Passphrase verification delegates to libsodium's `crypto_pwhash_str_verify`,
  documented constant-time (`PassphraseHash.cpp:57-70`). The "user not
  found" path runs a dummy-hash verify so timing doesn't leak existence
  (`AuthHandlers.cpp:42-58,403-...`).
- Session validation checks revoked / absolute timeout / idle timeout
  inside one `BEGIN IMMEDIATE` transaction (`Session.cpp:166-178`); no
  read-only "looks valid" pre-check exists.
- The Plaid broker's `withDecryptedToken<F>` requires the lambda to be
  callable with `NoTokenTag` (compile-time `static_assert`,
  `PlaidTokenBroker.h:159-163`) so the no-token case cannot be silently
  represented as an empty string.

### F-4 — Audit canonical bytes do not double-count timestamps

**Requirement.** The hash input must not include both a millisecond and a
nanosecond field, or any other variant that would let two distinct entries
collide because one carries the same time in two forms.

**v0.2 implementation.**

- `server/audit/CanonicalBytes.cpp:67-70`:
  ```cpp
  // tsUnixNanos: int64 BE = ts_ms * 1_000_000 exactly once (GUARDRAIL F-4)
  // NEVER add a sub-millisecond component here.
  const int64_t ts_unix_nanos = e.ts_ms * INT64_C(1'000'000);
  append_be_int64(out, ts_unix_nanos);
  ```
  There is exactly one time field in the canonical bytes, derived from a
  single `ts_ms` source.
- Regression test `tests/test_audit_chain_canonical_bytes_no_double_count.cpp`
  (wired at `CMakeLists.txt:1079-1096`) asserts the byte sequence and the
  total length for a hand-rolled fixture so any future double-count
  regresses the test.
- `server/auth/AuthHandlers.cpp:65-69` ("F-4: single source of truth")
  centralizes `now_ms()` so every audit emit uses the same time call.

### F-5 — Rate limit keys on user identity, never on `X-Forwarded-For`

**Requirement.** Limit buckets must be keyed on something the attacker
cannot trivially rotate. `X-Forwarded-For` is attacker-controlled.

**v0.2 implementation.**

- `server/auth/AuthHandlers.cpp:402-403`:
  ```cpp
  // F-5: rate-limit key is ("auth_login", email). NEVER XFF.
  std::string bucket_key = "auth_login:" + email_str;
  ```
- `AuthHandlers.cpp:8`, `AuthHandlers.cpp:92` repeat the contract in
  comments adjacent to the bucket logic.
- Constants in `server/auth/AuthRateLimitInternal.h:21-23`:
  `kRateLimitMax = 5`, `kRateLimitWindowSecs = 900`,
  `kMaxRateBuckets = 10000`. The map is bounded; saturation fails closed
  (`AuthHandlers.cpp:144-150`) — denial-of-service against the rate
  limiter cannot OOM the server.
- `tests/test_auth_rate_limit.cpp` (wired at `CMakeLists.txt:986-1018`)
  asserts bucket-by-email behavior, window expiry, and the bounded-growth
  contract.

## STRIDE summary

| Threat | What it looks like here | Mitigation in v0.2 | Residual risk |
|--------|--------------------------|---------------------|---------------|
| **Spoofing** (auth bypass) | Attacker logs in as someone else | Passphrase (Argon2id MODERATE) + RFC 6238 TOTP; user-not-found path runs dummy hash for timing parity; bucket-per-email rate limit (`AuthHandlers.cpp:402-403`); session tokens 32 random bytes from CSPRNG (`Session.cpp:77-78`). | Passphrase strength is operator hygiene. No anti-phishing — operator must verify the server cert manually. WebAuthn passkeys deferred to v0.3. |
| **Tampering** | Attacker modifies DB rows or audit chain | SQLCipher AES-256 at rest (`server/main.cpp:323-337`); BLAKE2b-256 hash-chained audit log with replay verifier shipped first (`CanonicalBytes.cpp`, `SqlAuditLog.cpp`); foreign keys enforced (`Database` constructor sets `PRAGMA foreign_keys=ON`). | Root on the server can rotate the chain head silently if it also rotates the master key. No external attestation. |
| **Repudiation** | Operator denies an action | Every auth + Plaid + sensitive data op writes an audit row with actor_user_id, action, outcome, and a chained hash (`server/audit/`, wired across handlers). | Audit covers server-mediated actions only. No audit on the TUI client side (e.g., snapshot reads). |
| **Information disclosure** | Plaid token leaked from memory; transaction descriptions read by backend operator | `withDecryptedToken<F>` constrains plaintext to one lambda scope, `ZeroizingBuffer` zeroes on destruct (`PlaidTokenBroker.h:212-240`); master key zeroed immediately after DEK derivation (`PlaidTokenBroker.cpp:74,140`); session tokens stored as BLAKE2b-256 (`Session.cpp:80-82`); passphrases/totp/session tokens never logged (comment block `AuthHandlers.cpp:11`). | SQLCipher decrypts pages into process memory while the server runs — a server-side memory disclosure exposes plaintext rows. Transaction `description_encrypted` is not independently encrypted at the handler layer (Option A trade-off). |
| **Denial of service** | Attacker burns CPU on Argon2id; fills rate-limit map | Login bucket count + window (`AuthRateLimitInternal.h:21-22`); `kMaxRateBuckets=10000` cap with fail-closed sweep (`AuthHandlers.cpp:130-150`); cpp-httplib SSL accept loop runs on a thread pool. | Single-host server; no DDoS protection beyond what the host's firewall provides. No connection-rate limit at TLS handshake layer. |
| **Elevation of privilege** | Member of one entity reads another's data | `server/data/EntityMembership.cpp` gates entity-scoped routes; `SessionMiddleware::require_session` fronts every data handler (`server/auth/SessionMiddleware.cpp`). | Role string in `entity_memberships` is not used for fine-grained checks in v0.2 (membership == authorization). No admin/non-admin separation. |

## Crypto choices and why

### Argon2id MODERATE for passphrases

Source: `server/auth/PassphraseHash.cpp:30-47`. `OPSLIMIT_MODERATE` (3 ops)
+ `MEMLIMIT_MODERATE` (256 MiB) via `crypto_pwhash_str`. Argon2id beats
bcrypt and scrypt for resistance to GPU/ASIC parallelism (memory-hard
construction with both data-dependent and data-independent passes). bcrypt
caps at ~72 bytes of input and is single-threaded but not memory-hard;
scrypt was an OK choice in 2009. Argon2id is the password-hashing
competition winner and is libsodium-native, so no extra dependency.

### XChaCha20-Poly1305 IETF for envelope encryption

Source: `src/services/crypto/EnvelopeEncryption.h:5-23`. Chosen over
AES-GCM for two reasons: a 192-bit nonce (random nonces are safe at any
realistic scale; AES-GCM's 96-bit nonce risks collisions at high write
volumes) and no side-channel issues on platforms without AES-NI (the
server may run on a low-end VPS without hardware AES). The IETF variant
exposes an AAD parameter — required to bind ciphertexts to `account_id`
in the Plaid token broker.

### BLAKE2b-256 for hashing

Source: `server/audit/CanonicalBytes.cpp:100-118` (entry hash) and
`server/auth/Session.cpp:19-30` (token hash). BLAKE2b is significantly
faster than SHA-256 in software, libsodium-native, and provides equivalent
security. SHA-256 would only have been chosen if we needed byte-compat
with an existing TS chain — we don't.

### libsodium throughout

`Brewfile:4`, `vcpkg.json:7`. One dependency, modern defaults,
constant-time primitives, well-curated API surface. The choice is
documented in `V0_2_PLAN.md` §e — explicitly rejected OpenSSL (larger
surface, easier to misuse) and mbedTLS (smaller community).

## Known limitations / out of scope for v0.2

The following are intentional gaps. Pull requests proposing to remove a
limitation should reference this section.

1. **No anti-phishing.** Operators verify the server cert manually
   (`scripts/generate-dev-cert.sh` produces mkcert certs; production
   deployments use Let's Encrypt — see [RUNBOOK.md](RUNBOOK.md)).
2. **No HSM / hardware-backed master key.** `TF_MASTER_KEY` is an env var
   on the host (`V0_2_PLAN.md` §i Q3 choice A). Migrating to a managed
   secret store is a v0.3 task.
3. **No protection against a compromised host OS.** The server depends on
   the host kernel and filesystem. If root reads `/proc/<pid>/mem`, the
   master key is exposed during the server's lifetime. `sodium_mlock` is
   not used in v0.2 — page swapping may leak key material to disk.
4. **No protection against compromised Plaid.** We trust the Plaid TLS
   chain and Plaid's storage of our `client_id`/`secret`.
5. **No multi-user RBAC.** Membership in `entity_memberships` is the
   single authorization check. No role hierarchy is enforced beyond the
   role string (which is currently informational).
6. **No master-key rotation.** Re-keying SQLCipher requires
   `PRAGMA rekey` and re-encrypting every Plaid token blob. No tooling
   for this in v0.2.
7. **WebAuthn / passkey login deferred to v0.3.** `V0_2_PLAN.md` §d
   recommendation A.
8. **No client-side audit log.** Only server-mediated actions are
   recorded. TUI-only operations (e.g., viewing the cached DataStore)
   are not audited.
9. **No Linux client secret store.** `ISecretStore` has no Linux
   implementation (`CMakeLists.txt:282-288`); CLI auth flows on Linux
   clients refuse to run (`src/main.cpp:633-637` and siblings).
10. **`transactions.description_encrypted` not handler-side encrypted.**
    The column is BLOB-typed (`Migrations.cpp:265`); per-row envelope
    encryption was deferred (Option C dropped in favor of Option A). The
    field is encrypted at rest by SQLCipher only.

## Verification

The regression battery enforces the guardrails above. Run with:

```sh
ctest --test-dir build --output-on-failure
```

Tests that directly map to a guardrail or asset:

| Test target | Source | Guardrail / asset |
|-------------|--------|-------------------|
| `AuditCanonicalBytesTests` | `tests/test_audit_chain_canonical_bytes_no_double_count.cpp` | F-4 |
| `AuditReplayTests` | `tests/test_audit_replay.cpp` | Audit chain integrity (verifier-first) |
| `AuditWriterTests` | `tests/test_audit_writer.cpp` | Writer matches verifier |
| `AuditSanitizerTests` | `tests/test_audit_sanitizer.cpp` | Sanitizer allow/deny + token-shape reject |
| `AuthRateLimitTests` | `tests/test_auth_rate_limit.cpp` | F-5 |
| `AuthPassphraseTests` | `tests/test_auth_passphrase.cpp` | Argon2id round-trip + timing |
| `AuthTotpTests` | `tests/test_auth_totp.cpp` | RFC 6238 vectors |
| `AuthSessionTests` | `tests/test_auth_session.cpp` | mint / validate / revoke + window |
| `AuthEnrollmentTokenTests` | `tests/test_auth_enrollment_token.cpp` | One-shot tokens |
| `AuthEndpointsTests` | `tests/test_auth_endpoints.cpp` | End-to-end enroll+login+whoami+logout against a live server |
| `PlaidTokenBrokerTests` | `tests/test_plaid_token_broker.cpp` | Round-trip, AAD binding, no-token sentinel, multi-account isolation |
| `PlaidServiceRefactorTests` | `tests/test_plaid_service_refactor.cpp` | `Account` has no `plaid_access_token`; API is account_id-based |
| `CryptoTests` | `tests/test_crypto.cpp` | `ZeroizingBuffer`, `ConstantTime`, `EnvelopeEncryption` |
| `SqlCipherTests` | `tests/test_sqlcipher.cpp` | Wrong-key reject; correct-key round-trip; missing-key on encrypted file → throw |
| `ServerHealthzTests` | `tests/test_server_healthz.cpp` | Includes `Healthz_TLSVerificationActuallyChecks` (F-2) |
| `CurlHttpClientTests` | `tests/test_curl_http_client.cpp` | HTTPS-only, CRLF guard |

If a guardrail above ever moves out of compliance, the corresponding test
should fail. That's the deal.
