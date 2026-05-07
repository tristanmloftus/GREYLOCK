/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  poweredByHeader: false,

  // Hardening headers — full CSP added in Phase 5.
  async headers() {
    return [
      {
        source: '/:path*',
        headers: [
          { key: 'X-Frame-Options', value: 'DENY' },
          { key: 'X-Content-Type-Options', value: 'nosniff' },
          { key: 'Referrer-Policy', value: 'strict-origin' },
          { key: 'Permissions-Policy', value: 'camera=(), microphone=(), geolocation=()' },
          {
            key: 'Strict-Transport-Security',
            value: 'max-age=63072000; includeSubDomains; preload',
          },
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
