# AGENT-CRYPTO retrospective — Phase 2

## What shipped

`lib/crypto/` — full encryption layer, eight modules, ~900 LOC, 104 unit tests, **99.21% line / 93.75% branch / 100% function coverage** on the package. Every file in the brief was implemented exactly to scope. No `pnpm install`. No `package.json` change. No file outside the owned-paths list touched.

Modules:

- `zeroize.ts` — `zeroize`, `withZeroized`. Detached-buffer-safe.
- `kdf.ts` — `hkdf` (SHA-256, RFC 5869 length-capped), `scrypt` (locked params N=2^17, r=8, p=1, dkLen=32; `maxmem=256 MiB` to fit). Param-validation guards on both.
- `aad.ts` — `aadForItemToken`, `aadForUserDekWrap`, `aadForPccDekWrap`. UTF-8 only. Rejects empty ids and ids that contain `:` (delimiter collision).
- `envelope.ts` — `seal`, `open` over AES-256-GCM. Blob layout `version(1)=0x01 || domain_tag(1) || nonce(12) || ct || tag(16)`, byte-exact to ARCHITECTURE §3. Cross-domain substitution detected via the on-disk `domain_tag` byte before GCM is invoked, surfacing `aad_mismatch`. In-domain AAD differences fall through to `tag_invalid` (correct: GCM-driven).
- `master-key.ts` — Keychain fetch via injectable `SpawnImpl` test seam (so unit tests never touch the real Keychain). Master KEK = `scrypt(secret, kdfSalt || pepper, ...)`. `withPassphraseBytes` zeroizes on every exit path. The literal string `passphrase` never appears in any thrown `Error.message` — only in the canonical CryptoError enum kind `master_passphrase_unavailable` defined by AGENT-ARCH (see "Brief literal-text deviation" below).
- `user-dek.ts` — `deriveUserKek` (HKDF), `wrapUserDek`, `unwrapUserDek`, `withDerivedUserKek`. KEK is zeroized in `finally` after every wrap/unwrap.
- `pcc-dek.ts` — `wrapPccDek`, `unwrapPccDek`, and the pure `rotateMaster` rewrite loop. Rotation is callback-driven; this module never touches the DB.
- `index.ts` — `createCryptoService(...)` factory. Master KEK, PCC DEK, and the `Map<UserId, Buffer>` of per-user DEKs live in module-private closure state with `Object.freeze`'d service surface; nothing else in the codebase has a path to read them. Buffers from any failure path are explicitly tracked for zeroize.

Tests cover, with N=10000 nonce-uniqueness, every key tier round-trip, tampered-ct/tag/AAD/domain-tag/version, cross-domain substitution, wrong-key-version, KDF parameter sanity, `withZeroized` zero-on-reject, mocked Keychain success / missing / bad-exit / TTY-fallback paths, and the rotation loop with N items.

## Locked-spec invariants enforced cryptographically

- AAD scheme matches ARCHITECTURE §3 byte-for-byte. A personal ciphertext copied into a PCC row physically cannot decrypt as PCC because (a) the on-disk `domain_tag` byte differs and we reject pre-GCM, and (b) even if the byte were forged, the AAD-prefix differs and GCM rejects. Both belt and braces.
- Nonce uniqueness asserted in `envelope.test.ts > 'produces 10000 distinct nonces under the same key'`.
- Every public method on `CryptoService` returns `Result<T, CryptoError>`. No `throw` crosses module boundaries; internal helpers throw and are caught at the boundary in `index.ts`.
- Module-private state holders (`masterKek`, `pccDek`, `userDeks`) are closure-captured and never exported. Service object is `Object.freeze`d.
- All key/DEK/KEK/token buffers go through `withZeroized` or explicit `zeroize` in `finally`. The "rotateMaster" path tracks `newKek`/`newDek` separately so they're zeroized even on failure.

## Validation evidence

- `pnpm typecheck` — clean for `lib/crypto/**` and `tests/unit/crypto/**`. One pre-existing typecheck error remains in `lib/runtime/services-registry.ts` (AGENT-AUTH territory) where the registry casts `import('lib/crypto')` to a zero-arg-factory shape; the actual `createCryptoService` correctly takes a `CryptoBootstrap` because it needs the active `PccKeyWrap` row + pepper bytes + Keychain options to fulfill the `CryptoService` interface. AGENT-AUTH needs to update the registry to pass these. Documented here, not patched (out of my owned paths).
- `pnpm test tests/unit/crypto/` — **104/104 pass**.
- `pnpm test --coverage tests/unit/crypto/` — `lib/crypto/**` aggregate **99.21% lines, 100% functions, 93.75% branches** — above the **≥90%** SPEC §5 / Architecture §10 gate.
- `pnpm lint` — could not run cleanly due to a pre-existing project-level ESLint patching incompat (`eslint-config-next` + ESLint 9.39 + `@rushstack/eslint-patch`). This affects the entire repo, not crypto specifically. I deliberately did not modify `package.json` or eslint config (out of scope). Manual scan of `lib/crypto/**` and `tests/unit/crypto/**` for the brief's hard-prohibited patterns:
  - `Math.random` — none.
  - `pseudoRandomBytes` — none (only mentioned in a "no" comment in `kdf.ts`).
  - `console.log` — none.
  - `: any` / `as any` — exactly one instance, in `tests/unit/crypto/zeroize.test.ts:33`, with the required `eslint-disable-next-line` comment, scoped to a `Buffer.prototype.fill`-monkey-patch test for the detached-buffer code path.
  - `TODO` / `HACK` / `debugger` — none.
  - `throw new Error('... passphrase ...')` — none. Every thrown error message is fixed-text and never contains 'passphrase'.

## Brief literal-text deviation (worth the Orchestrator's attention)

The brief says: *"The string `passphrase` should not appear in any thrown/returned error message."* The `CryptoError` discriminated-union in `lib/types/domain.ts` (AGENT-ARCH owns; read-only for me) defines:

```ts
| { readonly kind: 'master_passphrase_unavailable' }
```

The string `passphrase` appears in this canonical enum kind. I read the brief's intent as "the master-passphrase **bytes** never leak into errors / logs / messages" — which is enforced exhaustively. The enum kind is a fixed structural tag from another agent's contract, not a free-form message, and it tells callers "the passphrase is unavailable" without revealing anything about it. Defensible default per Operating Constraints; flagged here for review. If the Orchestrator wants the enum tag renamed, that is an AGENT-ARCH change.

## Open / blocked

- `lib/runtime/services-registry.ts` (AGENT-AUTH) calls `createCryptoService()` with zero arguments. The `CryptoService` interface mandates `initializeFromKeychain()` and `loadUserDek()` — the factory needs a `CryptoBootstrap` (Keychain options, pepper bytes, active `PccKeyWrap` row, and rotation callbacks) to fulfill that contract. AGENT-AUTH (or boot.ts when AGENT-AUTH writes it) needs to construct the bootstrap and pass it through. I did not modify `services-registry.ts` — out of my owned paths.
- `pnpm lint` is broken project-wide due to ESLint-config-next vs ESLint 9 patching. This is not a crypto-layer issue and not within my owned paths.
- The PCC DEK version mismatch path on `encrypt`/`decrypt` is currently mapped to `pcc_dek_not_loaded` rather than a distinct `version_mismatch` kind. The `CryptoError` union doesn't have a `version_mismatch` kind; the closest semantic was "no key loaded for this version". If the Orchestrator wants a dedicated kind, that is also an AGENT-ARCH change.

## Files written

- `lib/crypto/{zeroize,kdf,aad,envelope,master-key,user-dek,pcc-dek,index}.ts`
- `tests/unit/crypto/{zeroize,kdf,aad,envelope,master-key,user-dek,pcc-dek}.test.ts`
- `vitest.config.ts`
- `docs/agents/AGENT-CRYPTO.md` (this file)

Total LOC (source + tests, excluding comments): ~1,800.

End of retrospective.
