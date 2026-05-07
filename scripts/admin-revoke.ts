#!/usr/bin/env tsx
// Greylock — pnpm admin:revoke <email>
// =============================================================================
// Revoke a single user's active sessions and revoke their passkey(s). Use
// after losing a device. The user must be re-enrolled via `pnpm admin:enroll`
// to regain access.
//
// Usage: pnpm admin:revoke <email> [--reason "..."]
// =============================================================================

import { createAuditService } from '../lib/audit/index.js';

import { findFlag, requireArg, runAdmin } from './_admin-boot.js';

const USAGE = 'usage: pnpm admin:revoke <email> [--reason "..."]';

void runAdmin(async ({ booted }) => {
  const email = requireArg(2, 'email', USAGE);
  const reason = findFlag('reason') ?? 'admin_revoke';

  const audit = createAuditService({ auditRepo: booted.repos.auditRepo });

  const userRes = await booted.repos.userRepo.findByEmail(email.trim().toLowerCase());
  if (!userRes.ok) {
    process.stderr.write(`revoke failed: ${userRes.error.kind}\n`);
    return 1;
  }
  if (userRes.value === null) {
    process.stderr.write(`revoke: no user with email ${email}\n`);
    await audit.append({
      actorUserId: null,
      actorKind: 'admin_cli',
      domain: null,
      subjectId: null,
      subjectKind: 'user',
      action: 'admin_revoke_invoked',
      outcome: 'failure',
      details: { reason, errorKind: 'not_found' },
    });
    return 1;
  }
  const user = userRes.value;

  // Revoke active session for this user (single-session-per-user model means
  // there is at most one).
  const activeRes = await booted.repos.sessionRepo.findActiveByUser(user.id);
  let revokedSessions = 0;
  if (activeRes.ok && activeRes.value !== null) {
    const r = await booted.repos.sessionRepo.revoke({ id: activeRes.value.id, reason });
    if (r.ok) {
      revokedSessions = 1;
    }
  }

  // Revoke every passkey for this user.
  const passkeysRes = await booted.repos.passkeyRepo.listByUser(user.id);
  let revokedPasskeys = 0;
  if (passkeysRes.ok) {
    for (const pk of passkeysRes.value) {
      if (pk.revokedAt !== null) {
        continue;
      }
      const r = await booted.repos.passkeyRepo.revoke({ id: pk.id });
      if (r.ok) {
        revokedPasskeys += 1;
      }
    }
  }

  await audit.append({
    actorUserId: user.id,
    actorKind: 'admin_cli',
    domain: null,
    subjectId: user.id,
    subjectKind: 'user',
    action: 'admin_revoke_invoked',
    outcome: 'success',
    details: { reason, revokedSessions, revokedPasskeys },
  });

  process.stdout.write(
    `revoked: user=${user.email} sessions=${String(revokedSessions)} passkeys=${String(revokedPasskeys)} (reason=${reason})\n`,
  );
  return 0;
});
