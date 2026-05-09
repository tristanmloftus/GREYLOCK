/** @type {import('next').NextConfig} */
//
// Content-Security-Policy (Phase 5 lock).
//
// Greylock is a localhost-only app. The single accepted third-party origin
// is `https://cdn.plaid.com` for the Plaid Link script + iframe. Every
// other resource is `'self'`.
//
// `'unsafe-inline'` for styles is regrettable but unavoidable for Next.js
// 15 + React 19 (RSC streams inline `<style>` tags). The same Next.js
// hardening guides accept this trade-off; we mitigate by forbidding inline
// scripts (the dangerous case) and by being a localhost-only app where
// XSS into our pages requires shell on the box (which already loses).
//
// `script-src` does NOT include `'unsafe-inline'`. Our codebase has zero
// inline scripts (verified by grep + ESLint).
const csp = [
  "default-src 'self'",
  "script-src 'self' https://cdn.plaid.com",
  "style-src 'self' 'unsafe-inline'", // Next.js RSC inline styles — not script
  "img-src 'self' data: https://cdn.plaid.com https://plaid-merchant-logos.plaid.com",
  "font-src 'self'", // self-hosted IBM Plex Mono + Syne
  "connect-src 'self' https://production.plaid.com https://development.plaid.com https://sandbox.plaid.com",
  "frame-src https://cdn.plaid.com https://*.plaid.com",
  "frame-ancestors 'none'",
  "form-action 'self'",
  "base-uri 'none'",
  "object-src 'none'",
  'upgrade-insecure-requests',
].join('; ');

const nextConfig = {
  reactStrictMode: true,
  poweredByHeader: false,

  async headers() {
    return [
      {
        source: '/:path*',
        headers: [
          { key: 'Content-Security-Policy', value: csp },
          { key: 'X-Frame-Options', value: 'DENY' },
          { key: 'X-Content-Type-Options', value: 'nosniff' },
          { key: 'Referrer-Policy', value: 'strict-origin' },
          {
            key: 'Permissions-Policy',
            value:
              'camera=(), microphone=(), geolocation=(), payment=(), usb=(), interest-cohort=()',
          },
          {
            key: 'Strict-Transport-Security',
            value: 'max-age=63072000; includeSubDomains; preload',
          },
          { key: 'Cross-Origin-Opener-Policy', value: 'same-origin' },
          { key: 'Cross-Origin-Resource-Policy', value: 'same-origin' },
        ],
      },
    ];
  },

  // No telemetry, no remote images.
  images: { remotePatterns: [] },
  experimental: {
    typedRoutes: true,
  },

  // Phase 4 (AGENT-UI): the codebase uses NodeNext-style `.js` import
  // suffixes on `.ts` source files (e.g. `import {...} from './foo.js'`
  // resolves to `./foo.ts`). Next.js 15's default webpack resolver doesn't
  // map `.js` -> `.ts` in our `lib/` tree, so we extend the resolver with
  // an extension-alias entry. This keeps existing route handlers working
  // without rewriting every `.js` suffix.
  webpack: (config) => {
    config.resolve = config.resolve ?? {};
    config.resolve.extensionAlias = {
      ...(config.resolve.extensionAlias ?? {}),
      '.js': ['.ts', '.tsx', '.js'],
      '.mjs': ['.mts', '.mjs'],
      '.cjs': ['.cts', '.cjs'],
    };
    return config;
  },
};

export default nextConfig;
