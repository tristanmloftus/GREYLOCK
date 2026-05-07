// Greylock — AccountRepository (scope-by-construction via Item join)
// =============================================================================
// AGENT-DB. Account visibility piggy-backs on Item visibility: an account is
// visible to a scope iff its parent Item is visible. We express that via a
// `where: { item: { is: itemScopeFilter(scope) } }` relation predicate so the
// SAME scope rule is enforced on every read and every write.
// =============================================================================

import type { PrismaClient, Account as PrismaAccount, Prisma } from '@prisma/client';

import { Err, Ok, AccountId, ItemId, UserId } from '../../types/domain.js';
import type {
  Account,
  Cents,
  Domain,
  IsoCurrencyCode,
  RepoError,
  Result,
} from '../../types/domain.js';
import type { AccountRepository, RepoScope } from '../../types/services.js';

import {
  asPlaidAccountId,
  itemScopeFilter,
  mapPrismaError,
  requirePccMembershipOrNotFound,
  tryDb,
} from './_shared.js';

function toDomain(row: PrismaAccount): Account {
  return {
    id: AccountId(row.id),
    itemId: ItemId(row.itemId),
    domain: row.domain as Domain,
    userId: row.userId === null ? null : UserId(row.userId),
    plaidAccountId: asPlaidAccountId(row.plaidAccountId),
    name: row.name,
    officialName: row.officialName,
    mask: row.mask,
    type: row.type as Account['type'],
    subtype: row.subtype,
    isoCurrencyCode: row.isoCurrencyCode as IsoCurrencyCode,
    currentBalanceCents: row.currentBalanceCents,
    availableBalanceCents: row.availableBalanceCents,
    limitCents: row.limitCents,
    balanceUpdatedAt: row.balanceUpdatedAt,
    createdAt: row.createdAt,
    updatedAt: row.updatedAt,
    closedAt: row.closedAt,
  };
}

export interface CreateAccountRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createAccountRepository(input: CreateAccountRepositoryInput): AccountRepository {
  const { prisma } = input;

  function whereForScope(
    scope: RepoScope,
    extra: Prisma.AccountWhereInput = {},
  ): Prisma.AccountWhereInput {
    // Account inherits visibility from its parent Item. We additionally pin
    // the denormalized `domain` column so the planner uses the (domain,
    // userId) index without descending into the join when the scope is
    // personal or pcc.
    const itemFilter = itemScopeFilter(scope);
    const flat: Prisma.AccountWhereInput = {};
    if (scope.kind === 'personal') {
      flat.domain = 'personal';
      flat.userId = scope.userId;
    } else if (scope.kind === 'pcc') {
      flat.domain = 'pcc';
    }
    return { ...flat, item: { is: itemFilter }, ...extra };
  }

  return {
    async listByItem(scope, itemId): Promise<Result<ReadonlyArray<Account>, RepoError>> {
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      return tryDb(async () => {
        const rows = await prisma.account.findMany({
          where: whereForScope(scope, { itemId }),
          orderBy: { createdAt: 'asc' },
        });
        return rows.map(toDomain);
      });
    },

    async listAllInScope(scope, filter): Promise<Result<ReadonlyArray<Account>, RepoError>> {
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      return tryDb(async () => {
        const where = whereForScope(
          scope,
          filter?.domain !== undefined ? { domain: filter.domain } : {},
        );
        const rows = await prisma.account.findMany({
          where,
          orderBy: { createdAt: 'asc' },
        });
        return rows.map(toDomain);
      });
    },

    async upsertFromPlaid(scope, args): Promise<Result<{ readonly upserted: number }, RepoError>> {
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      try {
        // First gate: the parent Item must be visible to the caller's scope.
        // If not, return `not_found` (indistinguishable from "no such item").
        const item = await prisma.item.findFirst({
          where: { id: args.itemId, ...itemScopeFilter(scope) },
          select: { id: true, domain: true, userId: true },
        });
        if (item === null) {
          return Err({ kind: 'not_found' });
        }

        const itemDomain = item.domain as Domain;
        const itemUserId = item.userId;

        // Upsert each account. We do this in a single transaction so a
        // partial failure leaves no account rows behind.
        const upsertedCount = await prisma.$transaction(async (tx) => {
          let count = 0;
          for (const acc of args.accounts) {
            await tx.account.upsert({
              where: { plaidAccountId: acc.plaidAccountId },
              create: {
                itemId: args.itemId,
                domain: itemDomain,
                userId: itemUserId,
                plaidAccountId: acc.plaidAccountId,
                name: acc.name,
                officialName: acc.officialName,
                mask: acc.mask,
                type: acc.type,
                subtype: acc.subtype,
                isoCurrencyCode: acc.isoCurrencyCode,
                currentBalanceCents: centsToBigint(acc.currentBalanceCents),
                availableBalanceCents: centsToBigint(acc.availableBalanceCents),
                limitCents: centsToBigint(acc.limitCents),
                balanceUpdatedAt: new Date(),
              },
              update: {
                name: acc.name,
                officialName: acc.officialName,
                mask: acc.mask,
                type: acc.type,
                subtype: acc.subtype,
                isoCurrencyCode: acc.isoCurrencyCode,
                currentBalanceCents: centsToBigint(acc.currentBalanceCents),
                availableBalanceCents: centsToBigint(acc.availableBalanceCents),
                limitCents: centsToBigint(acc.limitCents),
                balanceUpdatedAt: new Date(),
              },
            });
            count += 1;
          }
          return count;
        });
        return Ok({ upserted: upsertedCount });
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },
  };
}

function centsToBigint(c: Cents | null): bigint | null {
  return c === null ? null : c;
}
