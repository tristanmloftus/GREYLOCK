#!/usr/bin/env bash
# generate-dev-cert.sh — Mint a local dev TLS cert for TerminalFinanceServer.
#
# Prerequisites:
#   macOS:   brew install mkcert nss   (nss for Firefox/NSS trust; optional)
#   Windows: choco install mkcert      (or winget install mkcert)
#   Linux:   see https://github.com/FiloSottile/mkcert#installation
#
# Usage:
#   scripts/generate-dev-cert.sh           # generates dev/cert.pem + dev/key.pem
#   scripts/generate-dev-cert.sh --regenerate-fixtures  # also updates tests/fixtures/
#
# The generated dev/cert.pem and dev/key.pem are gitignored.  Every developer
# runs this script once after cloning.  CI uses the checked-in
# tests/fixtures/test-cert.pem and test-key.pem instead.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEV_DIR="${REPO_ROOT}/dev"
FIXTURES_DIR="${REPO_ROOT}/tests/fixtures"

# Ensure the dev/ directory exists (it is kept in git via dev/.gitkeep).
mkdir -p "${DEV_DIR}"

# mkcert -install is idempotent: it installs the local CA into the system
# trust stores (and Firefox's NSS db if available).  Safe to run multiple
# times.
echo "==> Running mkcert -install (idempotent)..."
mkcert -install

echo "==> Generating dev/cert.pem + dev/key.pem for localhost + 127.0.0.1..."
mkcert \
  -cert-file "${DEV_DIR}/cert.pem" \
  -key-file  "${DEV_DIR}/key.pem" \
  localhost \
  127.0.0.1

echo ""
echo "Generated:"
echo "  ${DEV_DIR}/cert.pem"
echo "  ${DEV_DIR}/key.pem"
echo ""
echo "Boot the server:"
echo "  cmake --build build --target TerminalFinanceServer"
echo "  ./build/TerminalFinanceServer"
echo ""
echo "Smoke test:"
echo "  curl https://localhost:8443/healthz"
echo "  # expected: {\"ok\":true,\"version\":\"0.2\"}"

# -------------------------------------------------------------------------
# Optional: regenerate the test fixtures in tests/fixtures/.
# This is needed when the mkcert CA is rotated or when the fixtures expire.
# -------------------------------------------------------------------------
if [[ "${1:-}" == "--regenerate-fixtures" ]]; then
  echo ""
  echo "==> Regenerating tests/fixtures/ (--regenerate-fixtures flag)..."
  mkdir -p "${FIXTURES_DIR}"

  mkcert \
    -cert-file "${FIXTURES_DIR}/test-cert.pem" \
    -key-file  "${FIXTURES_DIR}/test-key.pem" \
    localhost \
    127.0.0.1

  # Copy the mkcert root CA so the integration tests can use it as the
  # CURLOPT_CAINFO trust anchor without relying on system trust store state.
  MKCERT_CAROOT="$(mkcert -CAROOT)"
  cp "${MKCERT_CAROOT}/rootCA.pem" "${FIXTURES_DIR}/test-ca.pem"

  # Generate an unrelated self-signed CA for the F-2 TLS-verify test.
  # This cert is NOT trusted by any browser or the mkcert CA chain.
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
    -keyout "${FIXTURES_DIR}/wrong-ca-key.pem" \
    -out    "${FIXTURES_DIR}/wrong-ca.pem" \
    -days 365 -nodes \
    -subj "/CN=WrongCA for TLS-verify test" 2>/dev/null

  echo "Regenerated test fixtures:"
  ls -1 "${FIXTURES_DIR}"
  echo ""
  echo "Remember to commit tests/fixtures/ after regeneration."
fi
