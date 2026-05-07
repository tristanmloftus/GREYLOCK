// Greylock — compute service factory + barrel
// =============================================================================
// AGENT-COMPUTE. Pure functions, fully unit-testable from fixtures.
// `createComputeService()` returns a `ComputeService` whose methods just
// delegate to the per-feature pure functions in this directory.
// =============================================================================

import type { ComputeService } from '../types/services.js';

import { billionProgress } from './billion-progress.js';
import { cashOnly } from './cash-only.js';
import { monthNet } from './month-net.js';
import { netWorth } from './net-worth.js';

export { billionProgress } from './billion-progress.js';
export { cashOnly } from './cash-only.js';
export { centsAbs, centsToDisplay, toCents } from './currency.js';
export type { CentsToDisplayOptions } from './currency.js';
export { monthNet } from './month-net.js';
export { netWorth } from './net-worth.js';

export const createComputeService = (): ComputeService => ({
  netWorth,
  cashOnly,
  monthNet,
  billionProgress,
});
