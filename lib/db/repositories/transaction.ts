// Greylock — TransactionRepository
// =============================================================================
// AGENT-DB. Visibility-gated by parent Item; same scope-by-construction rule
// as AccountRepository. Soft-deletes via `removedAt`.
// =============================================================================

import type { PrismaClient, Transaction as PrismaTransaction, Prisma } from '@prisma/client';

import { AccountId, Err, ItemId, Ok, TransactionId, UserId } from '../../types/domain.js';
import type {
  Cents,
  Domain,
  IsoCurrencyCode,
  RepoError,
  Result,
  Transaction,
} from '../../types/domain.js';
import type { RepoScope, TransactionInput, TransactionRepository } from '../../types/services.js';

import {
  asPlaidTransactionId,
  itemScopeFilter,
  mapPrismaError,
  requirePccMembershipOrNotFound,
  tryDb,
} from './_shared.js';

function toDomain(row: PrismaTransaction): Transaction {
  return {
    id: TransactionId(row.id),
    itemId: ItemId(row.itemId),
    accountId: AccountId(row.accountId),
    domain: row.domain as Domain,
    userId: row.userId === null ? null : UserId(row.userId),
    plaidTransactionId: asPlaidTransactionId(row.plaidTransactionId),
    amountCents: row.amountCents,
    isoCurrencyCode: row.isoCurrencyCode as IsoCurrencyCode,
    date: row.date,
    authorizedDate: row.authorizedDate,
    name: row.name,
    merchantName: row.merchantName,
    pending: row.pending,
    category: row.category,
    categoryDetailed: row.categoryDetailed,
    createdAt: row.createdAt,
    updatedAt: row.updatedAt,
    removedAt: row.removedAt,
  };
}

export interface CreateTransactionRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createTransactionRepository(
  input: CreateTransactionRepositoryInput,
): TransactionRepository {
  const { prisma } = input;

  function whereForScope(
    scope: RepoScope,
    extra: Prisma.TransactionWhereInput = {},
  ): Prisma.TransactionWhereInput {
    const itemFilter = itemScopeFilter(scope);
    const flat: Prisma.TransactionWhereInput = {};
    if (scope.kind === 'personal') {
      flat.domain = 'personal';
      flat.userId = scope.userId;
    } else if (scope.kind === 'pcc') {
      flat.domain = 'pcc';
    }
    return { ...flat, item: { is: itemFilter }, ...extra };
  }

  return {
    async applyPlaidSync(
      scope,
      args,
    ): Promise<
      Result<
        { readonly added: number; readonly modified: number; readonly removed: number },
        RepoError
      >
    > {
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      try {
        // Parent Item must be visible to the caller's scope.
        const item = await prisma.item.findFirst({
          where: { id: args.itemId, ...itemScopeFilter(scope) },
          select: { id: true },
        });
        if (item === null) {
          return Err({ kind: 'not_found' });
        }
        const counts = await prisma.$transaction(async (tx) => {
          let added = 0;
          let modified = 0;
          let removed = 0;
          for (const t of args.added) {
            await tx.transaction.create({ data: txCreateData(t) });
            added += 1;
          }
          for (const t of args.modified) {
            // Update by plaidTransactionId; only modify rows under our item
            // (extra safety even though plaidTransactionId is globally unique).
            const r = await tx.transaction.updateMany({
              where: { plaidTransactionId: t.plaidTransactionId, itemId: args.itemId },
              data: txUpdateData(t),
            });
            modified += r.count;
          }
          if (args.removedPlaidIds.length > 0) {
            const r = await tx.transaction.updateMany({
              where: {
                plaidTransactionId: { in: args.removedPlaidIds.map((s) => s) },
                itemId: args.itemId,
                removedAt: null,
              },
              data: { removedAt: new Date() },
            });
            removed = r.count;
          }
          return { added, modified, removed };
        });
        return Ok(counts);
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async listByDateRange(scope, args): Promise<Result<ReadonlyArray<Transaction>, RepoError>> {
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      return tryDb(async () => {
        const where = whereForScope(scope, {
          date: { gte: args.fromDate, lt: args.toDate },
          removedAt: null,
          ...(args.domain !== undefined ? { domain: args.domain } : {}),
        });
        const rows = await prisma.transaction.findMany({
          where,
          orderBy: { date: 'desc' },
        });
        return rows.map(toDomain);
      });
    },
  };
}

function txCreateData(t: TransactionInput): Prisma.TransactionUncheckedCreateInput {
  return {
    itemId: t.itemId,
    accountId: t.accountId,
    domain: t.domain,
    userId: t.userId,
    plaidTransactionId: t.plaidTransactionId,
    amountCents: bigintFromCents(t.amountCents),
    isoCurrencyCode: t.isoCurrencyCode,
    date: t.date,
    authorizedDate: t.authorizedDate,
    name: t.name,
    merchantName: t.merchantName,
    pending: t.pending,
    category: t.category,
    categoryDetailed: t.categoryDetailed,
  };
}

function txUpdateData(t: TransactionInput): Prisma.TransactionUpdateManyMutationInput {
  return {
    amountCents: bigintFromCents(t.amountCents),
    isoCurrencyCode: t.isoCurrencyCode,
    date: t.date,
    authorizedDate: t.authorizedDate,
    name: t.name,
    merchantName: t.merchantName,
    pending: t.pending,
    category: t.category,
    categoryDetailed: t.categoryDetailed,
  };
}

function bigintFromCents(c: Cents): bigint {
  return c;
}
