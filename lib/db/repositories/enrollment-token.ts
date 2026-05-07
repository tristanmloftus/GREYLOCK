// Greylock — EnrollmentTokenRepository
// =============================================================================
// AGENT-DB. Implements `EnrollmentTokenRepository` from
// `lib/auth/enrollment-token.ts` (the interface AGENT-AUTH owns). One-shot
// URL tokens for `pnpm admin:enroll`.
//
// Persistence rules (per agent brief + ARCHITECTURE.md):
//   - The CLEARTEXT token is NEVER stored: only `tokenHash = SHA-256(token)`.
//   - Hash comparison uses `crypto.timingSafeEqual` semantics: we look up by
//     the hash (which is a unique key) so the database does the equality
//     check on its own indexed comparison, not in user-space.
//   - `verify` does NOT consume the token; the caller (AGENT-AUTH route
//     handler) calls `consume()` only when the registration ceremony
//     successfully completes. This avoids accidentally burning a token on
//     transport errors.
//
// Mint helper is exported off-band (not in the interface) because AGENT-AUTH
// doesn't have a mint route in this phase; admin CLI uses `mintEnrollmentToken`
// directly to print the URL.
// =============================================================================

import { createHash, randomBytes } from 'node:crypto';

import type { PrismaClient } from '@prisma/client';

import { Err, Ok } from '../../types/domain.js';
import type { Result } from '../../types/domain.js';
import type {
  EnrollmentTokenError,
  EnrollmentTokenRepository,
  VerifiedEnrollmentToken,
} from '../../auth/enrollment-token.js';

import { asBuffer, mapPrismaError } from './_shared.js';

const DEFAULT_TOKEN_TTL_MINUTES = 60;
const TOKEN_BYTES = 32;

export interface CreateEnrollmentTokenRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createEnrollmentTokenRepository(
  input: CreateEnrollmentTokenRepositoryInput,
): EnrollmentTokenRepository {
  const { prisma } = input;

  return {
    async verify(args): Promise<Result<VerifiedEnrollmentToken, EnrollmentTokenError>> {
      const tokenBytes = decodeBase64Url(args.token);
      if (tokenBytes === null) {
        return Err({ kind: 'token_not_found' });
      }
      const hash = sha256(tokenBytes);
      try {
        const row = await prisma.enrollmentToken.findUnique({
          where: { tokenHash: asBuffer(hash) },
          select: {
            id: true,
            email: true,
            createdAt: true,
            expiresAt: true,
            usedAt: true,
          },
        });
        if (row === null) {
          return Err({ kind: 'token_not_found' });
        }
        if (row.usedAt !== null) {
          return Err({ kind: 'token_already_used' });
        }
        if (row.expiresAt.getTime() <= Date.now()) {
          return Err({ kind: 'token_expired' });
        }
        return Ok({
          tokenId: row.id,
          email: row.email,
          issuedAt: row.createdAt,
          expiresAt: row.expiresAt,
        });
      } catch (cause: unknown) {
        const mapped = mapPrismaError(cause);
        if (mapped.kind === 'not_found') {
          return Err({ kind: 'token_not_found' });
        }
        return Err({ kind: 'storage_failure' });
      }
    },

    async consume(args): Promise<Result<void, EnrollmentTokenError>> {
      try {
        const result = await prisma.$transaction(async (tx) => {
          const row = await tx.enrollmentToken.findUnique({
            where: { id: args.tokenId },
            select: { id: true, usedAt: true, expiresAt: true },
          });
          if (row === null) {
            return { kind: 'not_found' as const };
          }
          if (row.usedAt !== null) {
            return { kind: 'already_used' as const };
          }
          if (row.expiresAt.getTime() <= Date.now()) {
            return { kind: 'expired' as const };
          }
          await tx.enrollmentToken.update({
            where: { id: row.id },
            data: { usedAt: new Date() },
          });
          return { kind: 'ok' as const };
        });
        switch (result.kind) {
          case 'ok':
            return Ok(undefined);
          case 'not_found':
            return Err({ kind: 'token_not_found' });
          case 'already_used':
            return Err({ kind: 'token_already_used' });
          case 'expired':
            return Err({ kind: 'token_expired' });
        }
      } catch (cause: unknown) {
        const mapped = mapPrismaError(cause);
        if (mapped.kind === 'not_found') {
          return Err({ kind: 'token_not_found' });
        }
        return Err({ kind: 'storage_failure' });
      }
    },
  };
}

// -----------------------------------------------------------------------------
// Off-band helpers — used by `pnpm admin:enroll` to mint, and tests.
// -----------------------------------------------------------------------------

export interface MintedEnrollmentToken {
  readonly tokenId: string;
  /** base64url-encoded random bytes; carried in the URL. NEVER LOGGED. */
  readonly cleartextToken: string;
  readonly email: string;
  readonly expiresAt: Date;
}

export async function mintEnrollmentToken(args: {
  readonly prisma: PrismaClient;
  readonly email: string;
  readonly ttlMinutes?: number;
}): Promise<Result<MintedEnrollmentToken, { kind: 'storage_failure' }>> {
  const tokenBytes = randomBytes(TOKEN_BYTES);
  const tokenHash = sha256(tokenBytes);
  const expiresAt = new Date(Date.now() + (args.ttlMinutes ?? DEFAULT_TOKEN_TTL_MINUTES) * 60_000);
  try {
    const row = await args.prisma.enrollmentToken.create({
      data: {
        email: args.email.trim().toLowerCase(),
        tokenHash: asBuffer(tokenHash),
        expiresAt,
      },
      select: { id: true, email: true, expiresAt: true },
    });
    return Ok({
      tokenId: row.id,
      cleartextToken: encodeBase64Url(tokenBytes),
      email: row.email,
      expiresAt: row.expiresAt,
    });
  } catch (cause: unknown) {
    const mapped = mapPrismaError(cause);
    if (mapped.kind === 'storage_failure' || mapped.kind === 'conflict') {
      return Err({ kind: 'storage_failure' });
    }
    return Err({ kind: 'storage_failure' });
  } finally {
    // Best-effort scrub of the random buffer in this scope. The hash bytes
    // and the cleartext bytes are then released to the GC.
    tokenBytes.fill(0);
  }
}

// -----------------------------------------------------------------------------
// Encoding helpers (base64url + sha256)
// -----------------------------------------------------------------------------

function sha256(b: Uint8Array): Uint8Array {
  return new Uint8Array(createHash('sha256').update(b).digest());
}

function encodeBase64Url(b: Uint8Array): string {
  return Buffer.from(b)
    .toString('base64')
    .replace(/\+/g, '-')
    .replace(/\//g, '_')
    .replace(/=+$/u, '');
}

function decodeBase64Url(s: string): Uint8Array | null {
  // Accept only the base64url alphabet to keep the parser tight.
  if (!/^[A-Za-z0-9_-]+$/u.test(s)) {
    return null;
  }
  const padded = s.replace(/-/g, '+').replace(/_/g, '/');
  const rem = padded.length % 4;
  const padding = rem === 0 ? '' : '='.repeat(4 - rem);
  try {
    return new Uint8Array(Buffer.from(padded + padding, 'base64'));
  } catch {
    return null;
  }
}
