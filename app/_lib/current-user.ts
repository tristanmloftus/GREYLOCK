// Greylock — server-side currentUser resolution helper (App Router).
// =============================================================================
// AGENT-UI (Phase 4). Reads the iron-session cookie, validates the session via
// AuthService, then loads the User row. Returns null on any failure (the
// caller decides whether to redirect / 404). Never throws across the boundary.
//
// This file lives under `app/_lib/` (App Router treats `_`-prefixed dirs as
// non-routable) so it doesn't conflict with `lib/*` (which AGENT-UI must not
// touch).
// =============================================================================

import { cookies } from 'next/headers';

import { SessionId } from '../../lib/types/domain';
import type { CurrentUserContext } from '../../lib/types/ui';
import {
  readSessionConfig,
  unsealSessionCookie,
} from '../../lib/auth/session';
import {
  getAuthService,
  getFullRepos,
} from '../../lib/runtime/services-registry';

export async function resolveCurrentUser(): Promise<CurrentUserContext | null> {
  const sessionConfig = readSessionConfig();
  const cookieStore = await cookies();
  const cookie = cookieStore.get(sessionConfig.cookieName);
  if (cookie === undefined) {
    return null;
  }
  const unsealed = await unsealSessionCookie({
    cookieValue: cookie.value,
    config: sessionConfig,
  });
  if (!unsealed.ok) {
    return null;
  }
  let auth;
  try {
    auth = await getAuthService();
  } catch {
    return null;
  }
  const sess = await auth.validateSession({
    sessionId: SessionId(unsealed.value.sessionId),
    cookieValue: cookie.value,
  });
  if (!sess.ok) {
    return null;
  }
  let repos;
  try {
    repos = await getFullRepos();
  } catch {
    return null;
  }
  const userRes = await repos.userRepo.findById(sess.value.userId);
  if (!userRes.ok || userRes.value === null) {
    return null;
  }
  const memberRes = await repos.pccMembershipRepo.isActiveMember(sess.value.userId);
  const isPccMember = memberRes.ok && memberRes.value;
  return {
    user: userRes.value,
    session: sess.value,
    isPccMember,
  };
}
