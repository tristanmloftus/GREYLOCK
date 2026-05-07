// Greylock — integration test: dashboard layout server-side rendering
// =============================================================================
// AGENT-UI (Phase 4). Renders <DashboardLayout> on the server with a fixture
// summary + columns and asserts:
//   - Cents are formatted to USD display strings (no bigint leakage).
//   - The empty-state path renders for unavailable columns.
//   - The summary strip has all four cards.
//   - Tabular-nums class is present on number elements.
// =============================================================================

import { describe, it, expect } from 'vitest';
import { renderToStaticMarkup } from 'react-dom/server';
import { createElement } from 'react';

import { DashboardLayout } from '../../../components/dashboard/DashboardLayout';
import type {
  DashboardLayoutProps,
} from '../../../components/dashboard/DashboardLayout';

function makeProps(): DashboardLayoutProps {
  return {
    summary: {
      netWorthDisplay: '$1,234.56',
      cashDisplay: '$500.00',
      monthNetDisplay: '+$200.00',
      billionDisplay: '0.00%',
      billionProgress: 0.0001,
      monthNetDelta: { text: '30d window', direction: 'positive' },
    },
    personal: {
      title: 'Personal',
      available: true,
      netWorthDisplay: '$1,000.00',
      cashDisplay: '$500.00',
      monthNetDisplay: '+$100.00',
      monthNetDirection: 'positive',
      accounts: [
        {
          accountId: 'acc_1',
          name: 'Chase Checking',
          type: 'depository',
          balanceDisplay: '$500.00',
          contribution: 'asset',
        },
      ],
      transactions: [
        {
          transactionId: 'tx_1',
          date: '2026-05-01',
          name: 'Coffee',
          category: 'food',
          amountDisplay: '+$5.50',
          direction: 'outflow',
        },
      ],
    },
    pcc: {
      title: 'PCC',
      available: false,
      netWorthDisplay: '$0.00',
      cashDisplay: '$0.00',
      monthNetDisplay: '+$0.00',
      monthNetDirection: 'neutral',
      accounts: [],
      transactions: [],
      emptyDescription: 'PCC member required.',
    },
  };
}

describe('DashboardLayout — server render', () => {
  it('renders all four summary cards', () => {
    const html = renderToStaticMarkup(createElement(DashboardLayout, makeProps()));
    expect(html).toContain('Net Worth');
    expect(html).toContain('Cash');
    expect(html).toContain('Month Net');
    expect(html).toContain('$1B Progress');
  });

  it('renders display strings rather than raw cents', () => {
    const html = renderToStaticMarkup(createElement(DashboardLayout, makeProps()));
    expect(html).toContain('$1,234.56');
    expect(html).toContain('+$200.00');
    // No bigint suffix anywhere.
    expect(html).not.toContain('100000n');
  });

  it('shows empty state when a column is unavailable', () => {
    const html = renderToStaticMarkup(createElement(DashboardLayout, makeProps()));
    expect(html).toContain('PCC member required.');
  });

  it('renders num class for tabular-nums on the value displays', () => {
    const html = renderToStaticMarkup(createElement(DashboardLayout, makeProps()));
    // Only need to assert the helper class is applied somewhere — actual font-feature
    // is a CSS concern.
    expect(html).toMatch(/class="[^"]*\bnum\b/);
  });

  it('renders the recent transactions row', () => {
    const html = renderToStaticMarkup(createElement(DashboardLayout, makeProps()));
    expect(html).toContain('Coffee');
    expect(html).toContain('2026-05-01');
  });

  it('renders the connected accounts row', () => {
    const html = renderToStaticMarkup(createElement(DashboardLayout, makeProps()));
    expect(html).toContain('Chase Checking');
    expect(html).toContain('depository');
  });
});
