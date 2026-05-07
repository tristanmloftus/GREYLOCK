// Greylock — dashboard (server component)
// =============================================================================
// AGENT-UI (Phase 4). The main dashboard. Reads the session cookie, validates
// it server-side, fetches NetWorthSnapshot + accounts + transactions for both
// personal and (if member) PCC, formats every number into display strings,
// and renders the layout. The client poller refreshes the page every 30s when
// the tab is visible.
// =============================================================================

import type { ReactNode } from 'react';
import { redirect } from 'next/navigation';

import { Header } from '../components/chrome/Header';
import { Footer } from '../components/chrome/Footer';
import { DashboardLayout } from '../components/dashboard/DashboardLayout';
import { DashboardPoller } from '../components/dashboard/DashboardPoller';

import { resolveCurrentUser } from './_lib/current-user';
import { fetchDashboardData } from './_lib/dashboard-data';

export const dynamic = 'force-dynamic';

export default async function DashboardPage(): Promise<ReactNode> {
  const ctx = await resolveCurrentUser();
  if (ctx === null) {
    redirect('/login');
  }
  const data = await fetchDashboardData({
    userId: ctx.user.id,
    isPccMember: ctx.isPccMember,
  });

  return (
    <>
      <Header
        brand="LOFTUS"
        subtitle="OPERATING DASHBOARD"
        lastSyncAt={data.lastSyncAt}
      />
      <main className="app-shell">
        <DashboardLayout
          summary={data.summary}
          personal={data.personal}
          pcc={data.pcc}
        />
      </main>
      <DashboardPoller domain={ctx.isPccMember ? 'pcc' : 'personal'} />
      <Footer />
    </>
  );
}
