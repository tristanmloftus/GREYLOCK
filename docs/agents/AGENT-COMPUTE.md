# AGENT-COMPUTE — Phase 3 retrospective

> Pure-function compute layer: net worth, cash, month-net, $1B progress, and
> USD currency helpers. No I/O. No `Date.now()`. No `crypto`. Fully
> unit-testable from fixtures. AGENT-PLAID, AGENT-SYNC, AGENT-AUDIT-LOG ran
> in parallel against the same Phase 3 commit — none of them depend on
> compute internals; they all consume `ComputeService` from `lib/types/services`.

---

## 1. Files shipped

| File | Purpose |
|---|---|
| `lib/compute/net-worth.ts` | `netWorth({accounts}) -> NetWorthResult`. Sums assets vs liabilities, derives cash subset, builds per-account breakdown. |
| `lib/compute/cash-only.ts` | `cashOnly({accounts}) -> Cents`. Strict-positive depository balances only. |
| `lib/compute/month-net.ts` | `monthNet({transactions, now}) -> MonthNetResult`. 30-day rolling window. |
| `lib/compute/billion-progress.ts` | `billionProgress({netWorthCents}) -> BillionProgressResult`. Bigint-first division for precision. |
| `lib/compute/currency.ts` | USD helpers: `toCents`, `centsToDisplay`, `centsAbs`. |
| `lib/compute/index.ts` | `createComputeService()` factory + barrel exports. |
| `tests/fixtures/compute/builders.ts` | `buildAccount` / `buildTransaction` helpers — used only by tests. |
| `tests/fixtures/compute/{empty,single-checking,mixed-positive,all-zero,negative-nw,transactions-30day}.ts` | Six golden fixtures covering empty/positive/negative/boundary scenarios. |
| `tests/unit/compute/{net-worth,cash-only,month-net,billion-progress,currency,index}.test.ts` | 90 unit tests across all surfaces. |
| `vitest.config.ts` | Extended `coverage.include` to add `lib/compute/**` and a ≥80% threshold (compute clears it at 100%). |

## 2. Decisions made (with reasoning)

### "Other" account type counts as an asset

Plaid's `other` category is a residual — HSA accounts, brokerage variants the
classifier can't pin down, gift cards, prepaid card balances, etc. The brief
left this open ("count as asset by default; document the choice"). Decision:
treat `other` as an asset (positive contribution) but **not** as cash. This
matches the operator's intent ("things I own count toward net worth") and
errs on the optimistic side, which is the right default for an `other`
classification on accounts the user explicitly connected. The choice is
documented inline in `net-worth.ts` and asserted by a dedicated test
(`netWorth — type contributions > treats `other` as an asset`).

### Sign convention preserved on `Account.currentBalanceCents` for liabilities

For credit cards / loans, `currentBalanceCents = 5000n` means "you owe $50".
The plaintext bigint stays positive in storage; `netWorth` is responsible
for flipping the sign at compute time (`netWorth = assets - liabilities`).
This keeps `Account` storage uniform — every row carries a non-negative
"how much money is in/owed on this account" — and concentrates sign logic
in one pure function. A regression here surfaces as a single clearly
identifiable test failure rather than as scattered storage bugs.

### `monthNet` window is `[now - 30d, now)` — closed-open

Matches the standard "rolling 30-day" UX expectation: the boundary at
`now - 30d` is inclusive (a transaction stamped at exactly that instant
counts), and the boundary at `now` is exclusive (a transaction stamped at
exactly `now` belongs to the *next* tick, not this one). Two boundary
fixtures (`txAtStartBoundary`, `txAtNowBoundary`) and a `txOneMsBeforeStart`
fixture pin this down byte-for-byte; future refactors that drift the
boundary fail those tests immediately.

### `billionProgress` uses bigint-first scaling

Naive `Number(nw) / Number(goal)` would silently lose precision once
`netWorthCents > 9.007e15` (≈ $90T). Far above v0.1 needs, but the brief
explicitly called out the precision check, so we use
`Number(nw * 10000n / goalCents) / 10000`. The bigint multiply happens
before the divide, preserving 4 decimal places regardless of magnitude. A
test (`billionProgress — large-value precision > preserves precision for
nw > 1e15 cents`) exercises a $50T net worth value to lock this in.

### `toCents` parses lexically, never via `Number`

The whole point of the bigint discipline is to never let a float touch a
money value. `toCents` slices the dollar string at the decimal point, runs
two regex digit checks, and constructs the result via `BigInt(dollarsPart)
* 100n + BigInt(centsPart)`. No `parseFloat`, no `Number(...)`. Inputs like
`"1e3"`, `"NaN"`, `"Infinity"`, `"$1.00"`, `"1,234.56"` all `throw`
explicitly per the brief ("rejects bad input with explicit `throw`"). 38
tests cover the accepted/rejected matrix.

### Pure function bodies + small `createComputeService` factory

The architecture brief said `lib/compute/*` is pure. The `ComputeService`
interface in `lib/types/services.ts` is a thin shim over those functions,
so the factory is a 4-line object literal. Callers can either import the
named function directly (preferred for tests) or take the service via DI
(preferred for route handlers and the snapshot writer). Both paths share
the same code.

## 3. Validation evidence

```
$ pnpm typecheck                       # → 0 errors in lib/compute/** + tests/(unit|fixtures)/compute/**
$ pnpm test tests/unit/compute/        # → 6 files, 90 tests, all passing
$ pnpm test --coverage tests/unit/compute/
   compute     |  100  |  100  |  100  |  100   (lines / branches / functions / statements)
$ npx eslint lib/compute tests/unit/compute tests/fixtures/compute
                                       # → 0 errors, 0 warnings
```

Pre-existing failures from other agents' modules (`lib/ipc`, `lib/plaid`,
`lib/sync`, `scripts/db`) are visible from a project-wide `pnpm typecheck`
and `pnpm lint`, but none come from compute or its tests.

## 4. Coverage by file

| File | Lines | Branches | Functions |
|---|---|---|---|
| `net-worth.ts` | 100% | 100% | 100% |
| `cash-only.ts` | 100% | 100% | 100% |
| `month-net.ts` | 100% | 100% | 100% |
| `billion-progress.ts` | 100% | 100% | 100% |
| `currency.ts` | 100% | 100% | 100% |
| `index.ts` | 100% | 100% | 100% |

Total: 100% on every metric. Cleared the SPEC §5 ≥80% gate by 20 points.

## 5. Anti-patterns — self-rejection log

Every brief-mandated anti-pattern is enforced by code or tests:

| Anti-pattern | Where the violation would surface |
|---|---|
| Any I/O (`fs`, `process.env`, `Date.now()`) | None — the only external value used is `input.now: Date` passed by the caller. |
| Any `crypto` | None imported. |
| Any `console.log` | None present (would also be flagged by ESLint `no-console`). |
| `number` for money | Every Cents value is `bigint`. The single `Number()` call in `billion-progress.ts` is on the *progress ratio*, not on cents. |
| Float comparisons | None — `toCents` parses lexically, all bigint comparisons use `<`, `<=`, `>`, `>=` against `0n`. |
| Unrounded float→bigint | None — no float sources. |
| Mutating input arrays | Asserted in `netWorth — input not mutated` and `monthNet — input not mutated`. |

## 6. Open hand-off items

- **AGENT-SYNC** consumes `createComputeService()` in `lib/sync/snapshot-writer.ts`. Today the snapshot writer signature in the architecture doc shows the writer calls compute and writes a `NetWorthSnapshot`. Compute returns plain values; the writer's responsibility is to map them into the snapshot row (including `breakdownJson` — `JSON.stringify` over the `breakdown` array works as long as bigints are pre-stringified). This is a shape note, not a contract change.
- **AGENT-UI** can import `centsToDisplay` for every money-rendering site. The function handles sign + currency placement deterministically (e.g., `-$0.50` not `$-0.50`). UI does NOT need its own formatter.
- **`other` account behavior** is locked at the function level. If we ever decide to make `other` count toward cash (e.g. HSAs the operator wants liquid), it's a one-line change in `net-worth.ts` plus a test flip — no schema or downstream consumer impact.
- **Multi-currency** (SPEC §4 decision 7: USD-only at v0.1). When this opens up, `currency.ts` is the obvious target — split into per-currency formatters keyed by `IsoCurrencyCode`. The pure-function discipline means there is no I/O migration to coordinate.

---

End of AGENT-COMPUTE retro.
