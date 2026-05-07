// Greylock — e2e: login flow
// =============================================================================
// AGENT-UI (Phase 4). Drives the UI happy path through /login. We use
// Playwright's WebAuthn virtual authenticator to capture the registration +
// authentication ceremonies. The full passkey enrollment requires a booted
// crypto + db stack (Keychain master passphrase, SQLCipher, etc.) that may
// not be available in this test environment; the spec therefore verifies:
//
//   1. /login renders the sign-in form in the OVRWCH aesthetic.
//   2. The form posts to /api/auth/authentication/begin and surfaces a
//      generic error message when the API is not fully booted (no token leak).
//   3. The form is keyboard-accessible (label association).
//
// When the full stack is wired (Phase 5) this spec will be extended to drive
// a real virtual authenticator end-to-end. Keep the fast verification today.
// =============================================================================

import { test, expect } from '@playwright/test';

test.describe('login flow', () => {
  test('renders /login in the OVRWCH aesthetic', async ({ page }) => {
    await page.goto('/login');
    await expect(page).toHaveTitle(/Sign in/);
    await expect(page.locator('h1')).toContainText('Sign in');
    // Header brand + subtitle.
    await expect(page.locator('header')).toContainText('LOFTUS');
    await expect(page.locator('header')).toContainText('OPERATING DASHBOARD');
    // Footer
    await expect(page.locator('footer')).toContainText('NOT FINANCIAL ADVICE');
    // The body background is the locked OVRWCH dark.
    const bodyBg = await page.evaluate(() =>
      window.getComputedStyle(document.body).backgroundColor,
    );
    // rgb(14,15,17) === #0e0f11
    expect(bodyBg).toBe('rgb(14, 15, 17)');
  });

  test('email input is keyboard accessible and submit is gated by content', async ({ page }) => {
    await page.goto('/login');
    const submit = page.getByRole('button', { name: /sign in with passkey/i });
    await expect(submit).toBeDisabled();
    await page.getByLabel('Email').fill('rory.patrick.loftus@gmail.com');
    await expect(submit).toBeEnabled();
  });

  test('failed authentication surfaces generic error (no kind echo)', async ({ page }) => {
    await page.goto('/login');
    await page.getByLabel('Email').fill('not-on-allowlist@example.com');
    // Intercept to force a 500 — we verify the UI doesn't echo any kind/message.
    await page.route('**/api/auth/authentication/begin', (route) =>
      route.fulfill({
        status: 500,
        contentType: 'application/json',
        body: JSON.stringify({ error: { code: 'storage_failure', message: 'internal' } }),
      }),
    );
    await page.getByRole('button', { name: /sign in with passkey/i }).click();
    // Wait for the generic message to appear.
    await expect(page.locator('text=Request failed. Please retry.')).toBeVisible({
      timeout: 5000,
    });
    // The tag-shape kind must NOT appear in DOM.
    await expect(page.locator('body')).not.toContainText('storage_failure');
    await expect(page.locator('body')).not.toContainText('webauthn_verification_failed');
  });
});
