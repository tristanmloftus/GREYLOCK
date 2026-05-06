# Greylock — Build Log

Append-only record of orchestration decisions, agent spawns, QA verdicts, and phase boundaries. New entries go at the **top**.

---

## 2026-05-06 — Phase 0 — Bootstrap (in progress)

**Decisions made before kickoff:**

| # | Question | Decision |
|---|----------|----------|
| 1 | Master passphrase delivery | macOS Keychain (`security find-generic-password -s greylock-master`) with TTY fallback if Keychain item missing |
| 2 | mkcert install | Spec-spec confirmed: brew install + `mkcert -install` (interactive, run by Rory) |
| 3 | Node version | Spec updated from "20 LTS" to "Node 22 LTS minimum"; dev target is system v25.9.0; `package.json` engines `>=22` |
| 4 | Allowlist | `rory.patrick.loftus@gmail.com`, `tristan.m.loftus@gmail.com`, `cade-placeholder@greylock.invalid` (placeholder — must be replaced before Cade can enroll) |

**Hard-flag carried forward:** Cade's allowlist email is a placeholder. Phase 5 cannot tag `v0.1.0` until a real Cade email is set in `.env` and `.env.example` is updated. AGENT-AUTH must reject the placeholder string at registration time so an accidental enrollment can't succeed.

**Tooling state at Phase 0 entry:**
- Node v25.9.0 ✓
- pnpm 10.33.4 ✓ (installed via `npm install -g pnpm`)
- Homebrew 5.1.6 ✓
- git 2.53.0 ✓
- mkcert 1.4.4 ✓ (installed; `mkcert -install` pending — Rory must run interactively)

**Phase 0 actions taken so far:**
- `mkdir ~/greylock/{docs,certs,scripts}` + `git init`
- Wrote `.gitignore`, `.env.example`, `README.md`, `docs/BUILD_LOG.md`
- (next) `package.json` + `pnpm install` + `tsconfig.json` + `mkcert localhost cert` + initial commit

---
