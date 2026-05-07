// Greylock — Plaid SDK initialization
// =============================================================================
// AGENT-PLAID (Phase 3). Reads Plaid configuration from env once and
// constructs a singleton `PlaidApi` instance. The Plaid client_id and secret
// are sent to Plaid via headers (per Plaid docs); they are NEVER logged from
// this module.
//
// Hard rules enforced here:
//   - PLAID_SECRET is read from env exactly once and passed to the SDK. We do
//     not echo it to console / errors / audit anywhere.
//   - PLAID_ENV is validated against Plaid's known environments (sandbox /
//     development / production). v0.1 ships in sandbox per SPEC §4 decision 6.
// =============================================================================

import { Configuration, PlaidApi, PlaidEnvironments } from 'plaid';

import type { CountryCode, Products } from 'plaid';

// -----------------------------------------------------------------------------
// Config — locked at module load time
// -----------------------------------------------------------------------------

export interface PlaidEnvConfig {
  readonly clientId: string;
  readonly secret: string;
  readonly environment: 'sandbox' | 'development' | 'production';
  readonly products: ReadonlyArray<'transactions' | 'auth' | 'identity'>;
  readonly countryCodes: ReadonlyArray<string>;
}

const VALID_PRODUCTS: ReadonlySet<string> = new Set(['transactions', 'auth', 'identity']);

function readPlaidEnv(): PlaidEnvConfig {
  const clientId = process.env['PLAID_CLIENT_ID'];
  const secret = process.env['PLAID_SECRET'];
  const env = process.env['PLAID_ENV'] ?? 'sandbox';
  const productsRaw = process.env['PLAID_PRODUCTS'] ?? 'transactions';
  const countryCodesRaw = process.env['PLAID_COUNTRY_CODES'] ?? 'US';

  if (clientId === undefined || clientId.length === 0) {
    throw new Error('PLAID_CLIENT_ID is not set');
  }
  if (secret === undefined || secret.length === 0) {
    throw new Error('PLAID_SECRET is not set');
  }
  if (env !== 'sandbox' && env !== 'development' && env !== 'production') {
    throw new Error(`PLAID_ENV must be sandbox|development|production (got "${env}")`);
  }

  const products = productsRaw
    .split(',')
    .map((s) => s.trim())
    .filter((s) => s.length > 0);
  for (const p of products) {
    if (!VALID_PRODUCTS.has(p)) {
      throw new Error(`PLAID_PRODUCTS contains invalid product "${p}"`);
    }
  }
  const countryCodes = countryCodesRaw
    .split(',')
    .map((s) => s.trim())
    .filter((s) => s.length > 0);

  return {
    clientId,
    secret,
    environment: env,
    products: products as ReadonlyArray<'transactions' | 'auth' | 'identity'>,
    countryCodes,
  };
}

// -----------------------------------------------------------------------------
// Singleton PlaidApi
// -----------------------------------------------------------------------------

let cachedClient: PlaidApi | null = null;
let cachedConfig: PlaidEnvConfig | null = null;

/**
 * Construct (or return the cached) `PlaidApi` instance. The instance carries
 * the client_id / secret as headers internally; they are not exposed via any
 * of the exported helpers.
 */
export function getPlaidClient(): PlaidApi {
  if (cachedClient !== null) {
    return cachedClient;
  }
  const cfg = readPlaidEnv();
  const basePath = PlaidEnvironments[cfg.environment];
  if (basePath === undefined) {
    throw new Error(`PLAID_ENV "${cfg.environment}" not in PlaidEnvironments`);
  }
  const configuration = new Configuration({
    basePath,
    baseOptions: {
      headers: {
        'PLAID-CLIENT-ID': cfg.clientId,
        'PLAID-SECRET': cfg.secret,
        'Plaid-Version': '2020-09-14',
      },
    },
  });
  cachedClient = new PlaidApi(configuration);
  cachedConfig = cfg;
  return cachedClient;
}

/**
 * Return the parsed Plaid config (sans secrets). Useful for routes that need
 * to know which products / countries are configured.
 */
export function getPlaidConfig(): { readonly products: ReadonlyArray<'transactions' | 'auth' | 'identity'>; readonly countryCodes: ReadonlyArray<string>; readonly environment: 'sandbox' | 'development' | 'production' } {
  if (cachedConfig === null) {
    cachedConfig = readPlaidEnv();
  }
  return {
    products: cachedConfig.products,
    countryCodes: cachedConfig.countryCodes,
    environment: cachedConfig.environment,
  };
}

/**
 * Test-only: reset the cached client and config. Production code never calls
 * this.
 */
export function __resetPlaidClientForTests(): void {
  cachedClient = null;
  cachedConfig = null;
}

// Re-export Plaid SDK enum/types we use elsewhere.
export type { CountryCode, Products };
export { PlaidApi };
