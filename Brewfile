# Brewfile — macOS dependencies for TerminalFinance v0.2
# Run: brew bundle --file=Brewfile

brew "libsodium"   # Phase 1: cross-platform crypto (ISecretStore / envelope encryption)
brew "curl"        # Phase 0.B: IHttpClient / libcurl (replaces winhttp on macOS)
brew "openssl@3"   # Phase 2: TerminalFinanceServer HTTPS (cpp-httplib requires OpenSSL >= 1.1.1)
brew "mkcert"      # Phase 2: local dev cert generation (run scripts/generate-dev-cert.sh)
brew "sqlcipher"   # Phase 4.E: SQLCipher AES-256 at-rest database encryption
