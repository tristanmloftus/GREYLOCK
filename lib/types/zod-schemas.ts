// Greylock — Zod schemas for every API boundary
// =============================================================================
// AGENT-ARCH (Phase 1). Every request and response on every route in
// docs/API.md has a schema here. Route handlers MUST `safeParse` the request
// body (and `parse` the response shape) — no exceptions.
//
// Convention: schemas are declared first, types are inferred from them.
// =============================================================================

import { z } from 'zod';

// -----------------------------------------------------------------------------
// Primitives
// -----------------------------------------------------------------------------

/** Base64URL string, no padding, used for binary fields on the wire. */
export const Base64UrlStringSchema = z
  .string()
  .min(1)
  .regex(/^[A-Za-z0-9_-]+$/u, 'must be base64url');

/** Email — strict shape, normalized to lowercase. */
export const EmailSchema = z
  .string()
  .min(3)
  .max(254)
  .email()
  .transform((s) => s.toLowerCase());

export const UserIdSchema = z.string().min(1).max(64);
export const SessionIdSchema = z.string().min(1).max(64);
export const ItemIdSchema = z.string().min(1).max(64);
export const AccountIdSchema = z.string().min(1).max(64);
export const SnapshotIdSchema = z.string().min(1).max(64);
export const PasskeyIdSchema = z.string().min(1).max(64);

export const DomainSchema = z.enum(['personal', 'pcc']);
export const RoleSchema = z.enum(['owner', 'member']);
export const SessionStatusSchema = z.enum(['active', 'revoked', 'expired']);

export const IsoCurrencyCodeSchema = z.literal('USD');

/** Cents as a string (BigInt over JSON). Validated to be an integer string
 *  in the supported magnitude range (±2^63 - 1). */
export const CentsSchema = z
  .string()
  .regex(/^-?\d{1,19}$/u, 'integer cents string required')
  .transform((s) => BigInt(s));

/** Helper for serializing BigInt → string in responses. */
export const CentsOutSchema = z.bigint().transform((b) => b.toString());

// -----------------------------------------------------------------------------
// Auth — registration
// -----------------------------------------------------------------------------

export const RegistrationBeginRequestSchema = z.object({
  email: EmailSchema,
  displayName: z.string().min(1).max(120),
  // Owner is set server-side based on OWNER_EMAIL; clients cannot escalate.
  // Field accepted for shape-completeness; ignored on the server.
  role: RoleSchema.optional(),
});
export type RegistrationBeginRequest = z.infer<typeof RegistrationBeginRequestSchema>;

export const RegistrationBeginResponseSchema = z.object({
  challenge: Base64UrlStringSchema,
  rp: z.object({ id: z.string(), name: z.string() }),
  user: z.object({
    id: Base64UrlStringSchema,
    name: z.string(),
    displayName: z.string(),
  }),
  pubKeyCredParams: z.array(z.object({ type: z.literal('public-key'), alg: z.number() })),
  timeout: z.number().int().positive(),
  attestation: z.literal('none'),
  authenticatorSelection: z.object({
    residentKey: z.literal('required'),
    userVerification: z.literal('required'),
  }),
});
export type RegistrationBeginResponse = z.infer<typeof RegistrationBeginResponseSchema>;

export const PublicKeyCredentialResponseRegistrationSchema = z.object({
  id: Base64UrlStringSchema,
  rawId: Base64UrlStringSchema,
  response: z.object({
    attestationObject: Base64UrlStringSchema,
    clientDataJSON: Base64UrlStringSchema,
    transports: z.array(z.string()).optional(),
  }),
  clientExtensionResults: z.record(z.string(), z.unknown()),
  type: z.literal('public-key'),
  authenticatorAttachment: z.string().optional(),
});

export const RegistrationCompleteRequestSchema = z.object({
  email: EmailSchema,
  response: PublicKeyCredentialResponseRegistrationSchema,
  deviceLabel: z.string().min(1).max(80).nullable(),
});
export type RegistrationCompleteRequest = z.infer<typeof RegistrationCompleteRequestSchema>;

export const RegistrationCompleteResponseSchema = z.object({
  userId: UserIdSchema,
  passkeyId: PasskeyIdSchema,
});
export type RegistrationCompleteResponse = z.infer<typeof RegistrationCompleteResponseSchema>;

// -----------------------------------------------------------------------------
// Auth — authentication
// -----------------------------------------------------------------------------

export const AuthenticationBeginRequestSchema = z.object({
  email: EmailSchema,
});
export type AuthenticationBeginRequest = z.infer<typeof AuthenticationBeginRequestSchema>;

export const AuthenticationBeginResponseSchema = z.object({
  challenge: Base64UrlStringSchema,
  rpId: z.string(),
  timeout: z.number().int().positive(),
  userVerification: z.literal('required'),
  allowCredentials: z.array(z.object({ id: Base64UrlStringSchema, type: z.literal('public-key') })),
});
export type AuthenticationBeginResponse = z.infer<typeof AuthenticationBeginResponseSchema>;

export const PublicKeyCredentialResponseAuthenticationSchema = z.object({
  id: Base64UrlStringSchema,
  rawId: Base64UrlStringSchema,
  response: z.object({
    authenticatorData: Base64UrlStringSchema,
    clientDataJSON: Base64UrlStringSchema,
    signature: Base64UrlStringSchema,
    userHandle: Base64UrlStringSchema.optional(),
  }),
  clientExtensionResults: z.record(z.string(), z.unknown()),
  type: z.literal('public-key'),
  authenticatorAttachment: z.string().optional(),
});

export const AuthenticationCompleteRequestSchema = z.object({
  email: EmailSchema,
  response: PublicKeyCredentialResponseAuthenticationSchema,
});
export type AuthenticationCompleteRequest = z.infer<typeof AuthenticationCompleteRequestSchema>;

export const AuthenticationCompleteResponseSchema = z.object({
  userId: UserIdSchema,
  sessionId: SessionIdSchema,
  // The cookie itself is set via Set-Cookie header; this just signals success.
});
export type AuthenticationCompleteResponse = z.infer<typeof AuthenticationCompleteResponseSchema>;

export const LogoutResponseSchema = z.object({ ok: z.literal(true) });
export type LogoutResponse = z.infer<typeof LogoutResponseSchema>;

// -----------------------------------------------------------------------------
// Plaid
// -----------------------------------------------------------------------------

export const PlaidLinkTokenRequestSchema = z.object({
  domain: DomainSchema,
  // Subset of products allowed in v0.1.
  products: z.array(z.enum(['transactions', 'auth', 'identity'])).min(1),
});
export type PlaidLinkTokenRequest = z.infer<typeof PlaidLinkTokenRequestSchema>;

export const PlaidLinkTokenResponseSchema = z.object({
  linkToken: z.string().min(1),
  expiresAt: z.string().datetime(),
});
export type PlaidLinkTokenResponse = z.infer<typeof PlaidLinkTokenResponseSchema>;

export const PlaidExchangeRequestSchema = z.object({
  domain: DomainSchema,
  publicToken: z.string().min(1),
  institutionId: z.string().min(1).nullable(),
  institutionName: z.string().min(1).nullable(),
});
export type PlaidExchangeRequest = z.infer<typeof PlaidExchangeRequestSchema>;

export const PlaidExchangeResponseSchema = z.object({
  itemId: ItemIdSchema,
});
export type PlaidExchangeResponse = z.infer<typeof PlaidExchangeResponseSchema>;

export const PlaidItemListResponseSchema = z.object({
  items: z.array(
    z.object({
      itemId: ItemIdSchema,
      domain: DomainSchema,
      institutionName: z.string().nullable(),
      lastSyncAt: z.string().datetime().nullable(),
      lastSyncOutcome: z.enum(['success', 'error', 'pending']).nullable(),
      consecutiveFailures: z.number().int().nonnegative(),
      removedAt: z.string().datetime().nullable(),
    }),
  ),
});
export type PlaidItemListResponse = z.infer<typeof PlaidItemListResponseSchema>;

export const PlaidItemRemoveRequestSchema = z.object({
  itemId: ItemIdSchema,
  reason: z.string().min(1).max(200),
});
export type PlaidItemRemoveRequest = z.infer<typeof PlaidItemRemoveRequestSchema>;

export const PlaidItemRemoveResponseSchema = z.object({ ok: z.literal(true) });
export type PlaidItemRemoveResponse = z.infer<typeof PlaidItemRemoveResponseSchema>;

// -----------------------------------------------------------------------------
// Sync
// -----------------------------------------------------------------------------

export const SyncTriggerRequestSchema = z.object({
  itemId: ItemIdSchema,
});
export type SyncTriggerRequest = z.infer<typeof SyncTriggerRequestSchema>;

export const SyncTriggerResponseSchema = z.object({
  added: z.number().int().nonnegative(),
  modified: z.number().int().nonnegative(),
  removed: z.number().int().nonnegative(),
  hasMore: z.boolean(),
});
export type SyncTriggerResponse = z.infer<typeof SyncTriggerResponseSchema>;

// -----------------------------------------------------------------------------
// Dashboard / compute
// -----------------------------------------------------------------------------

export const DashboardSnapshotResponseSchema = z.object({
  domain: DomainSchema,
  takenAt: z.string().datetime(),
  assetsCents: CentsOutSchema,
  liabilitiesCents: CentsOutSchema,
  netWorthCents: CentsOutSchema,
  cashCents: CentsOutSchema,
  monthNetCents: CentsOutSchema.nullable(),
  billionProgress: z.number().min(0).max(1),
  breakdown: z.array(
    z.object({
      accountId: AccountIdSchema,
      name: z.string(),
      type: z.enum(['depository', 'credit', 'loan', 'investment', 'other']),
      balanceCents: CentsOutSchema,
      contribution: z.enum(['asset', 'liability']),
    }),
  ),
});
export type DashboardSnapshotResponse = z.infer<typeof DashboardSnapshotResponseSchema>;

export const DashboardSeriesQuerySchema = z.object({
  domain: DomainSchema,
  fromTs: z.string().datetime(),
  toTs: z.string().datetime(),
});
export type DashboardSeriesQuery = z.infer<typeof DashboardSeriesQuerySchema>;

export const DashboardSeriesResponseSchema = z.object({
  domain: DomainSchema,
  points: z.array(
    z.object({
      takenAt: z.string().datetime(),
      netWorthCents: CentsOutSchema,
      cashCents: CentsOutSchema,
    }),
  ),
});
export type DashboardSeriesResponse = z.infer<typeof DashboardSeriesResponseSchema>;

// -----------------------------------------------------------------------------
// Admin (CLI-invoked HTTP routes — authenticated as owner)
// -----------------------------------------------------------------------------

export const AdminEnrollRequestSchema = z.object({
  email: EmailSchema,
  displayName: z.string().min(1).max(120),
  role: RoleSchema,
});
export type AdminEnrollRequest = z.infer<typeof AdminEnrollRequestSchema>;

export const AdminEnrollResponseSchema = z.object({
  enrollmentUrl: z.string().url(),
  expiresAt: z.string().datetime(),
});
export type AdminEnrollResponse = z.infer<typeof AdminEnrollResponseSchema>;

export const AdminRevokeRequestSchema = z.object({
  email: EmailSchema,
});
export type AdminRevokeRequest = z.infer<typeof AdminRevokeRequestSchema>;

export const AdminRevokeResponseSchema = z.object({
  sessionsRevoked: z.number().int().nonnegative(),
});
export type AdminRevokeResponse = z.infer<typeof AdminRevokeResponseSchema>;

export const AdminAuditVerifyResponseSchema = z.object({
  verifiedCount: z.number().int().nonnegative(),
  brokenAtSeq: z.string().nullable(), // BigInt-as-string
});
export type AdminAuditVerifyResponse = z.infer<typeof AdminAuditVerifyResponseSchema>;

// -----------------------------------------------------------------------------
// Error envelope
// -----------------------------------------------------------------------------

export const ErrorResponseSchema = z.object({
  error: z.object({
    code: z.string(),
    message: z.string(),
    // Optional field for rate-limit responses.
    retryAfterSeconds: z.number().int().positive().optional(),
  }),
});
export type ErrorResponse = z.infer<typeof ErrorResponseSchema>;
