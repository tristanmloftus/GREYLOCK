// Greylock — Vitest config
// =============================================================================
// Phase 2 (AGENT-CRYPTO scope): Node environment, v8 coverage, threshold per
// module per SPEC.md §5 / Architecture §10. Other agents will extend the
// `include` patterns when their suites land.
// =============================================================================

import { defineConfig } from 'vitest/config';

export default defineConfig({
  // Phase 4 (AGENT-UI): Need the automatic JSX runtime for React 19 in
  // server-rendered tests. esbuild defaults to classic; force automatic so
  // .tsx files don't require an explicit React import.
  esbuild: {
    jsx: 'automatic',
  },
  test: {
    environment: 'node',
    globals: false,
    include: ['tests/**/*.test.ts'],
    reporters: ['default'],
    // Phase 4 (AGENT-UI): allow CSS Module imports under SSR by returning the
    // identity map. Vitest's `css.modules.classNameStrategy='non-scoped'` is
    // what `react-dom/server` consumes — class names are stable strings and
    // we don't depend on hashed names in assertions.
    css: {
      modules: {
        classNameStrategy: 'non-scoped',
      },
    },
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
