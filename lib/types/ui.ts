// Greylock — UI-only types (read-only export to AGENT-UI consumers)
// =============================================================================
// AGENT-UI (Phase 4). UI page components need a tiny shared shape for the
// "currently authenticated user" they pre-resolve from the cookie. This is
// strictly the orchestration shape passed from server components to client
// components; the canonical domain shapes still live in `lib/types/domain.ts`.
// =============================================================================

import type { Session, User } from './domain.js';

export interface CurrentUserContext {
  readonly user: User;
  readonly session: Session;
  readonly isPccMember: boolean;
}
