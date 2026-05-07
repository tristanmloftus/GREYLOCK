// Greylock — Playwright config
// =============================================================================
// AGENT-UI (Phase 4). Localhost HTTPS, ignore self-signed cert, single worker
// (the dev server is single-process and the e2e flows mutate global app state).
//
// `webServer` boots `pnpm dev` automatically when the spec runs and reuses
// it on subsequent runs.
// =============================================================================

import { defineConfig, devices } from '@playwright/test';

const PORT = 3000;
const BASE_URL = `https://localhost:${PORT}`;

export default defineConfig({
  testDir: './tests/e2e',
  fullyParallel: false,
  retries: 0,
  workers: 1,
  reporter: 'list',
  timeout: 30_000,
  use: {
    baseURL: BASE_URL,
    ignoreHTTPSErrors: true,
    headless: true,
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
  },
  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
    },
  ],
  webServer: {
    command: 'pnpm dev',
    url: BASE_URL,
    reuseExistingServer: true,
    timeout: 60_000,
    ignoreHTTPSErrors: true,
  },
});
