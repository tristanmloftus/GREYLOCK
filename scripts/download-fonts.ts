// Greylock — one-shot font downloader
// =============================================================================
// AGENT-UI (Phase 4). Downloads IBM Plex Mono and Syne `.woff2` files into
// `app/fonts/` so we can self-host them via `next/font/local`.
//
// Why self-hosted: SPEC §7 / Phase-5 CSP locks `font-src 'self'`. Any
// connection to fonts.googleapis.com or fonts.gstatic.com would break the
// hardened CSP.
//
// Sources: jsDelivr CDN mirrors of @fontsource (the same .woff2 files Google
// Fonts serves), under SIL Open Font License. Downloaded once at setup; the
// .woff2 files are committed to the repo afterwards.
//
// Run: `pnpm tsx scripts/download-fonts.ts`
// =============================================================================

import { createWriteStream, existsSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';

interface FontFile {
  readonly url: string;
  readonly outRelative: string;
}

const FILES: ReadonlyArray<FontFile> = [
  // IBM Plex Mono — three weights via @fontsource jsDelivr mirror.
  {
    url: 'https://cdn.jsdelivr.net/fontsource/fonts/ibm-plex-mono@latest/latin-400-normal.woff2',
    outRelative: 'app/fonts/IBMPlexMono-Regular.woff2',
  },
  {
    url: 'https://cdn.jsdelivr.net/fontsource/fonts/ibm-plex-mono@latest/latin-500-normal.woff2',
    outRelative: 'app/fonts/IBMPlexMono-Medium.woff2',
  },
  {
    url: 'https://cdn.jsdelivr.net/fontsource/fonts/ibm-plex-mono@latest/latin-700-normal.woff2',
    outRelative: 'app/fonts/IBMPlexMono-Bold.woff2',
  },
  // Syne — two weights.
  {
    url: 'https://cdn.jsdelivr.net/fontsource/fonts/syne@latest/latin-400-normal.woff2',
    outRelative: 'app/fonts/Syne-Regular.woff2',
  },
  {
    url: 'https://cdn.jsdelivr.net/fontsource/fonts/syne@latest/latin-700-normal.woff2',
    outRelative: 'app/fonts/Syne-Bold.woff2',
  },
];

async function downloadOne(file: FontFile): Promise<void> {
  const outAbs = resolve(process.cwd(), file.outRelative);
  if (existsSync(outAbs)) {
    process.stdout.write(`  already present: ${file.outRelative}\n`);
    return;
  }
  mkdirSync(dirname(outAbs), { recursive: true });
  process.stdout.write(`  fetching ${file.url}\n`);
  const res = await fetch(file.url);
  if (!res.ok || res.body === null) {
    throw new Error(`fetch failed (${res.status}): ${file.url}`);
  }
  const stream = Readable.fromWeb(res.body as Parameters<typeof Readable.fromWeb>[0]);
  await pipeline(stream, createWriteStream(outAbs));
  process.stdout.write(`  wrote ${file.outRelative}\n`);
}

async function main(): Promise<void> {
  process.stdout.write('downloading fonts to app/fonts/...\n');
  for (const f of FILES) {
    await downloadOne(f);
  }
  process.stdout.write('done.\n');
}

await main();
