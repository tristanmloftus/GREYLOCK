#!/usr/bin/env tsx
// Greylock — pnpm admin:revoke-all
// =============================================================================
// Revoke every active session. Used after a lost-laptop incident or a master-
// passphrase rotation. Does NOT revoke passkeys (use admin-revoke per-user
// for that). Emits one `admin_revoke_all_invoked` audit row.
//
// Usage: pnpm admin:revoke-all [--reason "..."]
// =============================================================================

import { createAuditService } from '../lib/audit/index.js';

import { findFlag, runAdmin } from './_admin-boot.js';

void runAdmin(async ({ booted }) => {
  const reason = findFlag('reason') ?? 'admin_revoke_all';

  const result = await booted.repos.sessionRepo.revokeAllActive({ reason });
  const audit = createAuditService({ auditRepo: booted.repos.auditRepo });

  if (!result.ok) {
    process.stderr.write(`revoke-all failed: ${result.error.kind}\n`);
    await audit.append({
      actorUserId: null,
      actorKind: 'admin_cli',
      domain: null,
      subjectId: null,
      subjectKind: null,
      action: 'admin_revoke_all_invoked',
      outcome: 'failure',
      details: { reason, errorKind: result.error.kind },
    });
    return 1;
  }

  await audit.append({
    actorUserId: null,
    actorKind: 'admin_cli',
    domain: null,
    subjectId: null,
    subjectKind: null,
    action: 'admin_revoke_all_invoked',
    outcome: 'success',
    details: { reason, count: result.value.count },
  });

  process.stdout.write(`revoked ${String(result.value.count)} active session(s) (reason=${reason})\n`);
  return 0;
});
