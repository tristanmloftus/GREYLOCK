# Contributing

Build, test, and dev workflow for TerminalFinance v0.2 across Linux,
macOS, and Windows. For the system overview see
[ARCHITECTURE.md](ARCHITECTURE.md); for security expectations see
[THREAT_MODEL.md](THREAT_MODEL.md); for running a server see
[RUNBOOK.md](RUNBOOK.md). [`../BUILD.md`](../BUILD.md) remains the deep
per-platform cookbook with manual smoke-test recipes.

## Get the code

```sh
git clone https://github.com/tristanmloftus/GREYLOCK.git
cd GREYLOCK
git checkout v0.2-dev
```

The default branch is `main` (v0.1). v0.2 work is on `v0.2-dev`.

## Per-platform build

The build system is CMake. Dependencies are sourced per platform:

- **macOS:** Homebrew (`Brewfile`).
- **Windows:** vcpkg manifest mode (`vcpkg.json`).
- **Linux:** distribution packages (`apt` on Debian/Ubuntu).

### macOS

```sh
# Install dependencies (one-time).
brew bundle --file=Brewfile

# Configure + build (server + client + all tests).
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4

# Run tests.
ctest --test-dir build --output-on-failure
```

`Brewfile` installs libsodium, curl, openssl@3, mkcert, and sqlcipher.
The CMake configure step probes `brew --prefix` for each
(`CMakeLists.txt:89-110, 147-162, 248-264, 615-631`), so no extra flags
are needed regardless of Intel vs Apple Silicon. `sudo` is not required.

### Windows

```cmd
:: One-time vcpkg setup
git clone https://github.com/microsoft/vcpkg %USERPROFILE%\vcpkg
%USERPROFILE%\vcpkg\bootstrap-vcpkg.bat
set VCPKG_ROOT=%USERPROFILE%\vcpkg

:: Configure (from a Developer Command Prompt for VS 2022).
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
      -DCMAKE_BUILD_TYPE=Release

:: Build.
cmake --build build --config Release

:: Test.
ctest --test-dir build -C Release --output-on-failure
```

`vcpkg.json` declares `libsodium`, `curl` (features `openssl`,
`non-http`), `openssl`, and `sqlcipher`. The triplet **must** be
`x64-windows-static` (`BUILD.md:273-275`) to match the `/MT` runtime that
GoogleTest links against. First configure on a cold vcpkg cache can take
5-15 minutes because OpenSSL compiles from source.

### Linux

```sh
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    curl \
    pkg-config \
    libsodium-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    libsqlcipher-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

`SecretStoreTests` is conditional on `WIN32 OR APPLE`
(`CMakeLists.txt:443`); on Linux it is not built (the project has no
Linux `ISecretStore` impl). On memory-constrained hosts (the skynet CI
VM has 3.8 GiB), build with `-j 1`:

```sh
cmake --build build -j 1
```

— matching `.github/workflows/ci.yml:55`.

## Running tests locally

```sh
ctest --test-dir build --output-on-failure -j 4
```

To run a single target:

```sh
ctest --test-dir build -R AuditReplayTests --output-on-failure
```

Network-touching CurlHttpClient tests skip themselves unless
`TF_NETWORK_TESTS=1` is set (`CMakeLists.txt:404-433`).

### Snapshot tests

The TUI widget snapshot harness lives in `tests/snapshot/`. The harness
renders each FTXUI widget at fixed `80x24` and diffs against checked-in
fixtures. To refresh fixtures after an intentional widget change:

```sh
TF_UPDATE_SNAPSHOTS=1 ctest --test-dir build -R WidgetSnapshot --output-on-failure
git diff tests/snapshot/fixtures/    # inspect, then either accept or revert
git add tests/snapshot/fixtures/
```

The `WidgetSnapshotTests` ctest target runs with
`WORKING_DIRECTORY = ${CMAKE_CURRENT_SOURCE_DIR}` so the default fixture
path resolves regardless of where ctest was invoked from
(`CMakeLists.txt:1612-1617`). Full contract in
[`../tests/snapshot/README.md`](../tests/snapshot/README.md).

## Coverage

A coverage-enabled build option (`TF_COVERAGE`) is **not yet wired into
`CMakeLists.txt`** at the current HEAD; introducing it is a Phase 6
test-engineer task.

Once `-DTF_COVERAGE=ON` exists, the intended invocation is:

```sh
cmake -S . -B build-cov -DTF_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cov
cd build-cov && ctest && cd ..
gcovr -r . \
  --filter 'src/' \
  --filter 'server/' \
  --exclude '_deps' \
  --exclude 'tests/' \
  --txt
```

Until that lands, this section is `[Phase 6 test-engineer adding]` and
the per-target coverage flags must be passed manually
(`-fprofile-arcs -ftest-coverage`).

## Code style

- **Language standard:** C++20
  (`CMakeLists.txt:7-8`). RAII for resources; no exceptions across
  module boundaries (the audit log and crypto layers catch internally
  and surface structured errors).
- **Secrets:** use `sodium_memzero` after any plaintext key, token, or
  passphrase touches memory. Examples:
  `src/main.cpp:387-400,432-434`, `PlaidTokenBroker.cpp:74,140,150-153`,
  `server/main.cpp:339-342`.
- **Constant-time compare:** `sodium_memcmp` (libsodium native) — see
  the timing-equalization note in
  `server/auth/AuthHandlers.cpp:42-58` for an example of using a dummy
  Argon2id hash to avoid leaking user existence.
- **Zeroizing buffers:** for in-scope plaintext, use
  `tf::crypto::ZeroizingBuffer` (`src/services/crypto/Zeroize.h`).
- **HTTP headers:** never pass caller-supplied data into a header name or
  value without going through `CurlHttpClient`'s CRLF check
  (`CurlHttpClient.cpp:47-50,181-189`).
- **Audit details:** route through `tf::audit::Sanitizer` — anything
  resembling a token, key, or PEM blob is rejected wholesale
  (`server/audit/Sanitizer.cpp`).

## Commit conventions

[Conventional Commits](https://www.conventionalcommits.org/) with an
optional scope. The history on `v0.2-dev` is the reference:

| Type | Examples |
|------|----------|
| `feat` | `feat(server): GET /supplier-map endpoint (session-gated)` |
| `fix` | `fix(security): zero PlaidTokenBroker master_key_ immediately after DEK derivation` |
| `refactor` | `refactor(discovery): extract SupplierMapping to data/supplier_map.json` |
| `test` | `test(consolidation): cross-account transfer + idempotency + hash-stability cases` |
| `docs` | `docs(architecture): system architecture for v0.2` |
| `build` | `build(ci): switch to self-hosted Linux runner; guard SecretStoreTests on Linux` |
| `ci` | `fix(ci): drop to -j 1 to keep self-hosted runner alive` |
| `chore` | rare; e.g., `chore: regenerate test TLS fixtures` |

The body should explain the **why** in 1–3 sentences; the diff explains
the **what**. Cite a guardrail (`F-1` … `F-5`) when the change touches
authentication, crypto, or audit code.

## Submitting changes

1. Branch off `v0.2-dev` (not `main`).
2. Keep commits small and individually buildable; CI runs every push.
3. Push to your fork or directly to a topic branch on the main repo
   (depending on access).
4. Open a PR against `v0.2-dev`. CI must be green before merge.
5. Security-relevant changes (anything in `server/auth/`,
   `server/audit/`, `server/plaid/`, or `src/services/crypto/`) get a
   security-reviewer pass — call it out in the PR description.

## CI

The current `.github/workflows/ci.yml` (at `4b4c861`) runs a single job
on a self-hosted Linux runner labelled
`[self-hosted, linux, skynet]`. It performs:

1. `actions/checkout@v4`
2. `actions/cache@v4` keyed on `runner.os + hashFiles('CMakeLists.txt')`
   for `build/_deps` (FetchContent caches).
3. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
4. `cmake --build build --config Release -j 1`
   (`-j 1` is intentional — see `ci.yml:53-55` and the comment about
    OOM on the 3.8 GiB VM).
5. `ctest --test-dir build -C Release --output-on-failure`
6. On failure: upload `build/Testing/Temporary/LastTest.log` and
   `build/CMakeFiles/CMakeOutput.log` artifacts.

The macOS and Windows runners are **dev-only** at present. Hosted CI for
those platforms is a Phase 7 task; the matrix was dropped in
`build(ci): switch to self-hosted Linux runner` after the original
hosted matrix exhausted the project's Actions budget. The CMake config
itself is platform-conditional and verified to configure cleanly on
both macOS and Windows in local dev.

## Related documents

- [ARCHITECTURE.md](ARCHITECTURE.md) — what you are building.
- [THREAT_MODEL.md](THREAT_MODEL.md) — what the security review will
  look at.
- [RUNBOOK.md](RUNBOOK.md) — what production looks like.
- [MIGRATION_V0.1_TO_V0.2.md](MIGRATION_V0.1_TO_V0.2.md) — operator
  migration path.
- [`../BUILD.md`](../BUILD.md) — full per-platform recipes (including
  manual /healthz curl commands and Windows mkcert flow).
- [`../tests/snapshot/README.md`](../tests/snapshot/README.md) —
  snapshot test harness specifics.
- [`../V0_2_PLAN.md`](../V0_2_PLAN.md) — the six-phase plan.
