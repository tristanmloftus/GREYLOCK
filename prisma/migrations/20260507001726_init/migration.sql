-- CreateTable
CREATE TABLE "User" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "email" TEXT NOT NULL,
    "displayName" TEXT NOT NULL,
    "role" TEXT NOT NULL DEFAULT 'member',
    "createdAt" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" DATETIME NOT NULL,
    "wrappedUserDek" BLOB,
    "userDekVersion" INTEGER NOT NULL DEFAULT 1
);

-- CreateTable
CREATE TABLE "Passkey" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "userId" TEXT NOT NULL,
    "credentialId" BLOB NOT NULL,
    "credentialPublicKey" BLOB NOT NULL,
    "counter" BIGINT NOT NULL DEFAULT 0,
    "transports" TEXT,
    "aaguid" BLOB,
    "deviceLabel" TEXT,
    "createdAt" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "lastUsedAt" DATETIME,
    "revokedAt" DATETIME,
    "kekSalt" BLOB NOT NULL,
    CONSTRAINT "Passkey_userId_fkey" FOREIGN KEY ("userId") REFERENCES "User" ("id") ON DELETE CASCADE ON UPDATE CASCADE
);

-- CreateTable
CREATE TABLE "Session" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "userId" TEXT NOT NULL,
    "status" TEXT NOT NULL DEFAULT 'active',
    "createdAt" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "lastActivityAt" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "expiresAt" DATETIME NOT NULL,
    "idleTimeoutAt" DATETIME NOT NULL,
    "revokedAt" DATETIME,
    "revokedReason" TEXT,
    "userAgent" TEXT,
    "remoteAddr" TEXT,
    CONSTRAINT "Session_userId_fkey" FOREIGN KEY ("userId") REFERENCES "User" ("id") ON DELETE CASCADE ON UPDATE CASCADE
);

-- CreateTable
CREATE TABLE "PccMembership" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "userId" TEXT NOT NULL,
    "joinedAt" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "revokedAt" DATETIME,
    CONSTRAINT "PccMembership_userId_fkey" FOREIGN KEY ("userId") REFERENCES "User" ("id") ON DELETE CASCADE ON UPDATE CASCADE
);

-- CreateTable
CREATE TABLE "Item" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "domain" TEXT NOT NULL,
    "userId" TEXT,
    "plaidItemId" TEXT NOT NULL,
    "plaidInstitutionId" TEXT,
    "institutionName" TEXT,
    "encryptedAccessToken" BLOB NOT NULL,
    "syncCursor" TEXT,
    "lastSyncAt" DATETIME,
    "lastSyncOutcome" TEXT,
    "consecutiveFailures" INTEGER NOT NULL DEFAULT 0,
    "createdAt" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" DATETIME NOT NULL,
    "removedAt" DATETIME,
    "removedReason" TEXT,
    CONSTRAINT "Item_userId_fkey" FOREIGN KEY ("userId") REFERENCES "User" ("id") ON DELETE SET NULL ON UPDATE CASCADE
);

-- CreateTable
CREATE TABLE "Account" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "itemId" TEXT NOT NULL,
    "domain" TEXT NOT NULL,
    "userId" TEXT,
    "plaidAccountId" TEXT NOT NULL,
    "name" TEXT NOT NULL,
    "officialName" TEXT,
    "mask" TEXT,
    "type" TEXT NOT NULL,
    "subtype" TEXT,
    "isoCurrencyCode" TEXT NOT NULL DEFAULT 'USD',
    "currentBalanceCents" BIGINT,
    "availableBalanceCents" BIGINT,
    "limitCents" BIGINT,
    "balanceUpdatedAt" DATETIME,
    "createdAt" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" DATETIME NOT NULL,
    "closedAt" DATETIME,
    CONSTRAINT "Account_itemId_fkey" FOREIGN KEY ("itemId") REFERENCES "Item" ("id") ON DELETE CASCADE ON UPDATE CASCADE
);

-- CreateTable
CREATE TABLE "Transaction" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "itemId" TEXT NOT NULL,
    "accountId" TEXT NOT NULL,
    "domain" TEXT NOT NULL,
    "userId" TEXT,
    "plaidTransactionId" TEXT NOT NULL,
    "amountCents" BIGINT NOT NULL,
    "isoCurrencyCode" TEXT NOT NULL DEFAULT 'USD',
    "date" DATETIME NOT NULL,
    "authorizedDate" DATETIME,
    "name" TEXT NOT NULL,
    "merchantName" TEXT,
    "pending" BOOLEAN NOT NULL DEFAULT false,
    "category" TEXT,
    "categoryDetailed" TEXT,
    "createdAt" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" DATETIME NOT NULL,
    "removedAt" DATETIME,
    CONSTRAINT "Transaction_itemId_fkey" FOREIGN KEY ("itemId") REFERENCES "Item" ("id") ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT "Transaction_accountId_fkey" FOREIGN KEY ("accountId") REFERENCES "Account" ("id") ON DELETE CASCADE ON UPDATE CASCADE
);

-- CreateTable
CREATE TABLE "NetWorthSnapshot" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "domain" TEXT NOT NULL,
    "userId" TEXT,
    "takenAt" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "assetsCents" BIGINT NOT NULL,
    "liabilitiesCents" BIGINT NOT NULL,
    "netWorthCents" BIGINT NOT NULL,
    "cashCents" BIGINT NOT NULL,
    "monthNetCents" BIGINT,
    "computeVersion" INTEGER NOT NULL DEFAULT 1,
    "breakdownJson" TEXT NOT NULL,
    CONSTRAINT "NetWorthSnapshot_userId_fkey" FOREIGN KEY ("userId") REFERENCES "User" ("id") ON DELETE SET NULL ON UPDATE CASCADE
);

-- CreateTable
CREATE TABLE "AuditLogEntry" (
    "seq" BIGINT NOT NULL PRIMARY KEY,
    "ts" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "tsNanos" INTEGER NOT NULL DEFAULT 0,
    "actorUserId" TEXT,
    "actorKind" TEXT NOT NULL,
    "domain" TEXT,
    "subjectId" TEXT,
    "subjectKind" TEXT,
    "action" TEXT NOT NULL,
    "outcome" TEXT NOT NULL,
    "detailsJson" TEXT NOT NULL,
    "prevHash" BLOB NOT NULL,
    "entryHash" BLOB NOT NULL,
    CONSTRAINT "AuditLogEntry_actorUserId_fkey" FOREIGN KEY ("actorUserId") REFERENCES "User" ("id") ON DELETE SET NULL ON UPDATE CASCADE
);

-- CreateTable
CREATE TABLE "PccKeyWrap" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "version" INTEGER NOT NULL,
    "wrappedDek" BLOB NOT NULL,
    "kdfAlgorithm" TEXT NOT NULL,
    "kdfN" INTEGER NOT NULL,
    "kdfR" INTEGER NOT NULL,
    "kdfP" INTEGER NOT NULL,
    "kdfSalt" BLOB NOT NULL,
    "createdAt" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "retiredAt" DATETIME
);

-- CreateTable
CREATE TABLE "RateLimitBucket" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "bucketKey" TEXT NOT NULL,
    "windowStart" DATETIME NOT NULL,
    "count" INTEGER NOT NULL DEFAULT 0,
    "updatedAt" DATETIME NOT NULL
);

-- CreateTable
CREATE TABLE "EnrollmentToken" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "email" TEXT NOT NULL,
    "tokenHash" BLOB NOT NULL,
    "expiresAt" DATETIME NOT NULL,
    "usedAt" DATETIME,
    "createdAt" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- CreateIndex
CREATE UNIQUE INDEX "User_email_key" ON "User"("email");

-- CreateIndex
CREATE INDEX "User_email_idx" ON "User"("email");

-- CreateIndex
CREATE INDEX "User_role_idx" ON "User"("role");

-- CreateIndex
CREATE UNIQUE INDEX "Passkey_credentialId_key" ON "Passkey"("credentialId");

-- CreateIndex
CREATE INDEX "Passkey_userId_idx" ON "Passkey"("userId");

-- CreateIndex
CREATE INDEX "Passkey_userId_revokedAt_idx" ON "Passkey"("userId", "revokedAt");

-- CreateIndex
CREATE INDEX "Session_userId_status_idx" ON "Session"("userId", "status");

-- CreateIndex
CREATE INDEX "Session_expiresAt_idx" ON "Session"("expiresAt");

-- CreateIndex
CREATE INDEX "Session_idleTimeoutAt_idx" ON "Session"("idleTimeoutAt");

-- CreateIndex
CREATE UNIQUE INDEX "PccMembership_userId_key" ON "PccMembership"("userId");

-- CreateIndex
CREATE INDEX "PccMembership_revokedAt_idx" ON "PccMembership"("revokedAt");

-- CreateIndex
CREATE UNIQUE INDEX "Item_plaidItemId_key" ON "Item"("plaidItemId");

-- CreateIndex
CREATE INDEX "Item_domain_idx" ON "Item"("domain");

-- CreateIndex
CREATE INDEX "Item_userId_idx" ON "Item"("userId");

-- CreateIndex
CREATE INDEX "Item_domain_userId_idx" ON "Item"("domain", "userId");

-- CreateIndex
CREATE INDEX "Item_removedAt_idx" ON "Item"("removedAt");

-- CreateIndex
CREATE UNIQUE INDEX "Account_plaidAccountId_key" ON "Account"("plaidAccountId");

-- CreateIndex
CREATE INDEX "Account_itemId_idx" ON "Account"("itemId");

-- CreateIndex
CREATE INDEX "Account_domain_userId_idx" ON "Account"("domain", "userId");

-- CreateIndex
CREATE INDEX "Account_domain_idx" ON "Account"("domain");

-- CreateIndex
CREATE UNIQUE INDEX "Transaction_plaidTransactionId_key" ON "Transaction"("plaidTransactionId");

-- CreateIndex
CREATE INDEX "Transaction_itemId_idx" ON "Transaction"("itemId");

-- CreateIndex
CREATE INDEX "Transaction_accountId_idx" ON "Transaction"("accountId");

-- CreateIndex
CREATE INDEX "Transaction_domain_userId_date_idx" ON "Transaction"("domain", "userId", "date");

-- CreateIndex
CREATE INDEX "Transaction_date_idx" ON "Transaction"("date");

-- CreateIndex
CREATE INDEX "Transaction_removedAt_idx" ON "Transaction"("removedAt");

-- CreateIndex
CREATE INDEX "NetWorthSnapshot_domain_userId_takenAt_idx" ON "NetWorthSnapshot"("domain", "userId", "takenAt");

-- CreateIndex
CREATE INDEX "NetWorthSnapshot_domain_takenAt_idx" ON "NetWorthSnapshot"("domain", "takenAt");

-- CreateIndex
CREATE INDEX "NetWorthSnapshot_takenAt_idx" ON "NetWorthSnapshot"("takenAt");

-- CreateIndex
CREATE UNIQUE INDEX "AuditLogEntry_entryHash_key" ON "AuditLogEntry"("entryHash");

-- CreateIndex
CREATE INDEX "AuditLogEntry_ts_idx" ON "AuditLogEntry"("ts");

-- CreateIndex
CREATE INDEX "AuditLogEntry_actorUserId_idx" ON "AuditLogEntry"("actorUserId");

-- CreateIndex
CREATE INDEX "AuditLogEntry_action_idx" ON "AuditLogEntry"("action");

-- CreateIndex
CREATE INDEX "AuditLogEntry_domain_ts_idx" ON "AuditLogEntry"("domain", "ts");

-- CreateIndex
CREATE INDEX "AuditLogEntry_subjectKind_subjectId_idx" ON "AuditLogEntry"("subjectKind", "subjectId");

-- CreateIndex
CREATE UNIQUE INDEX "PccKeyWrap_version_key" ON "PccKeyWrap"("version");

-- CreateIndex
CREATE INDEX "PccKeyWrap_retiredAt_idx" ON "PccKeyWrap"("retiredAt");

-- CreateIndex
CREATE UNIQUE INDEX "RateLimitBucket_bucketKey_key" ON "RateLimitBucket"("bucketKey");

-- CreateIndex
CREATE UNIQUE INDEX "EnrollmentToken_tokenHash_key" ON "EnrollmentToken"("tokenHash");

-- CreateIndex
CREATE INDEX "EnrollmentToken_email_idx" ON "EnrollmentToken"("email");

-- CreateIndex
CREATE INDEX "EnrollmentToken_expiresAt_idx" ON "EnrollmentToken"("expiresAt");
