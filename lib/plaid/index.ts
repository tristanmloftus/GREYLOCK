// Greylock — Plaid module barrel
// =============================================================================
// AGENT-PLAID (Phase 3). Export surface for callers (route handlers + sync
// orchestrator). The factory wires the broker + service together.
// =============================================================================

import type { PlaidApi } from 'plaid';

import type {
  AccountRepository,
  AuditService,
  CryptoService,
  ItemRepository,
  PccMembershipRepository,
  PlaidService,
  TransactionRepository,
  UserRepository,
} from '../types/services.js';

import type { PccKeyWrapRepository } from '../db/index.js';

import { createPlaidTokenBroker } from './token-broker.js';
import { createPlaidService } from './service.js';

export interface CreatePlaidServiceInput {
  readonly plaidClient: PlaidApi;
  readonly crypto: CryptoService;
  readonly itemRepo: ItemRepository;
  readonly accountRepo: AccountRepository;
  readonly transactionRepo: TransactionRepository;
  readonly userRepo: UserRepository;
  readonly pccMembershipRepo: PccMembershipRepository;
  readonly pccKeyWrapRepo: PccKeyWrapRepository;
  readonly audit: AuditService;
  readonly clientName: string;
  readonly countryCodes: ReadonlyArray<string>;
  readonly defaultProducts: ReadonlyArray<'transactions' | 'auth' | 'identity'>;
}

/**
 * Wire the Plaid token broker + service. Route handlers and the sync
 * orchestrator obtain this via the runtime registry.
 */
export function createPlaidServiceWithBroker(input: CreatePlaidServiceInput): PlaidService {
  const tokenBroker = createPlaidTokenBroker({
    crypto: input.crypto,
    itemRepo: input.itemRepo,
    userRepo: input.userRepo,
    pccKeyWrapRepo: input.pccKeyWrapRepo,
    audit: input.audit,
  });
  return createPlaidService({
    plaidClient: input.plaidClient,
    crypto: input.crypto,
    itemRepo: input.itemRepo,
    accountRepo: input.accountRepo,
    transactionRepo: input.transactionRepo,
    userRepo: input.userRepo,
    pccMembershipRepo: input.pccMembershipRepo,
    pccKeyWrapRepo: input.pccKeyWrapRepo,
    tokenBroker,
    audit: input.audit,
    clientName: input.clientName,
    countryCodes: input.countryCodes,
    defaultProducts: input.defaultProducts,
  });
}

export { createPlaidTokenBroker } from './token-broker.js';
export { createPlaidService } from './service.js';
export { getPlaidClient, getPlaidConfig, __resetPlaidClientForTests } from './client.js';
export type { PlaidServiceDeps } from './service.js';
export type { PlaidTokenBrokerDeps } from './token-broker.js';
