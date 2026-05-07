// Greylock — email allowlist + placeholder rejection
// =============================================================================
// AGENT-AUTH (Phase 2). Read-only configuration helpers. Source of truth is
// `process.env.ALLOWED_EMAILS` (comma-separated, lowercase, no spaces). The
// placeholder address `cade-placeholder@greylock.invalid` is hard-rejected at
// every flow even when present in the allowlist (SPEC §4 decision 2).
//
// Email comparison is case-insensitive after `.trim()`. Implementation never
// throws — caller-side normalization should happen via `EmailSchema` (Zod) but
// these helpers tolerate raw input as a defense-in-depth check.
// =============================================================================

/** Hard-coded placeholder address. Must NEVER be allowed to enroll or auth. */
export const PLACEHOLDER_EMAIL = 'cade-placeholder@greylock.invalid';

/** Normalize an email for comparison: trim + lowercase. */
export function normalizeEmail(email: string): string {
  return email.trim().toLowerCase();
}

/** True iff `email` (post-normalization) equals the placeholder address. */
export function isPlaceholderEmail(email: string): boolean {
  return normalizeEmail(email) === PLACEHOLDER_EMAIL;
}

/**
 * Read the configured allowlist from `process.env.ALLOWED_EMAILS`.
 * Returns the lowercased, trimmed set of allowed addresses. Empty entries
 * (e.g. trailing comma) are dropped. The placeholder is intentionally NOT
 * filtered here — `isAllowedEmail` does the placeholder rejection separately
 * so the audit log can distinguish "not in env" from "placeholder hit".
 */
export function readAllowedEmails(): ReadonlySet<string> {
  const raw = process.env['ALLOWED_EMAILS'] ?? '';
  const out = new Set<string>();
  for (const part of raw.split(',')) {
    const norm = normalizeEmail(part);
    if (norm.length > 0) {
      out.add(norm);
    }
  }
  return out;
}

/**
 * True iff `email` is in `ALLOWED_EMAILS` AND is not the placeholder.
 * Returns false on any of: empty env, missing entry, placeholder match.
 */
export function isAllowedEmail(email: string): boolean {
  const norm = normalizeEmail(email);
  if (norm === PLACEHOLDER_EMAIL) {
    return false;
  }
  return readAllowedEmails().has(norm);
}
