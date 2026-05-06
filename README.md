# Greylock

Private, paranoid-grade financial operating dashboard for Platinum Creek Capital (PCC). Localhost-only. Passkey-only. Three users.

> NOT FINANCIAL ADVICE. NOT A REGISTERED PRODUCT. INTERNAL USE ONLY.

---

## What this is

Greylock unifies personal (`#me`) and PCC (`#pcc`) financial state behind one passkey-gated dashboard running on `https://localhost:3000`. It pulls data from Plaid, encrypts every access token at rest, and computes net worth, cash position, and progress toward the $1B north star. There is no cloud component. There is no production deploy. There is no password.

## Operators

- Rory Loftus — owner
- Tristan Loftus
- Cade — placeholder allowlist entry until real email is set

## Stack

Node 22+ (dev: 25.9.0) · pnpm · Next.js 15 (App Router) · TypeScript strict · Prisma + SQLCipher · `@simplewebauthn` (passkey/WebAuthn) · iron-session · Plaid SDK · Node `crypto` (AES-256-GCM, scrypt, HKDF) · Zod · mkcert · Vitest + Playwright · ESLint + `eslint-plugin-security` + Prettier.

## Setup (one-time, ~10 min)

Prerequisites: macOS, Homebrew, Node 22+, mkcert.

```bash
# 1. Install local CA (interactive — sudo + Keychain prompt)
mkcert -install

# 2. Issue localhost cert
pnpm setup:certs

# 3. Install deps
pnpm install

# 4. Seed the master passphrase into macOS Keychain (one-time)
security add-generic-password -s greylock-master -a "$USER" -w '<your-strong-passphrase>' -U

# 5. Fill in .env from .env.example
cp .env.example .env
# generate SESSION_SECRET and CRYPTO_PEPPER:
node -e "console.log('SESSION_SECRET=' + require('crypto').randomBytes(32).toString('base64'))"
node -e "console.log('CRYPTO_PEPPER='   + require('crypto').randomBytes(32).toString('base64'))"

# 6. Initial DB migration
pnpm prisma migrate dev

# 7. Enroll your passkey via the admin CLI
pnpm admin:enroll rory.patrick.loftus@gmail.com
# follow the printed URL, complete the WebAuthn ceremony in your browser

# 8. Start
pnpm dev          # web (localhost:3000)
pnpm sync         # background sync loop (separate terminal)
```

## Architecture (1-paragraph)

Two-tier encryption. **Personal** Plaid tokens are encrypted with a per-user DEK wrapped by a KEK derived from that user's passkey credential — server cannot read User A's data without User A logged in. **PCC** Plaid tokens are encrypted with a server-held PCC DEK, which is itself wrapped by a Master KEK derived (via scrypt) from a master passphrase fetched from macOS Keychain at server start. The PCC DEK lives in process memory for the server's lifetime so the 15-min sync loop runs 24/7. Trade-off accepted: anyone with the running process and the master passphrase can read PCC data; mitigated by localhost-only deploy, encrypted disk, audit log on every PCC decrypt, and rotation on master-passphrase change.

Full design: [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md). Threat model: [`docs/THREAT_MODEL.md`](./docs/THREAT_MODEL.md). Operations: [`docs/RUNBOOK.md`](./docs/RUNBOOK.md).

## Recovery

There is no password. There is no email reset. If you lose your passkey:

```bash
pnpm admin:revoke <email>      # invalidate existing passkey
pnpm admin:enroll  <email>     # issue new enrollment URL
```

If a laptop is lost: rotate the master passphrase (`pnpm admin:rotate-master`), revoke all sessions (`pnpm admin:revoke-all`), and re-enroll passkeys.

## License

Private. Not licensed for redistribution.
