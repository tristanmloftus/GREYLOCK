// Greylock — @simplewebauthn/server thin wrappers
// =============================================================================
// AGENT-AUTH (Phase 2). Centralizes the SimpleWebAuthn API so the rest of
// `lib/auth/` (and route handlers) never imports the library directly. Every
// call is forced to:
//   - userVerification: 'required'   (passkey UV gesture mandatory)
//   - residentKey:      'required'   (discoverable credential)
//   - attestation:      'none'       (no attestation chain processing)
//
// RP configuration is read from process.env once per call so misconfiguration
// surfaces at the route boundary as a hard error rather than at module load.
// =============================================================================

import {
  generateAuthenticationOptions as swaGenerateAuthOptions,
  generateRegistrationOptions as swaGenerateRegOptions,
  verifyAuthenticationResponse as swaVerifyAuthResponse,
  verifyRegistrationResponse as swaVerifyRegResponse,
} from '@simplewebauthn/server';
import { isoBase64URL } from '@simplewebauthn/server/helpers';

import type {
  AuthenticationResponseFromBrowser,
  RegistrationResponseFromBrowser,
} from '../types/services.js';

// SimpleWebAuthn re-exports its types only via subpath imports that have no
// `exports` mapping. We mirror the minimal shapes here to avoid coupling.
type Base64URLString = string;
type AuthenticatorTransportFuture =
  | 'internal'
  | 'hybrid'
  | 'usb'
  | 'nfc'
  | 'ble'
  | 'cable'
  | 'smart-card';
interface WebAuthnCredentialShape {
  id: Base64URLString;
  publicKey: Uint8Array;
  counter: number;
  transports?: AuthenticatorTransportFuture[];
}

// -----------------------------------------------------------------------------
// RP configuration — fetched per-call so tests can override env between cases
// -----------------------------------------------------------------------------

export interface RpConfig {
  readonly rpID: string;
  readonly rpName: string;
  readonly rpOrigin: string;
}

export function readRpConfig(): RpConfig {
  const rpID = process.env['WEBAUTHN_RP_ID'];
  const rpName = process.env['WEBAUTHN_RP_NAME'];
  const rpOrigin = process.env['WEBAUTHN_RP_ORIGIN'];
  if (rpID === undefined || rpID.length === 0) {
    throw new Error('WEBAUTHN_RP_ID is not set');
  }
  if (rpName === undefined || rpName.length === 0) {
    throw new Error('WEBAUTHN_RP_NAME is not set');
  }
  if (rpOrigin === undefined || rpOrigin.length === 0) {
    throw new Error('WEBAUTHN_RP_ORIGIN is not set');
  }
  return { rpID, rpName, rpOrigin };
}

// -----------------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------------

export interface BeginRegistrationInput {
  readonly userId: Uint8Array;
  readonly userName: string;
  readonly userDisplayName: string;
  readonly excludeCredentials: ReadonlyArray<{
    readonly id: Base64URLString;
    readonly transports?: ReadonlyArray<AuthenticatorTransportFuture>;
  }>;
  readonly timeoutMs?: number;
}

export interface RegistrationOptionsResult {
  readonly challenge: string;
  readonly rp: { readonly id: string; readonly name: string };
  readonly user: { readonly id: string; readonly name: string; readonly displayName: string };
  readonly pubKeyCredParams: ReadonlyArray<{ readonly type: 'public-key'; readonly alg: number }>;
  readonly timeout?: number;
}

export async function beginRegistration(
  input: BeginRegistrationInput,
): Promise<RegistrationOptionsResult> {
  const rp = readRpConfig();
  const opts = await swaGenerateRegOptions({
    rpID: rp.rpID,
    rpName: rp.rpName,
    userID: input.userId,
    userName: input.userName,
    userDisplayName: input.userDisplayName,
    timeout: input.timeoutMs ?? 60_000,
    attestationType: 'none',
    excludeCredentials: input.excludeCredentials.map((c) => ({
      id: c.id,
      ...(c.transports !== undefined ? { transports: [...c.transports] } : {}),
    })),
    authenticatorSelection: {
      residentKey: 'required',
      userVerification: 'required',
      requireResidentKey: true,
    },
  });
  return {
    challenge: opts.challenge,
    rp: { id: opts.rp.id ?? rp.rpID, name: opts.rp.name ?? rp.rpName },
    user: { id: opts.user.id, name: opts.user.name, displayName: opts.user.displayName },
    pubKeyCredParams: opts.pubKeyCredParams.map((p) => ({
      type: 'public-key' as const,
      alg: p.alg,
    })),
    ...(opts.timeout !== undefined ? { timeout: opts.timeout } : {}),
  };
}

export interface VerifyRegistrationInput {
  readonly response: RegistrationResponseFromBrowser;
  readonly expectedChallenge: string;
}

export interface VerifiedRegistration {
  readonly verified: boolean;
  readonly credentialId: Uint8Array;
  readonly credentialPublicKey: Uint8Array;
  readonly counter: bigint;
  readonly aaguid: Uint8Array | null;
  readonly transports: ReadonlyArray<string>;
}

export async function verifyRegistration(
  input: VerifyRegistrationInput,
): Promise<VerifiedRegistration> {
  const rp = readRpConfig();
  const result = await swaVerifyRegResponse({
    response: {
      id: input.response.id,
      rawId: input.response.rawId,
      response: {
        attestationObject: input.response.response.attestationObject,
        clientDataJSON: input.response.response.clientDataJSON,
        ...(input.response.response.transports !== undefined
          ? {
              transports: input.response.response.transports as AuthenticatorTransportFuture[],
            }
          : {}),
      },
      clientExtensionResults: input.response.clientExtensionResults,
      type: input.response.type,
      ...(input.response.authenticatorAttachment !== undefined
        ? { authenticatorAttachment: input.response.authenticatorAttachment as 'platform' | 'cross-platform' }
        : {}),
    },
    expectedChallenge: input.expectedChallenge,
    expectedOrigin: rp.rpOrigin,
    expectedRPID: rp.rpID,
    requireUserVerification: true,
  });

  if (!result.verified || result.registrationInfo === undefined) {
    return {
      verified: false,
      credentialId: new Uint8Array(0),
      credentialPublicKey: new Uint8Array(0),
      counter: 0n,
      aaguid: null,
      transports: [],
    };
  }

  const info = result.registrationInfo;
  const credentialId = isoBase64URL.toBuffer(info.credential.id);
  const transports = info.credential.transports ?? [];
  const aaguidBytes = parseAaguidString(info.aaguid);
  return {
    verified: true,
    credentialId,
    credentialPublicKey: info.credential.publicKey,
    counter: BigInt(info.credential.counter),
    aaguid: aaguidBytes,
    transports: [...transports],
  };
}

/** AAGUID is returned as a UUID-ish hex string with dashes. Parse to 16 bytes,
 *  or return null if the format is not recognized. */
function parseAaguidString(s: string): Uint8Array | null {
  const hex = s.replace(/-/gu, '');
  if (hex.length !== 32 || !/^[0-9a-fA-F]{32}$/u.test(hex)) {
    return null;
  }
  const out = new Uint8Array(16);
  for (let i = 0; i < 16; i += 1) {
    const byte = hex.slice(i * 2, i * 2 + 2);
    out[i] = parseInt(byte, 16);
  }
  return out;
}

// -----------------------------------------------------------------------------
// Authentication
// -----------------------------------------------------------------------------

export interface BeginAuthenticationInput {
  readonly allowCredentials: ReadonlyArray<{
    readonly id: Base64URLString;
    readonly transports?: ReadonlyArray<AuthenticatorTransportFuture>;
  }>;
  readonly timeoutMs?: number;
}

export interface AuthenticationOptionsResult {
  readonly challenge: string;
  readonly rpId?: string;
  readonly timeout?: number;
  readonly userVerification?: 'required' | 'preferred' | 'discouraged';
  readonly allowCredentials?: ReadonlyArray<{ readonly id: string; readonly type?: 'public-key' }>;
}

export async function beginAuthentication(
  input: BeginAuthenticationInput,
): Promise<AuthenticationOptionsResult> {
  const rp = readRpConfig();
  const opts = await swaGenerateAuthOptions({
    rpID: rp.rpID,
    timeout: input.timeoutMs ?? 60_000,
    userVerification: 'required',
    allowCredentials: input.allowCredentials.map((c) => ({
      id: c.id,
      ...(c.transports !== undefined ? { transports: [...c.transports] } : {}),
    })),
  });
  return {
    challenge: opts.challenge,
    ...(opts.rpId !== undefined ? { rpId: opts.rpId } : { rpId: rp.rpID }),
    ...(opts.timeout !== undefined ? { timeout: opts.timeout } : {}),
    userVerification: 'required',
    ...(opts.allowCredentials !== undefined
      ? {
          allowCredentials: opts.allowCredentials.map((c) => ({
            id: c.id,
            type: 'public-key' as const,
          })),
        }
      : {}),
  };
}

export interface VerifyAuthenticationInput {
  readonly response: AuthenticationResponseFromBrowser;
  readonly expectedChallenge: string;
  readonly credential: {
    readonly id: Base64URLString;
    readonly publicKey: Uint8Array;
    readonly counter: bigint;
    readonly transports?: ReadonlyArray<string>;
  };
}

export interface VerifiedAuthentication {
  readonly verified: boolean;
  readonly newCounter: bigint;
  readonly userVerified: boolean;
}

export async function verifyAuthentication(
  input: VerifyAuthenticationInput,
): Promise<VerifiedAuthentication> {
  const rp = readRpConfig();
  // SimpleWebAuthn stores counter as `number`; we keep the domain `bigint` so
  // we never silently truncate. Counter values from real authenticators are
  // 32-bit unsigned and well below Number.MAX_SAFE_INTEGER, so the conversion
  // is safe — but assert it explicitly to surface impossible corruption.
  if (input.credential.counter > BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new Error('credential.counter exceeds Number.MAX_SAFE_INTEGER');
  }
  const swaCred: WebAuthnCredentialShape = {
    id: input.credential.id,
    publicKey: input.credential.publicKey,
    counter: Number(input.credential.counter),
    ...(input.credential.transports !== undefined
      ? { transports: [...(input.credential.transports as AuthenticatorTransportFuture[])] }
      : {}),
  };
  const result = await swaVerifyAuthResponse({
    response: {
      id: input.response.id,
      rawId: input.response.rawId,
      response: {
        authenticatorData: input.response.response.authenticatorData,
        clientDataJSON: input.response.response.clientDataJSON,
        signature: input.response.response.signature,
        ...(input.response.response.userHandle !== undefined
          ? { userHandle: input.response.response.userHandle }
          : {}),
      },
      clientExtensionResults: input.response.clientExtensionResults,
      type: input.response.type,
      ...(input.response.authenticatorAttachment !== undefined
        ? { authenticatorAttachment: input.response.authenticatorAttachment as 'platform' | 'cross-platform' }
        : {}),
    },
    expectedChallenge: input.expectedChallenge,
    expectedOrigin: rp.rpOrigin,
    expectedRPID: rp.rpID,
    credential: swaCred,
    requireUserVerification: true,
  });

  return {
    verified: result.verified,
    newCounter: BigInt(result.authenticationInfo.newCounter),
    userVerified: result.authenticationInfo.userVerified,
  };
}

// -----------------------------------------------------------------------------
// Counter-monotonicity gate (RFC 6.1.1)
// -----------------------------------------------------------------------------

/** Return true iff `newCounter > storedCounter`, OR both are zero (some
 *  authenticators never increment the counter; that case is allowed only when
 *  *both* sides are zero). Returns false (replay suspected) otherwise. */
export function isCounterMonotonic(args: {
  readonly storedCounter: bigint;
  readonly newCounter: bigint;
}): boolean {
  if (args.storedCounter === 0n && args.newCounter === 0n) {
    return true;
  }
  return args.newCounter > args.storedCounter;
}

// -----------------------------------------------------------------------------
// Encoding helpers re-exported for the rest of `lib/auth/`
// -----------------------------------------------------------------------------

export const base64UrlFromBytes = (b: Uint8Array): string => isoBase64URL.fromBuffer(b);
export const bytesFromBase64Url = (s: string): Uint8Array => isoBase64URL.toBuffer(s);
