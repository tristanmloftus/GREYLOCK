// Greylock — root layout
// =============================================================================
// AGENT-UI (Phase 4). Self-hosts IBM Plex Mono + Syne via `next/font/local` so
// the Phase-5 CSP `font-src 'self'` holds. Imports global CSS once. The layout
// is a Server Component.
// =============================================================================

import type { ReactNode } from 'react';
import localFont from 'next/font/local';

import './globals.css';

const monoFont = localFont({
  src: [
    { path: './fonts/IBMPlexMono-Regular.woff2', weight: '400', style: 'normal' },
    { path: './fonts/IBMPlexMono-Medium.woff2', weight: '500', style: 'normal' },
    { path: './fonts/IBMPlexMono-Bold.woff2', weight: '700', style: 'normal' },
  ],
  display: 'swap',
  variable: '--font-mono',
});

const displayFont = localFont({
  src: [
    { path: './fonts/Syne-Regular.woff2', weight: '400', style: 'normal' },
    { path: './fonts/Syne-Bold.woff2', weight: '700', style: 'normal' },
  ],
  display: 'swap',
  variable: '--font-display',
});

export const metadata = {
  title: 'Greylock — Operating Dashboard',
  description: 'Private financial operating dashboard. Localhost-only.',
};

export default function RootLayout({ children }: { readonly children: ReactNode }): ReactNode {
  return (
    <html lang="en" className={`${monoFont.variable} ${displayFont.variable}`}>
      <body>{children}</body>
    </html>
  );
}
