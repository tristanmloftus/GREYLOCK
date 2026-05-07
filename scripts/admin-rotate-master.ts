#!/usr/bin/env tsx
// Greylock — pnpm admin:rotate-master
// =============================================================================
// Rotate the master passphrase. Atomic: re-wraps the PCC DEK under a new
// Master KEK derived from the new passphrase; re-encrypts every PCC item
// access-token under the new wrap version. The PCC DEK bytes themselves do
// NOT change — only their wrap.
//
// Phase 3 v0.1: dev-only. The full production rotation requires interactive
// TTY prompts for the new passphrase + macOS Keychain update. Both are
// deferred to Phase 5 hardening (operators run the rotation through the
// hardened path then). For dev, this script prints "not yet implemented in
// dev boot mode" and exits non-zero.
//
// What IS implemented in v0.1:
//   - The crypto primitive lives in `lib/crypto/pcc-dek.ts:rotatePccDek`.
//   - The repo flow lives in `lib/db/repositories/pcc-key-wrap.ts` and
//     `lib/db/repositories/item.ts:rewriteEncryptedToken`.
//   - QA-SEC Phase 2 §M-3 carry-forward: at Phase 5 we wire (1) TTY prompt,
//     (2) Keychain update via `security add-generic-password -U`,
//     (3) re-encryption loop in a `$transaction`.
//
// Usage: pnpm admin:rotate-master
// =============================================================================

import { runAdmin } from './_admin-boot.js';

void runAdmin(async () => {
  process.stderr.write(
    [
      'admin:rotate-master is not yet implemented in v0.1.0-alpha.0.',
      '',
      'The crypto primitive (lib/crypto/pcc-dek.ts:rotatePccDek) and the repo',
      'flow (lib/db/repositories/pcc-key-wrap.ts) are ready. The remaining',
      'wiring — TTY prompt for the new passphrase, macOS Keychain update via',
      "  security add-generic-password -s greylock-master -a $USER -w '<new>' -U",
      'and the per-item re-encryption loop — is Phase 5 hardening work',
      '(QA-SEC Phase 2 §M-3 carry-forward).',
      '',
      'For now, do not rotate the master passphrase. If you must, do so by',
      'running the web app with NODE_ENV=production once the Phase 5 rotation',
      'flow ships.',
      '',
    ].join('\n'),
  );
  return 1;
});
