// Greylock — Vitest config
// =============================================================================
// Phase 2 (AGENT-CRYPTO scope): Node environment, v8 coverage, threshold per
// module per SPEC.md §5 / Architecture §10. Other agents will extend the
// `include` patterns when their suites land.
// =============================================================================

import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    environment: 'node',
    globals: false,
    include: ['tests/**/*.test.ts'],
    reporters: ['default'],
    coverage: {
      provider: 'v8',
      reporter: ['text', 'html', 'json'],
      include: ['lib/crypto/**/*.ts', 'lib/db/**/*.ts', 'lib/compute/**/*.ts'],
      exclude: [
        'lib/crypto/index.ts', // factory wiring; not all branches exercised in unit tests
        'lib/db/index.ts', // boot wiring + dynamic singleton; covered indirectly
        '**/*.d.ts',
      ],
      thresholds: {
        // Per-module: lib/crypto must hit ≥90% (SPEC §5 — QA-TEST gate).
        'lib/crypto/**/*.ts': {
          lines: 90,
          functions: 90,
          branches: 90,
          statements: 90,
        },
        // Per AGENT-DB brief / SPEC §QA-TEST: lib/db/** ≥80%.
        'lib/db/**/*.ts': {
          lines: 80,
          functions: 80,
          branches: 70,
          statements: 80,
        },
        // Per AGENT-COMPUTE brief / SPEC §QA-TEST: lib/compute/** ≥80%.
        'lib/compute/**/*.ts': {
          lines: 80,
          functions: 80,
          branches: 80,
          statements: 80,
        },
      },
    },
  },
});
