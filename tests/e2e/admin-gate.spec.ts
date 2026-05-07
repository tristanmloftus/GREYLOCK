// Greylock — e2e: admin route gating
// =============================================================================
// AGENT-UI (Phase 4). The owner-gated admin pages return 404 for non-owners
// (no role enumeration). Without a session the pages redirect to /login.
//
// We verify:
//   1. Unauthenticated GET /admin redirects to /login.
//   2. /admin/audit and /admin/items behave the same.
// Phase 5 (with full crypto stack) will add an authenticated non-owner spec
// asserting 404 instead of 403.
// =============================================================================

import { test, expect } from '@playwright/test';

test.describe('admin gate', () => {
  for (const path of ['/admin', '/admin/enroll', '/admin/audit', '/admin/items']) {
    test(`unauthenticated ${path} redirects to /login`, async ({ page }) => {
      await page.goto(path);
      await page.waitForLoadState('domcontentloaded');
      expect(page.url()).toMatch(/\/login$/);
    });
  }
});
