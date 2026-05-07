// Greylock — CryptoService factory + module-private state
// =============================================================================
// `createCryptoService` builds a concrete `CryptoService` whose Master KEK,
// PCC DEK, and per-user DEKs live in module-private closure variables.
// Nothing else in the codebase has a path to read these buffers.
//
// Public methods all return `Result<T, CryptoError>`. Internal helpers may
// throw; the boundary in this file catches.
// =============================================================================

import { randomBytes } from 'node:crypto';

import type {
  AadContext,
  CryptoService,
  KeyHandle,
} from '../types/services.js';
import type {
  CryptoError,
  Domain,
  EncryptedBlob,
  Result,
  UserId,
} from '../types/domain.js';
import {
  DomainTag,
  EncryptedBlob as EncryptedBlobCtor,
  Err,
  Ok,
} from '../types/domain.js';

import { aadForItemToken, aadForPccDekWrap, aadForUserDekWrap } from './aad.js';
import { isEnvelopeFailure, open as envelopeOpen, seal as envelopeSeal } from './envelope.js';
import { KEY_LENGTH_BYTES } from './kdf.js';
import {
  rotateMaster as rotatePure,
  unwrapPccDek,
  wrapPccDek,
  type PccItemTokenRow,
  type RotateMasterCallbacks,
} from './pcc-dek.js';
import { unwrapUserDek, wrapUserDek } from './user-dek.js';
import { zeroize } from './zeroize.js';
import { loadMasterKek, type KeychainFetchOptions } from './master-key.js';

// -----------------------------------------------------------------------------
// Bootstrap state — supplied by the caller (lib/runtime/boot.ts) so this file
// does not import the DB layer.
// -----------------------------------------------------------------------------

export interface CryptoBootstrap {
  /** Path of the active PccKeyWrap row. Caller fetches it from the repo. */
  readonly activePccKeyWrap: {
    readonly version: number;
    readonly wrappedDek: EncryptedBlob;
    readonly kdfSalt: Uint8Array;
  };
  /** Bytes of CRYPTO_PEPPER (caller decodes the env var). */
  readonly pepperBytes: Uint8Array;
  /** Keychain options — service name, fallback toggle. */
  readonly keychain: KeychainFetchOptions;
  /**
   * Hook invoked when rotateMaster is called. Provides callbacks the rotation
   * loop needs (read pcc item rows, persist new wrap + rewritten rows).
   * Caller is repo-aware; we are not.
   */
  readonly rotation?: {
    readonly fetchKeychainForNewMaster: () => Promise<KeychainFetchOptions>;
    readonly nextVersion: () => Promise<number>;
    readonly callbacks: RotateMasterCallbacks;
  };
}

export interface CreateCryptoServiceOptions {
  readonly bootstrap: CryptoBootstrap;
}

// -----------------------------------------------------------------------------
// Module-private state holder
// -----------------------------------------------------------------------------
// We use a closure (createCryptoService) to scope state per-instance, so the
// module is testable. There is no exported singleton; the runtime constructs
// one in boot.ts and passes the reference through DI.
// -----------------------------------------------------------------------------

interface InternalState {
  masterKek: Buffer | null;
  pccDek: Buffer | null;
  pccDekVersion: number | null;
  /** Per-user DEKs, keyed by branded UserId. */
  userDeks: Map<UserId, Buffer>;
  /** Per-user DEK versions, keyed by branded UserId. */
  userDekVersions: Map<UserId, number>;
  /** Set when `rotateMaster` is in progress so concurrent calls are rejected. */
  rotationInProgress: boolean;
}

function freshState(): InternalState {
  return {
    masterKek: null,
    pccDek: null,
    pccDekVersion: null,
    userDeks: new Map(),
    userDekVersions: new Map(),
    rotationInProgress: false,
  };
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

function buildAad(domain: Domain, ctx: AadContext, keyVersion: number): Uint8Array {
  switch (ctx.kind) {
    case 'item_token':
      return aadForItemToken({
        domain,
        itemId: ctx.itemId,
        keyVersion,
      });
    case 'user_dek_wrap':
      return aadForUserDekWrap({ userId: ctx.userId });
    case 'pcc_dek_wrap':
      return aadForPccDekWrap({ version: ctx.version });
  }
}

function expectedDomainTag(domain: Domain): typeof DomainTag.Personal | typeof DomainTag.Pcc {
  return domain === 'pcc' ? DomainTag.Pcc : DomainTag.Personal;
}

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------

export function createCryptoService(opts: CreateCryptoServiceOptions): CryptoService {
  const state = freshState();
  const boot = opts.bootstrap;

  async function initializeFromKeychain(): Promise<Result<void, CryptoError>> {
    if (state.masterKek !== null && state.pccDek !== null) {
      return Ok(undefined); // idempotent
    }

    const masterRes = await loadMasterKek(
      boot.keychain,
      boot.activePccKeyWrap.kdfSalt,
      boot.pepperBytes,
    );
    if (!masterRes.ok) {
      return Err({ kind: 'master_passphrase_unavailable' });
    }
    const newKek = masterRes.kek;
    // Unwrap the PCC DEK.
    const unwrapped = unwrapPccDek({
      masterKek: newKek,
      version: boot.activePccKeyWrap.version,
      wrappedDek: boot.activePccKeyWrap.wrappedDek,
    });
    if (!unwrapped.ok) {
      zeroize(newKek);
      return Err({ kind: unwrapped.kind });
    }
    // Commit to state.
    if (state.masterKek !== null) {
      zeroize(state.masterKek);
    }
    if (state.pccDek !== null) {
      zeroize(state.pccDek);
    }
    state.masterKek = newKek;
    state.pccDek = unwrapped.dek;
    state.pccDekVersion = boot.activePccKeyWrap.version;
    return Ok(undefined);
  }

  async function shutdown(): Promise<void> {
    if (state.masterKek !== null) {
      zeroize(state.masterKek);
      state.masterKek = null;
    }
    if (state.pccDek !== null) {
      zeroize(state.pccDek);
      state.pccDek = null;
    }
    state.pccDekVersion = null;
    for (const [, dek] of state.userDeks) {
      zeroize(dek);
    }
    state.userDeks.clear();
    state.userDekVersions.clear();
  }

  async function loadUserDek(input: {
    readonly userId: UserId;
    readonly credentialId: Uint8Array;
    readonly kekSalt: Uint8Array;
    readonly wrappedUserDek: EncryptedBlob;
    readonly userDekVersion: number;
  }): Promise<Result<void, CryptoError>> {
    try {
      const res = unwrapUserDek({
        userId: input.userId,
        credentialId: input.credentialId,
        kekSalt: input.kekSalt,
        pepperBytes: boot.pepperBytes,
        wrappedUserDek: input.wrappedUserDek,
      });
      if (!res.ok) {
        return Err({ kind: res.kind });
      }
      // Replace prior DEK if present (zeroize first).
      const prior = state.userDeks.get(input.userId);
      if (prior !== undefined) {
        zeroize(prior);
      }
      state.userDeks.set(input.userId, res.dek);
      state.userDekVersions.set(input.userId, input.userDekVersion);
      return Ok(undefined);
    } catch {
      return Err({ kind: 'kdf_failure' });
    }
  }

  async function unloadUserDek(userId: UserId): Promise<void> {
    const dek = state.userDeks.get(userId);
    if (dek !== undefined) {
      zeroize(dek);
      state.userDeks.delete(userId);
    }
    state.userDekVersions.delete(userId);
  }

  function hasUserDek(userId: UserId): boolean {
    return state.userDeks.has(userId);
  }

  function hasPccDek(): boolean {
    return state.pccDek !== null;
  }

  // ---------------------------------------------------------------------------
  // encrypt / decrypt
  // ---------------------------------------------------------------------------

  function resolveKeyForHandle(
    handle: KeyHandle,
  ): { readonly ok: true; readonly key: Buffer; readonly version: number } | { readonly ok: false; readonly err: CryptoError } {
    if (handle.kind === 'pcc') {
      if (state.pccDek === null) {
        return { ok: false, err: { kind: 'pcc_dek_not_loaded' } };
      }
      // Version mismatch on encrypt is a programmer error → treat as kdf_failure.
      // (Caller should have read the active version from boot.)
      if (state.pccDekVersion !== handle.version) {
        // Not exposed as `aad_mismatch` here — a wrong version requested means
        // there's no key to use, which is closer to "not loaded".
        return { ok: false, err: { kind: 'pcc_dek_not_loaded' } };
      }
      return { ok: true, key: state.pccDek, version: handle.version };
    }
    // user
    const dek = state.userDeks.get(handle.userId);
    if (dek === undefined) {
      return { ok: false, err: { kind: 'user_dek_not_loaded', userId: handle.userId } };
    }
    const v = state.userDekVersions.get(handle.userId);
    if (v === undefined || v !== handle.version) {
      return { ok: false, err: { kind: 'user_dek_not_loaded', userId: handle.userId } };
    }
    return { ok: true, key: dek, version: handle.version };
  }

  async function encrypt(input: {
    readonly handle: KeyHandle;
    readonly aad: AadContext;
    readonly domain: Domain;
    readonly plaintext: Uint8Array;
  }): Promise<Result<EncryptedBlob, CryptoError>> {
    const resolved = resolveKeyForHandle(input.handle);
    if (!resolved.ok) {
      return Err(resolved.err);
    }
    try {
      const aadBytes = buildAad(input.domain, input.aad, resolved.version);
      const blob = envelopeSeal({
        key: resolved.key,
        aad: aadBytes,
        plaintext: input.plaintext,
        domainTag: expectedDomainTag(input.domain),
      });
      return Ok(blob);
    } catch {
      return Err({ kind: 'kdf_failure' });
    }
  }

  async function decrypt(input: {
    readonly handle: KeyHandle;
    readonly aad: AadContext;
    readonly domain: Domain;
    readonly blob: EncryptedBlob;
  }): Promise<Result<Uint8Array, CryptoError>> {
    const resolved = resolveKeyForHandle(input.handle);
    if (!resolved.ok) {
      return Err(resolved.err);
    }
    let aadBytes: Uint8Array;
    try {
      aadBytes = buildAad(input.domain, input.aad, resolved.version);
    } catch {
      return Err({ kind: 'aad_mismatch' });
    }
    const opened = envelopeOpen({
      key: resolved.key,
      aad: aadBytes,
      blob: input.blob,
      expectedDomainTag: expectedDomainTag(input.domain),
    });
    if (isEnvelopeFailure(opened)) {
      return Err({ kind: opened.kind });
    }
    return Ok(opened);
  }

  // ---------------------------------------------------------------------------
  // wrapUserDek / rotateUserDek
  // ---------------------------------------------------------------------------

  async function wrapUserDekImpl(input: {
    readonly userId: UserId;
    readonly credentialId: Uint8Array;
    readonly kekSalt: Uint8Array;
    readonly version: number;
    readonly dekMaterial: Uint8Array;
  }): Promise<Result<EncryptedBlob, CryptoError>> {
    try {
      const blob = wrapUserDek({
        userId: input.userId,
        credentialId: input.credentialId,
        kekSalt: input.kekSalt,
        pepperBytes: boot.pepperBytes,
        dekMaterial: input.dekMaterial,
      });
      return Ok(blob);
    } catch {
      return Err({ kind: 'kdf_failure' });
    }
  }

  async function rotateUserDek(input: {
    readonly userId: UserId;
    readonly credentialId: Uint8Array;
    readonly kekSalt: Uint8Array;
    readonly currentVersion: number;
  }): Promise<Result<{ readonly newVersion: number; readonly wrapped: EncryptedBlob }, CryptoError>> {
    const newDek = randomBytes(KEY_LENGTH_BYTES);
    try {
      const newVersion = input.currentVersion + 1;
      const blob = wrapUserDek({
        userId: input.userId,
        credentialId: input.credentialId,
        kekSalt: input.kekSalt,
        pepperBytes: boot.pepperBytes,
        dekMaterial: newDek,
      });
      return Ok({ newVersion, wrapped: EncryptedBlobCtor.unsafeFromBytes(blob) });
    } catch {
      return Err({ kind: 'kdf_failure' });
    } finally {
      zeroize(newDek);
    }
  }

  // ---------------------------------------------------------------------------
  // rotateMaster
  // ---------------------------------------------------------------------------

  async function rotateMasterImpl(): Promise<Result<{ readonly oldVersion: number; readonly newVersion: number }, CryptoError>> {
    if (state.rotationInProgress) {
      return Err({ kind: 'rotation_in_progress' });
    }
    if (state.masterKek === null || state.pccDek === null || state.pccDekVersion === null) {
      return Err({ kind: 'pcc_dek_not_loaded' });
    }
    if (boot.rotation === undefined) {
      return Err({ kind: 'rotation_in_progress' });
    }
    state.rotationInProgress = true;
    const oldKek = state.masterKek;
    const oldDek = state.pccDek;
    const oldVersion = state.pccDekVersion;

    try {
      const nextV = await boot.rotation.nextVersion();
      const newKeychain = await boot.rotation.fetchKeychainForNewMaster();
      // We need a NEW kdfSalt for the new wrap. Generate it here.
      const newKdfSalt = randomBytes(16);
      let newKek: Buffer | null = null;
      let newDek: Buffer | null = null;
      try {
        const newMasterRes = await loadMasterKek(newKeychain, newKdfSalt, boot.pepperBytes);
        if (!newMasterRes.ok) {
          return Err({ kind: 'master_passphrase_unavailable' });
        }
        newKek = newMasterRes.kek;
        newDek = randomBytes(KEY_LENGTH_BYTES);
        const result = await rotatePure({
          oldMasterKek: oldKek,
          oldPccDek: oldDek,
          oldVersion,
          newMasterKek: newKek,
          newPccDekMaterial: newDek,
          newVersion: nextV,
          callbacks: boot.rotation.callbacks,
        });
        if (!result.ok) {
          switch (result.error.kind) {
            case 'tag_invalid':
              return Err({ kind: 'tag_invalid' });
            case 'aad_mismatch':
              return Err({ kind: 'aad_mismatch' });
            case 'malformed_blob':
              return Err({ kind: 'malformed_blob' });
            case 'persist_failed':
              return Err({ kind: 'kdf_failure' });
          }
        }
        // Commit: replace in-memory keys with the new ones. Zeroize old.
        zeroize(oldKek);
        zeroize(oldDek);
        // Swap into state — copy the new buffers because the local newKek/newDek
        // are zeroized by `finally` below.
        state.masterKek = Buffer.from(newKek);
        state.pccDek = Buffer.from(newDek);
        state.pccDekVersion = nextV;
        return Ok({ oldVersion, newVersion: nextV });
      } finally {
        if (newKek !== null) {
          zeroize(newKek);
        }
        if (newDek !== null) {
          zeroize(newDek);
        }
        zeroize(newKdfSalt);
      }
    } catch {
      return Err({ kind: 'kdf_failure' });
    } finally {
      state.rotationInProgress = false;
    }
  }

  // ---------------------------------------------------------------------------
  // Service object — note: closure-captured `state` is not exported, never
  // accessible from outside this factory.
  // ---------------------------------------------------------------------------

  return Object.freeze({
    initializeFromKeychain,
    shutdown,
    loadUserDek,
    unloadUserDek,
    hasUserDek,
    hasPccDek,
    encrypt,
    decrypt,
    wrapUserDek: wrapUserDekImpl,
    rotateUserDek,
    rotateMaster: rotateMasterImpl,
  });
}

// -----------------------------------------------------------------------------
// Pure helpers re-exported for callers that need them OUTSIDE the service
// instance (e.g. the bootstrap path that has to wrap a brand-new PCC DEK
// before the service exists, or admin-rotate-master which needs to construct
// a wrap blob from raw bytes).
// -----------------------------------------------------------------------------

export { wrapPccDek };
export type { CryptoService, KeyHandle, AadContext };
export type { PccItemTokenRow, RotateMasterCallbacks };
