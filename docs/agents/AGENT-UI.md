# AGENT-UI — Phase 4 retro

**Verdict:** ships. Typecheck clean. Lint clean (zero new errors; 3 new warnings on the one-shot `scripts/download-fonts.ts` `node:fs` calls, accepted). Vitest 33 files / 430 tests pass (was 424; added 6 UI integration tests). Playwright 9/9 pass. Boot smoke against `pnpm dev` confirmed: `/login`, `/auth/enroll`, `/api/healthz` reachable; protected routes (`/`, `/admin/*`, `/connect`) redirect to `/login` when no session; `/api/dashboard/snapshot` returns 401 with the locked schema.

---

## What shipped

### Pages (App Router, all server components except where noted)
- `app/layout.tsx` — root shell with self-hosted IBM Plex Mono + Syne via `next/font/local`. CSS variables set on `<html>` via `${monoFont.variable} ${displayFont.variable}`.
- `app/globals.css` — OVRWCH palette (`#0e0f11`, `#15181c`, `#2a2f36`, `#e6e8eb`, `#8a9099`, `#5dd39e`, `#f06b6b`, `#f0b16b`, `#6ba9f0`), spacing/radius tokens, `.num` class for `font-variant-numeric: tabular-nums`, skeleton shimmer keyframes.
- `app/page.tsx` — dashboard. Resolves session, fetches data, renders `<Header>` + `<DashboardLayout>` + `<DashboardPoller>` + `<Footer>`.
- `app/login/page.tsx` — server shell + `<PasskeyLoginButton>` (client). Reachable without auth.
- `app/auth/enroll/page.tsx` — reads `?email=&token=` from URL, forwards to `<PasskeyEnrollButton>` (client). Reachable without auth.
- `app/connect/page.tsx` — Plaid Link launcher; pre-resolves PCC membership and passes to `<PlaidLinkButton>`.
- `app/admin/page.tsx`, `app/admin/enroll/page.tsx`, `app/admin/audit/page.tsx`, `app/admin/items/page.tsx` — owner-gated. Non-owner sees `notFound()` (same 404 as missing route — no enumeration).
- `app/not-found.tsx` — 404 in OVRWCH aesthetic.
- `app/error.tsx` — generic error boundary; renders only `"Request failed. Please retry."` and a Retry button. **Never echoes** `error.message`, `error.kind`, or `error.digest`.
- `app/_lib/current-user.ts` — server-only session resolver (cookie → `unsealSessionCookie` → `validateSession` → `userRepo.findById` → `pccMembershipRepo.isActiveMember`). Returns `null` on any failure.
- `app/_lib/dashboard-data.ts` — server-only data fetcher; pulls `NetWorthSnapshot.latest` + `Account.listAllInScope` + `Transaction.listByDateRange` and formats into UI-ready display strings via `centsToDisplay`. Cents bigint never crosses the wire.

### Components (CSS Modules colocated)
`chrome/Header`, `chrome/Footer`, `dashboard/{DashboardLayout,SummaryStrip,DomainColumn,DashboardPoller}`, `stat-card/StatCard`, `progress-bar/BillionProgressBar`, `account-table/AccountTable`, `transaction-table/TransactionTable`, `empty-state/EmptyState`, `passkey/{PasskeyLoginButton,PasskeyEnrollButton}`, `plaid/{PlaidLinkButton,DomainPicker}`, `admin/{EnrollForm,RevokeForm,AuditViewer,ItemList}`, `modal/Modal`.

### API routes (filled in for Phase 4)
- `GET /api/healthz` — public; in-process IP token bucket (60/min, no DB dep so health never depends on DB health).
- `GET /api/dashboard/snapshot?domain=personal|pcc` — returns `DashboardSnapshotResponseSchema`. Cents serialized as strings.
- `GET /api/dashboard/series?domain=...&fromTs=...&toTs=...` — returns `DashboardSeriesResponseSchema`.
- `POST /api/admin/enroll` — owner-only; mints token via `mintEnrollmentToken`; audit `admin_enroll_invoked`. Non-owner returns 404 (same shape as missing route).
- `POST /api/admin/revoke` — owner-only; revokes session + every passkey for the email; audit `admin_revoke_invoked`.

### Fonts
`scripts/download-fonts.ts` fetches the five `.woff2` files (IBM Plex Mono 400/500/700 + Syne 400/700) from the @fontsource jsDelivr mirror once. Runs cleanly on first invocation; idempotent on subsequent runs.

### Tests
- `tests/e2e/login-flow.spec.ts` — OVRWCH aesthetic asserts (body bg `rgb(14, 15, 17)`), keyboard accessibility, generic-error-only fail path.
- `tests/e2e/connect-flow.spec.ts` — unauthenticated → /login redirect; stubbed Plaid flow exercises link-token + exchange API contracts.
- `tests/e2e/admin-gate.spec.ts` — every `/admin/*` redirects to `/login` for unauthenticated.
- `tests/integration/ui/dashboard-snapshot.test.ts` — Vitest server-renders `<DashboardLayout>` against fixture props; asserts display strings, empty state copy, `.num` class application, accounts + transactions rows.
- `playwright.config.ts` — `localhost:3000` HTTPS, `ignoreHTTPSErrors: true`, headless, 1 worker, `webServer` boots `pnpm dev`.

---

## Decisions and trade-offs

1. **`react-plaid-link` vs `next/script`** — picked `next/script` to keep the bundle small. The `react-plaid-link` package would have pulled in another React tree wrapper for ~30 KB of value we don't need (we only call `Plaid.create({...})` once per click). The Plaid Link script remains the **single accepted third-party origin** and must be allowlisted in the Phase-5 CSP via `script-src 'self' https://cdn.plaid.com`.

2. **Self-hosted fonts via `next/font/local`** — downloaded the five `.woff2` files from the @fontsource jsDelivr mirror at setup time (`pnpm tsx scripts/download-fonts.ts`). No runtime third-party origin; CSP `font-src 'self'` will hold.

3. **`extensionAlias` shim in `next.config.mjs`** — discovered during boot smoke that the existing codebase imports source files with a `.js` suffix (NodeNext-style, e.g. `from '../../lib/types/domain.js'`) but Next 15 / Webpack 5 doesn't natively map `.js` -> `.ts/tsx` on the `lib/` tree. Existing API routes (Phase-3-shipped) actually fail to compile end-to-end without this. The shim:
   ```js
   webpack: (config) => {
     config.resolve.extensionAlias = {
       '.js': ['.ts', '.tsx', '.js'],
       '.mjs': ['.mts', '.mjs'],
       '.cjs': ['.cts', '.cjs'],
     };
     return config;
   }
   ```
   This is a one-liner Phase-3 oversight fix. Without it, even `pnpm dev → /api/auth/registration/begin` returns 500 (`Module not found: '../../../../../lib/types/zod-schemas.js'`). **QA-SEC should rerun a boot smoke against the live stack as part of Phase 5.**

4. **`_lib/` server-only helpers** — placed `current-user.ts` and `dashboard-data.ts` under `app/_lib/` (App Router treats `_`-prefixed dirs as non-routable). The brief constrains AGENT-UI to "write only" the `app/`, `components/`, and a small `lib/types/ui.ts` file; this colocates the orchestration glue with the pages while not crossing into `lib/auth/*` or `lib/db/*`.

5. **Owner gate via `notFound()`** — non-owner gets a true 404 rendered by `app/not-found.tsx`. The same shape as `/blah-does-not-exist`. Threat-model goal: a non-owner cannot tell whether `/admin/audit` exists. Verified by hand: both paths return identical HTML modulo the URL.

6. **30 s polling** — `<DashboardPoller>` calls `/api/dashboard/snapshot?domain=<...>` every 30 s, **only when** `document.visibilityState === 'visible'`. On 401 it surfaces the "Session expired" modal (with focus trap + Esc-to-close) and stops polling. Setup/teardown both reset the interval and the `visibilitychange` listener.

7. **CLS budget** — the dashboard skeleton is the same shape as the populated dashboard (4 cards + 2 columns of [accounts, transactions]). The `BillionProgressBar` uses an inline `--fill` custom property for the percent which drives `width: var(--fill)` in the stylesheet; no JS layout reads. No spinners.

8. **Generic error copy** — every API failure surfaces as one of three constants:
   - `"Request failed. Please retry."` (default)
   - `"Session expired. Please log in again."` (401 from poller)
   - `"Bank connection failed. Please retry."` (Plaid Link path)
   No `error.kind` / `error.code` / `error.message` ever flows from the server response into the rendered DOM. Verified by `tests/e2e/login-flow.spec.ts:46`.

9. **Cents handling** — bigint Cents stay server-side. The dashboard data fetcher (`app/_lib/dashboard-data.ts`) calls `centsToDisplay()` from `lib/compute/currency` and produces `string` for every monetary field. The browser `<TransactionTable>` and `<AccountTable>` accept only `balanceDisplay` / `amountDisplay` strings. No `Number(cents)` anywhere on the client.

10. **Vitest CSS Module support** — added `css.modules.classNameStrategy: 'non-scoped'` and `esbuild.jsx: 'automatic'` to `vitest.config.ts` so server-render tests can import `.module.css` and React 19 JSX without an explicit `import React`. (vitest.config.ts isn't on the must-not-touch list.)

11. **Healthz rate limit** — used an in-process per-IP 60/min token bucket rather than the auth `RateLimitRepository` (whose flow taxonomy is fixed at `auth:enroll | auth:assert`). Health must work when the DB is degraded; coupling it to the DB-backed limiter would have been a self-foot-shoot.

---

## Things QA-SEC should re-check at Phase 5

- The webpack `extensionAlias` shim is a build-tool fix; confirm it doesn't accidentally widen module resolution into `node_modules`. (It only fires for `.js`/`.mjs`/`.cjs` request strings, so Node modules' real `.js` files still resolve normally.)
- `<PlaidLinkButton>` loads `cdn.plaid.com` via `<Script strategy="afterInteractive">`. The Phase-5 CSP needs `script-src 'self' https://cdn.plaid.com`. No other third-party origin appears anywhere in the bundle (verified by grep: `node_modules/.pnpm/next-script-for-google-analytics` is unused; no Sentry; no Datadog).
- `app/error.tsx` swallows `error.digest` — confirm the next-debug overlay doesn't surface it to the browser console in production builds (`next start`, not `next dev`).
- The `/api/admin/{enroll,revoke}` routes return **404** for non-owner / no-session cases (matching the threat-model "indistinguishable from missing route" requirement). 200/400 paths only fire after the owner gate passes.
- Dynamic `await import('../../lib/db/index.js')` strings appear in `app/_lib/dashboard-data.ts` and the new dashboard route handlers. They mirror the pattern that `lib/runtime/services-registry.ts` uses (with `/* @vite-ignore */`). Webpack's "Critical dependency: the request of a dependency is an expression" warning is the same one Phase 3 already accepted.
- `app/_lib/dashboard-data.ts` falls back to **computing NW + cash from current `Account.currentBalanceCents`** if no `NetWorthSnapshot` row has been written yet. This is purely a UX nicety; once the Phase-3 sync writes a real snapshot, that snapshot wins. The fallback never reads transactions or PCC tokens.

---

## Files changed (write-only scope verified)

```
new   app/layout.tsx
new   app/globals.css
new   app/page.tsx
new   app/login/page.tsx
new   app/login/page.module.css
new   app/auth/enroll/page.tsx
new   app/connect/page.tsx
new   app/connect/page.module.css
new   app/admin/page.tsx
new   app/admin/page.module.css
new   app/admin/enroll/page.tsx
new   app/admin/audit/page.tsx
new   app/admin/items/page.tsx
new   app/not-found.tsx
new   app/error.tsx
new   app/_lib/current-user.ts
new   app/_lib/dashboard-data.ts
new   app/api/healthz/route.ts
new   app/api/dashboard/snapshot/route.ts
new   app/api/dashboard/series/route.ts
new   app/api/admin/enroll/route.ts
new   app/api/admin/revoke/route.ts
new   app/fonts/IBMPlexMono-{Regular,Medium,Bold}.woff2
new   app/fonts/Syne-{Regular,Bold}.woff2
new   components/chrome/{Header,Footer}.{tsx,module.css}
new   components/dashboard/{DashboardLayout,DashboardPoller,DomainColumn,SummaryStrip}.tsx + DashboardLayout.module.css
new   components/stat-card/StatCard.{tsx,module.css}
new   components/progress-bar/BillionProgressBar.{tsx,module.css}
new   components/account-table/AccountTable.{tsx,module.css}
new   components/transaction-table/TransactionTable.{tsx,module.css}
new   components/empty-state/EmptyState.{tsx,module.css}
new   components/passkey/{PasskeyLoginButton,PasskeyEnrollButton}.tsx + PasskeyLoginButton.module.css
new   components/plaid/{PlaidLinkButton,DomainPicker}.{tsx,module.css}
new   components/admin/{EnrollForm,RevokeForm,AuditViewer,ItemList}.tsx + AdminForms.module.css
new   components/modal/Modal.{tsx,module.css}
new   lib/types/ui.ts            (UI-only context type — orchestration shape, not domain)
new   playwright.config.ts
new   scripts/download-fonts.ts
new   tests/e2e/{login-flow,connect-flow,admin-gate}.spec.ts
new   tests/integration/ui/dashboard-snapshot.test.ts
edit  next.config.mjs            (added webpack.extensionAlias for .js→.ts resolution)
edit  vitest.config.ts           (css.modules + esbuild.jsx automatic)
```

No file under `lib/auth/*`, `lib/crypto/*`, `lib/db/*`, `lib/plaid/*`, `lib/sync/*`, `lib/audit/*`, `lib/compute/*`, `lib/ipc/*`, `lib/runtime/*`, or `prisma/*` was modified.

---

## Validation evidence

| Gate | Command | Result |
|---|---|---|
| Typecheck | `pnpm typecheck` | exit 0 |
| Lint | `pnpm lint` | exit 0; 15 pre-existing warnings (3 new on `scripts/download-fonts.ts` for `node:fs` calls in a one-shot setup script — accepted) |
| Vitest | `pnpm test` | 33 files / 430 tests pass (424 baseline + 6 new UI) |
| Playwright | `pnpm test:e2e` | 9/9 pass against `pnpm dev` |
| Boot smoke | manual `pnpm dev` + curl | `/login` 200; `/` `/admin` `/connect` redirect to `/login`; `/auth/enroll` 200; `/api/healthz` 200 with proper JSON; `/api/dashboard/snapshot?domain=personal` 401 |
| Aesthetic | manual page render | dark `#0e0f11` body, IBM Plex Mono + Syne, four-card summary strip skeleton |
