#!/usr/bin/env tsx
// Greylock — pnpm admin:audit-verify
// =============================================================================
// Walks the audit chain seq-ascending and verifies every `entryHash`. Prints
// `OK seq=<N>` on success or `BROKEN at seq=<N>` on the first mismatch.
//
// Auditing this invocation: every run inserts an `admin_audit_verify_invoked`
// audit row with the verdict in `outcome`.
// =============================================================================

import { createAuditService } from '../lib/audit/index.js';

import { runAdmin } from './_admin-boot.js';

void runAdmin(async ({ booted }) => {
  const audit = createAuditService({ auditRepo: booted.repos.auditRepo });

  const result = await audit.verifyChain();

  const invocationDetails: Record<string, unknown> = {};
  if (result.ok) {
    invocationDetails['verifiedCount'] = result.value.verifiedCount;
  } else {
    invocationDetails['errorKind'] = result.error.kind;
    if (result.error.kind === 'chain_break') {
      invocationDetails['atSeq'] = String(result.error.atSeq);
    }
  }
  await audit.append({
    actorUserId: null,
    actorKind: 'admin_cli',
    domain: null,
    subjectId: null,
    subjectKind: null,
    action: 'admin_audit_verify_invoked',
    outcome: result.ok ? 'success' : 'failure',
    details: invocationDetails,
  });

  if (result.ok) {
    process.stdout.write(`OK verifiedCount=${String(result.value.verifiedCount)}\n`);
    return 0;
  }
  if (result.error.kind === 'chain_break') {
    process.stderr.write(`BROKEN at seq=${String(result.error.atSeq)}\n`);
  } else {
    process.stderr.write(`audit-verify failed: ${result.error.kind}\n`);
  }
  return 1;
});
