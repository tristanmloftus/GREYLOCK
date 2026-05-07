#!/usr/bin/env tsx
// Greylock — pnpm admin:enroll <email>
// =============================================================================
// Mint a one-shot passkey-enrollment URL for an allowlisted email. The URL
// embeds a token whose hash is stored in the `EnrollmentToken` table; the
// cleartext token is printed once to stdout and never persisted.
//
// Usage: pnpm admin:enroll <email> [--name "Display Name"] [--role owner|member]
// =============================================================================

import { createAuditService } from '../lib/audit/index.js';
import { isAllowedEmail, isPlaceholderEmail, normalizeEmail } from '../lib/auth/allowlist.js';
import { mintEnrollmentToken } from '../lib/db/index.js';
import { UserId } from '../lib/types/domain.js';

import { findFlag, requireArg, runAdmin } from './_admin-boot.js';

const USAGE =
  'usage: pnpm admin:enroll <email> [--name "Display Name"] [--role owner|member]';

const ENROLLMENT_TTL_MIN = 30;

void runAdmin(async ({ booted }) => {
  const emailRaw = requireArg(2, 'email', USAGE);
  const email = normalizeEmail(emailRaw);
  const displayName = findFlag('name') ?? email.split('@')[0] ?? email;
  const roleArg = findFlag('role') ?? 'member';
  if (roleArg !== 'owner' && roleArg !== 'member') {
    process.stderr.write(`bad role: ${roleArg} (must be owner|member)\n`);
    return 2;
  }
  const role = roleArg as 'owner' | 'member';

  const audit = createAuditService({ auditRepo: booted.repos.auditRepo });

  // Allowlist + placeholder gates.
  if (isPlaceholderEmail(email)) {
    process.stderr.write(`enroll refused: ${email} is a placeholder address\n`);
    await audit.append({
      actorUserId: null,
      actorKind: 'admin_cli',
      domain: null,
      subjectId: null,
      subjectKind: 'user',
      action: 'passkey_enrollment_rejected',
      outcome: 'denied',
      details: { reason: 'placeholder_email', email },
    });
    return 2;
  }
  if (!isAllowedEmail(email)) {
    process.stderr.write(`enroll refused: ${email} is not in ALLOWED_EMAILS\n`);
    await audit.append({
      actorUserId: null,
      actorKind: 'admin_cli',
      domain: null,
      subjectId: null,
      subjectKind: 'user',
      action: 'passkey_enrollment_rejected',
      outcome: 'denied',
      details: { reason: 'not_allowlisted', email },
    });
    return 2;
  }

  // Owner role only allowed for OWNER_EMAIL.
  const ownerEmail = (process.env['OWNER_EMAIL'] ?? '').trim().toLowerCase();
  if (role === 'owner' && email !== ownerEmail) {
    process.stderr.write(`enroll refused: 'owner' role only for OWNER_EMAIL\n`);
    return 2;
  }

  // Upsert User row.
  const existing = await booted.repos.userRepo.findByEmail(email);
  if (!existing.ok) {
    process.stderr.write(`enroll: ${existing.error.kind}\n`);
    return 1;
  }
  let userId: string;
  if (existing.value !== null) {
    userId = existing.value.id;
  } else {
    const created = await booted.repos.userRepo.create({ email, displayName, role });
    if (!created.ok) {
      process.stderr.write(`enroll: ${created.error.kind}\n`);
      return 1;
    }
    userId = created.value.id;
  }

  // Mint the enrollment token via the lib/db helper. Persists only the hash;
  // the cleartext is returned once and printed below.
  const mintRes = await mintEnrollmentToken({
    prisma: booted.prisma,
    email,
    ttlMinutes: ENROLLMENT_TTL_MIN,
  });
  if (!mintRes.ok) {
    process.stderr.write(`enroll: mint failed: ${mintRes.error.kind}\n`);
    return 1;
  }
  const minted = mintRes.value;

  const appUrl = process.env['APP_URL'] ?? 'https://localhost:3000';
  const url = `${appUrl}/auth/enroll?email=${encodeURIComponent(email)}&token=${minted.cleartextToken}`;

  const brandedUserId = UserId(userId);
  await audit.append({
    actorUserId: brandedUserId,
    actorKind: 'admin_cli',
    domain: null,
    subjectId: userId,
    subjectKind: 'user',
    action: 'admin_enroll_invoked',
    outcome: 'success',
    details: { email, role, ttlMinutes: ENROLLMENT_TTL_MIN },
  });

  process.stdout.write(`\nEnrollment URL (valid ${String(ENROLLMENT_TTL_MIN)}m, one-shot):\n\n  ${url}\n\n`);
  process.stdout.write(`Send this to ${email} via a private channel.\n`);
  process.stdout.write('The cleartext token is NOT stored — only its SHA-256 hash.\n');
  return 0;
});
