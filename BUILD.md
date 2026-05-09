# TerminalFinance — Build Instructions

## macOS (Apple Clang, tested with Xcode CLI tools)

**Install dependencies:**
```sh
brew bundle --file=Brewfile
```

**Configure and build:**
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

**Run tests:**
```sh
ctest --test-dir build
```

**Run the app:**
```sh
./build/TerminalFinance
```
Press `Q` to quit.

---

## TerminalFinanceServer (Phase 2)

The server binary is a separate executable that does not link the TUI.  It
speaks HTTPS, requires a TLS cert + key, and serves `/healthz` in v0.2.

### Install mkcert (one-time)

```sh
brew install mkcert
```

Windows:
```cmd
choco install mkcert
# or: winget install mkcert
```

### Generate dev certificates (one-time per checkout)

```sh
scripts/generate-dev-cert.sh
```

This runs `mkcert -install` (installs the local CA into system/browser trust
stores — idempotent) and writes `dev/cert.pem` + `dev/key.pem` for
`localhost` and `127.0.0.1`.  These files are gitignored.

### Build the server

```sh
cmake --build build --target TerminalFinanceServer
```

### Environment variables

| Variable             | Default        | Description                              |
|----------------------|----------------|------------------------------------------|
| `TF_SERVER_PORT`     | `8443`         | TCP port to listen on                    |
| `TF_SERVER_CERT_PATH`| `dev/cert.pem` | Path to PEM certificate file             |
| `TF_SERVER_KEY_PATH` | `dev/key.pem`  | Path to PEM private key file             |
| `TF_SERVER_BIND_ADDR`| `127.0.0.1`    | Address to bind to (`0.0.0.0` = all NICs)|

### Run the server

```sh
./build/TerminalFinanceServer
```

With custom settings:
```sh
TF_SERVER_PORT=9443 \
TF_SERVER_CERT_PATH=/path/to/cert.pem \
TF_SERVER_KEY_PATH=/path/to/key.pem \
./build/TerminalFinanceServer
```

### Verify with curl

After running `mkcert -install` and starting the server:
```sh
curl https://localhost:8443/healthz
```
Expected response (HTTP 200):
```json
{"ok":true,"version":"0.2"}
```

If mkcert is not installed in the system trust store, add `--cacert`:
```sh
curl --cacert "$(mkcert -CAROOT)/rootCA.pem" https://localhost:8443/healthz
```

### Build and run the integration tests

```sh
cmake --build build --target ServerHealthzTests
ctest --test-dir build -R ServerHealthzTests --output-on-failure
```

All four test cases must pass:
- `Healthz_Returns200WithOkBody` — status 200, JSON `ok:true`
- `Healthz_ContentTypeIsJson` — Content-Type contains `application/json`
- `UnknownPath_Returns404` — status 404 for unknown paths
- `Healthz_TLSVerificationActuallyChecks` — TLS handshake fails with wrong CA (F-2 compliance)

### Test fixtures

`tests/fixtures/` contains pre-generated cert + key files committed to the
repository so CI runners never need mkcert installed:

| File                        | Purpose                                         |
|-----------------------------|-------------------------------------------------|
| `test-cert.pem`             | Server cert for `localhost` + `127.0.0.1`       |
| `test-key.pem`              | Server private key                              |
| `test-ca.pem`               | mkcert root CA (trust anchor for good-CA tests) |
| `wrong-ca.pem`              | Unrelated self-signed CA (for F-2 failure test) |

To regenerate (when certs expire or mkcert CA is rotated):
```sh
scripts/generate-dev-cert.sh --regenerate-fixtures
git add tests/fixtures/
git commit -m "chore: regenerate test TLS fixtures"
```

---

## Windows (MSVC, Visual Studio 2022)

**Prerequisites:**
- Visual Studio 2022 with "Desktop development with C++" workload
- [vcpkg](https://github.com/microsoft/vcpkg) bootstrapped and the `VCPKG_ROOT` env var set
  ```
  git clone https://github.com/microsoft/vcpkg %USERPROFILE%\vcpkg
  %USERPROFILE%\vcpkg\bootstrap-vcpkg.bat
  set VCPKG_ROOT=%USERPROFILE%\vcpkg
  ```

**Configure (from a Developer Command Prompt):**
```cmd
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
      -DCMAKE_BUILD_TYPE=Debug
```

**Build:**
```cmd
cmake --build build --config Debug
```

**Run tests:**
```cmd
ctest --test-dir build -C Debug
```

**Run the app:**
```cmd
build\Debug\TerminalFinance.exe
```

### Windows gotchas

- `vcpkg.json` at the repo root is the dependency manifest. When CMake runs with
  the vcpkg toolchain file, vcpkg will automatically install `libsodium`, `curl`,
  and `openssl` from `vcpkg.json` before configuring. No manual `vcpkg install`
  needed.  **First configure on a cold vcpkg cache can take 5–15 minutes** as
  OpenSSL compiles from source.
- The triplet must be `x64-windows-static` to match the `/MT` runtime used by
  `DataStoreTests`. Using the default dynamic triplet will produce linker errors.
- DPAPI (`crypt32`, `advapi32`) are linked automatically via the `if(WIN32)` block
  in `CMakeLists.txt`. No extra flags needed.
- `HKCU\Software\TerminalFinance\Tokens` is the registry path for stored Plaid
  access tokens. It will be created on first use. To inspect it in regedit, look
  under `HKEY_CURRENT_USER\Software\TerminalFinance\Tokens`.

### Building TerminalFinanceServer on Windows

```cmd
cmake --build build --config Debug --target TerminalFinanceServer
cmake --build build --config Debug --target ServerHealthzTests
ctest --test-dir build -C Debug -R ServerHealthzTests --output-on-failure
```

To generate dev certs on Windows (requires mkcert in PATH):
```cmd
mkcert -install
mkcert -cert-file dev\cert.pem -key-file dev\key.pem localhost 127.0.0.1
```

Run the server:
```cmd
set TF_SERVER_CERT_PATH=dev\cert.pem
set TF_SERVER_KEY_PATH=dev\key.pem
build\Debug\TerminalFinanceServer.exe
```

Verify:
```cmd
curl https://localhost:8443/healthz
```

---

## Dependencies overview

| Dep | Windows source | macOS source | Purpose |
|-----|---------------|-------------|---------|
| ftxui | FetchContent (GitHub) | FetchContent | TUI framework |
| nlohmann/json | FetchContent (GitHub) | FetchContent | JSON |
| googletest | FetchContent (GitHub) | FetchContent | Unit tests |
| libsodium | vcpkg | brew | Crypto (Phase 1+) |
| curl / libcurl | vcpkg | brew | HTTP client (Phase 0.B+) |
| openssl | vcpkg | brew (openssl@3) | HTTPS server TLS (Phase 2+) |
| cpp-httplib | FetchContent (GitHub) | FetchContent | HTTPS server framework (Phase 2+) |
| mkcert | choco / winget | brew | Dev cert generation (Phase 2+) |
| crypt32 / advapi32 | Windows SDK | N/A | DPAPI, registry |
| Security.framework / CoreFoundation | N/A | Xcode SDK | macOS Keychain |
