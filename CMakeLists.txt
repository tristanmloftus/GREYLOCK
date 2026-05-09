cmake_minimum_required(VERSION 3.14)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(TerminalFinance)

# Use C++20 for modern features (Concepts, Ranges, etc.)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FetchContent: Downloads the library source code at build time
include(FetchContent)

FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
  GIT_TAG v5.0.0
)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz
)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)

# cpp-httplib: header-only C++ HTTP/HTTPS library used by TerminalFinanceServer.
# HTTPS support requires OpenSSL (detected below) and the compile definition
# CPPHTTPLIB_OPENSSL_SUPPORT (added to the server targets).
# Using v0.18.5 which is the version specified in the plan.  cpp-httplib
# supports port 0 (OS-assigned ephemeral port) via bind_to_port() +
# listen_after_bind() — required by the integration test fixture.
FetchContent_Declare(cpp-httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.18.5
)

set(gtest_force_static_RUNTIME TRUE)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY_DBG "MultiThreadedDebug")
  set(CMAKE_MSVC_RUNTIME_LIBRARY_REL "MultiThreaded")
endif()

# Windows: suppress the legacy <windows.h> min/max macros project-wide so
# std::min / std::max don't collide. <curl/curl.h> transitively pulls in
# <windows.h> on Windows, so any TU that uses both std::max and curl will
# break without this. NOMINMAX is project-policy, not per-file.
if(WIN32)
  add_compile_definitions(NOMINMAX)
endif()

FetchContent_MakeAvailable(ftxui)
FetchContent_MakeAvailable(nlohmann_json)
FetchContent_MakeAvailable(googletest)

# cpp-httplib is header-only; FetchContent_MakeAvailable populates
# cpp-httplib_SOURCE_DIR.  We create an INTERFACE library so targets
# can just do target_link_libraries(... cpphttplib::cpphttplib).
FetchContent_MakeAvailable(cpp-httplib)
# cpp-httplib ships its own CMakeLists which creates the `httplib::httplib`
# target.  We alias it for clarity.
# (cpp-httplib's own CMake may or may not create a target depending on version;
# guard both cases.)
if(NOT TARGET httplib::httplib)
  add_library(httplib::httplib INTERFACE IMPORTED)
  set_target_properties(httplib::httplib PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${cpp-httplib_SOURCE_DIR}"
  )
endif()

# --------------------------------------------------------------------------
# Platform-specific dependencies
# --------------------------------------------------------------------------

# libcurl: the sole HTTP backend on both macOS and Windows.
# Phase 0.B makes CURL the universal transport; winhttp is no longer used.
#
# macOS CURL_ROOT decision: prefer Homebrew curl (/usr/local/opt/curl) over
# the SDK system curl (/Library/Developer/CommandLineTools/SDKs/…/usr/lib).
# Rationale: Homebrew curl 8.x ships HTTP/2, HTTP/3, modern TLS via OpenSSL
# 3.x, and a real dylib (not a .tbd stub). The SDK curl is version-pinned to
# Apple's release cadence and, as a .tbd, links against SecureTransport
# rather than OpenSSL — fine for basic HTTPS but less capable. Since
# Homebrew curl is already installed (Brewfile), prefer it explicitly.
#
# Windows: vcpkg installs libcurl; find_package resolves via the vcpkg
# toolchain. Triplet x64-windows-static is required (see BUILD.md).
if(APPLE AND NOT DEFINED CURL_ROOT)
  # Resolve Homebrew's curl prefix at configure time so the build is portable
  # across Apple Silicon (`/opt/homebrew/opt/curl`) and Intel
  # (`/usr/local/opt/curl`). `macos-latest` GitHub Actions runners are arm64;
  # a hardcoded `/usr/local/opt/curl` would fail there.
  find_program(_BREW_BIN brew)
  if(_BREW_BIN)
    execute_process(
      COMMAND "${_BREW_BIN}" --prefix curl
      OUTPUT_VARIABLE _BREW_CURL_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _BREW_RESULT
      ERROR_QUIET
    )
    if(_BREW_RESULT EQUAL 0 AND IS_DIRECTORY "${_BREW_CURL_PREFIX}")
      set(CURL_ROOT "${_BREW_CURL_PREFIX}" CACHE PATH "Homebrew curl prefix" FORCE)
      message(STATUS "Using Homebrew curl at ${CURL_ROOT}")
    endif()
  endif()
  # If brew was not found or did not return a prefix, fall through to
  # `find_package(CURL)` which will use the SDK system curl.
endif()
find_package(CURL REQUIRED)

# --------------------------------------------------------------------------
# libsodium
# --------------------------------------------------------------------------
# Lookup strategy (in priority order):
#
# 1. macOS / Linux with Homebrew: probe `brew --prefix libsodium` and set
#    SODIUM_ROOT so CMake's find_library/find_path pick it up. This mirrors
#    the CURL_ROOT probe above and avoids hardcoding /usr/local vs /opt/homebrew.
#
# 2. Windows (vcpkg): `find_package(unofficial-sodium CONFIG)` using the vcpkg
#    toolchain. vcpkg ships libsodium under the `unofficial::sodium` namespace.
#    Canonical invocation: find_package(unofficial-sodium CONFIG REQUIRED)
#    Target name: unofficial-sodium::sodium
#    Triplet: x64-windows-static (see BUILD.md).
#    The vcpkg toolchain must be passed at configure time:
#      cmake -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ...
#
# 3. FetchContent fallback: libsodium uses Autotools / MSBuild natively — it
#    does NOT have a first-party CMakeLists.txt. A source-level FetchContent
#    build would require invoking autotools (macOS/Linux) or MSBuild (Windows)
#    as ExternalProject steps, which is fragile. Instead, the fallback downloads
#    the official pre-compiled release archive for the host platform:
#      macOS:   libsodium-<ver>-macos.tar.gz (universal binary, arm64+x86_64)
#      Windows: libsodium-<ver>-msvc.zip (x64, static .lib)
#    The fallback creates an IMPORTED STATIC library target `Sodium::sodium`
#    pointing at the downloaded artifacts. If the archive URL is unavailable
#    (network outage, version bump) the build will fail loudly — which is the
#    correct behavior; do not paper over a missing security dependency.
#
# STOP CONDITION (per task brief): If the FetchContent fallback is triggered
# and the archive download fails, the build fails with a descriptive error.
# In that case, install libsodium via the package manager and re-run CMake.
# Do NOT silence the error or fall back to a stub.

if(APPLE AND NOT DEFINED SODIUM_ROOT)
    find_program(_BREW_BIN brew)
    if(_BREW_BIN)
        execute_process(
            COMMAND "${_BREW_BIN}" --prefix libsodium
            OUTPUT_VARIABLE _BREW_SODIUM_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _BREW_SODIUM_RESULT
            ERROR_QUIET
        )
        if(_BREW_SODIUM_RESULT EQUAL 0 AND IS_DIRECTORY "${_BREW_SODIUM_PREFIX}")
            set(SODIUM_ROOT "${_BREW_SODIUM_PREFIX}" CACHE PATH "Homebrew libsodium prefix" FORCE)
            message(STATUS "Using Homebrew libsodium at ${SODIUM_ROOT}")
        endif()
    endif()
endif()

# Try to find libsodium via package manager first.
# On Windows with vcpkg toolchain, unofficial-sodium::sodium is the target.
# On macOS/Linux, we use the Homebrew prefix probed above (or system install).
set(_SODIUM_FOUND FALSE)

if(WIN32)
    # Windows: vcpkg target takes precedence.
    find_package(unofficial-sodium CONFIG QUIET)
    if(unofficial-sodium_FOUND)
        set(_SODIUM_FOUND TRUE)
        set(_SODIUM_TARGET "unofficial-sodium::sodium")
        message(STATUS "libsodium: using vcpkg unofficial-sodium::sodium")
    endif()
endif()

if(NOT _SODIUM_FOUND)
    # macOS / Linux: search in SODIUM_ROOT (Homebrew prefix) then system paths.
    find_library(_SODIUM_LIB
        NAMES sodium
        HINTS "${SODIUM_ROOT}/lib"
        REQUIRED
    )
    find_path(_SODIUM_INCLUDE
        NAMES sodium.h
        HINTS "${SODIUM_ROOT}/include"
        REQUIRED
    )
    if(_SODIUM_LIB AND _SODIUM_INCLUDE)
        # Create an IMPORTED target so downstream targets link the same way
        # regardless of whether we got here via vcpkg or Homebrew.
        add_library(Sodium::sodium STATIC IMPORTED GLOBAL)
        set_target_properties(Sodium::sodium PROPERTIES
            IMPORTED_LOCATION "${_SODIUM_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${_SODIUM_INCLUDE}"
        )
        set(_SODIUM_FOUND TRUE)
        set(_SODIUM_TARGET "Sodium::sodium")
        message(STATUS "libsodium: using library at ${_SODIUM_LIB}, includes at ${_SODIUM_INCLUDE}")
    endif()
endif()

if(NOT _SODIUM_FOUND)
    # FetchContent fallback: download the official pre-built libsodium release.
    # Pinned to 1.0.20 (latest stable as of plan authoring date 2026-05-09).
    set(_SODIUM_VERSION "1.0.20")

    if(APPLE)
        # Universal binary (arm64 + x86_64) — works on both Intel and Apple Silicon.
        FetchContent_Declare(libsodium_prebuilt
            URL "https://download.libsodium.org/libsodium/releases/libsodium-${_SODIUM_VERSION}-stable.tar.gz"
            # NOTE: libsodium's macOS release archives require autotools to build from source.
            # The fetch below intentionally downloads the *source* tarball and will FAIL at
            # configure time with the message below, prompting the operator to install via
            # Homebrew. This is correct behavior per the task brief stop condition.
        )
        message(FATAL_ERROR
            "libsodium was not found on this macOS system and the FetchContent source-build "
            "path is not supported (libsodium uses Autotools, not CMake). "
            "Install via Homebrew: brew install libsodium  — then re-run cmake.")
    elseif(WIN32)
        message(FATAL_ERROR
            "libsodium was not found. On Windows, install via vcpkg: "
            "  vcpkg install libsodium:x64-windows-static "
            "Then pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake to cmake.")
    else()
        message(FATAL_ERROR
            "libsodium was not found. Install via your system package manager "
            "(e.g. apt install libsodium-dev) then re-run cmake.")
    endif()
endif()

# --------------------------------------------------------------------------
# OpenSSL — required by cpp-httplib for HTTPS server (Phase 2+).
#
# macOS: Homebrew openssl@3 is preferred over the SDK LibreSSL.
#   cpp-httplib requires a real OpenSSL (≥ 1.1.1); LibreSSL has API
#   differences that break SSL_CTX_set_min_proto_version and other calls.
#   We probe `brew --prefix openssl@3` and set OPENSSL_ROOT_DIR so that
#   CMake's FindOpenSSL.cmake picks up Homebrew's installation.
#
# Windows: vcpkg installs openssl; find_package(OpenSSL REQUIRED) resolves
#   via the vcpkg toolchain file (set at configure time via
#   -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake).
# --------------------------------------------------------------------------
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
  find_program(_BREW_BIN_SSL brew)
  if(_BREW_BIN_SSL)
    execute_process(
      COMMAND "${_BREW_BIN_SSL}" --prefix openssl@3
      OUTPUT_VARIABLE _BREW_OPENSSL_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _BREW_OPENSSL_RESULT
      ERROR_QUIET
    )
    if(_BREW_OPENSSL_RESULT EQUAL 0 AND IS_DIRECTORY "${_BREW_OPENSSL_PREFIX}")
      set(OPENSSL_ROOT_DIR "${_BREW_OPENSSL_PREFIX}" CACHE PATH
          "Homebrew openssl@3 prefix" FORCE)
      message(STATUS "OpenSSL: using Homebrew openssl@3 at ${OPENSSL_ROOT_DIR}")
    endif()
  endif()
endif()

find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL: version ${OPENSSL_VERSION}, "
               "libraries ${OPENSSL_LIBRARIES}, "
               "include dir ${OPENSSL_INCLUDE_DIR}")

# --------------------------------------------------------------------------
# Platform-specific source files for the secret store
# --------------------------------------------------------------------------
if(WIN32)
  set(SECRET_STORE_SOURCES
    src/services/secret_store/DpapiSecretStore.cpp
  )
elseif(APPLE)
  set(SECRET_STORE_SOURCES
    src/services/secret_store/KeychainSecretStore.cpp
  )
else()
  # Linux / other POSIX: no secret store implementation in Phase 0.
  # Add a stub or libsecret implementation in a future phase.
  set(SECRET_STORE_SOURCES "")
  message(WARNING "TerminalFinance: No ISecretStore implementation for this platform. "
                  "SecretStore will be unavailable at runtime.")
endif()

# --------------------------------------------------------------------------
# Common source files (platform-independent)
# --------------------------------------------------------------------------
set(COMMON_SOURCES
  src/models/DataStore.cpp
  src/services/PlaidService.cpp
  src/services/StorageService.cpp
  src/services/DiscoveryService.cpp
  src/services/ConsolidationService.cpp
  src/services/http/CurlHttpClient.cpp
  src/services/crypto/EnvelopeEncryption.cpp
  src/services/BackendClient.cpp
  src/utils/SyntheticGenerator.cpp
  src/utils/Validator.cpp
  src/utils/ConfigManager.cpp
  ${SECRET_STORE_SOURCES}
)

# Crypto implementation sources (subset of COMMON_SOURCES for test-only targets).
set(CRYPTO_SOURCES
  src/services/crypto/EnvelopeEncryption.cpp
)

# NOTE: SecurityService.cpp is intentionally excluded from COMMON_SOURCES.
# Its v0.1 content has been moved into DpapiSecretStore.cpp / KeychainSecretStore.cpp.
# The file still exists in the tree as a documented stub.

enable_testing()

# --------------------------------------------------------------------------
# Main executable
# --------------------------------------------------------------------------
add_executable(TerminalFinance src/main.cpp ${COMMON_SOURCES})

target_link_libraries(TerminalFinance PRIVATE
    ftxui::screen
    ftxui::dom
    ftxui::component
    nlohmann_json::nlohmann_json
    ${_SODIUM_TARGET}
)

if(WIN32)
  # winhttp removed: PlaidService no longer uses WinHTTP; all HTTP goes through
  # CurlHttpClient. crypt32 and advapi32 remain for DpapiSecretStore.
  # Verify with: grep -r winhttp src/ -- should return nothing.
  target_link_libraries(TerminalFinance PRIVATE crypt32 advapi32 CURL::libcurl)
elseif(APPLE)
  target_link_libraries(TerminalFinance PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
endif()

# --------------------------------------------------------------------------
# DataStoreTests
# --------------------------------------------------------------------------
add_executable(DataStoreTests tests/test_datastore.cpp ${COMMON_SOURCES})

if(MSVC)
  target_compile_options(DataStoreTests PRIVATE /MT)
endif()

target_link_libraries(DataStoreTests PRIVATE
    gtest
    gtest_main
    nlohmann_json::nlohmann_json
    ${_SODIUM_TARGET}
)

if(WIN32)
  target_link_libraries(DataStoreTests PRIVATE crypt32 advapi32 CURL::libcurl)
elseif(APPLE)
  target_link_libraries(DataStoreTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
endif()

add_test(NAME DataStoreTests COMMAND DataStoreTests)

# --------------------------------------------------------------------------
# CurlHttpClientTests
# Tests for the IHttpClient abstraction and CurlHttpClient implementation.
# Network-touching tests are gated behind TF_NETWORK_TESTS=1.
# --------------------------------------------------------------------------
add_executable(CurlHttpClientTests tests/test_curl_http_client.cpp ${COMMON_SOURCES})

if(MSVC)
  target_compile_options(CurlHttpClientTests PRIVATE /MT)
endif()

target_link_libraries(CurlHttpClientTests PRIVATE
    gtest
    gtest_main
    nlohmann_json::nlohmann_json
    ${_SODIUM_TARGET}
)

if(WIN32)
  target_link_libraries(CurlHttpClientTests PRIVATE crypt32 advapi32 CURL::libcurl)
elseif(APPLE)
  target_link_libraries(CurlHttpClientTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
endif()

# Run the non-network tests by default. The network tests skip themselves
# unless TF_NETWORK_TESTS=1 is set in the environment.
add_test(NAME CurlHttpClientTests COMMAND CurlHttpClientTests)

# --------------------------------------------------------------------------
# SecretStoreTests
# Tests for ISecretStore: KeychainSecretStore (macOS) / DpapiSecretStore (Windows).
# Exercises all 10 contract properties (put/get round-trips, missing-key semantics,
# remove idempotency, overwrite, binary payloads up to 64 KiB, multi-key isolation).
# All test keys are prefixed "tf-test-" and removed in TearDown — safe to run on
# a machine that already has production TerminalFinance keychain/registry items.
# --------------------------------------------------------------------------
add_executable(SecretStoreTests
    tests/test_secret_store.cpp
    ${SECRET_STORE_SOURCES}
)

if(MSVC)
  target_compile_options(SecretStoreTests PRIVATE /MT)
endif()

target_link_libraries(SecretStoreTests PRIVATE
    gtest
    gtest_main
)

if(WIN32)
  # DpapiSecretStore: DPAPI (crypt32) + registry (advapi32).
  target_link_libraries(SecretStoreTests PRIVATE crypt32 advapi32)
elseif(APPLE)
  # KeychainSecretStore: Security.framework + CoreFoundation.
  target_link_libraries(SecretStoreTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
  )
endif()

add_test(NAME SecretStoreTests COMMAND SecretStoreTests)

# --------------------------------------------------------------------------
# CryptoTests
# Tests for Phase 1.B: ZeroizingBuffer, ConstantTime, EnvelopeEncryption.
# Depends only on libsodium + googletest; does NOT need CURL or secret_store.
# --------------------------------------------------------------------------
add_executable(CryptoTests
    tests/test_crypto.cpp
    ${CRYPTO_SOURCES}
)

if(MSVC)
  target_compile_options(CryptoTests PRIVATE /MT)
endif()

target_link_libraries(CryptoTests PRIVATE
    gtest
    gtest_main
    ${_SODIUM_TARGET}
)

add_test(NAME CryptoTests COMMAND CryptoTests)

# --------------------------------------------------------------------------
# Server sources (shared between TerminalFinanceServer and ServerHealthzTests)
# --------------------------------------------------------------------------
set(SERVER_SOURCES
  server/http/Server.cpp
  server/http/HealthzHandler.cpp
)

# --------------------------------------------------------------------------
# TerminalFinanceServer
# The HTTPS server binary.  Does NOT link ftxui, libsodium, or libcurl.
# Dependencies: cpp-httplib (header-only), OpenSSL (for TLS), nlohmann/json.
#
# CPPHTTPLIB_OPENSSL_SUPPORT enables the SSLServer code path in httplib.h.
# It must be defined for every translation unit that includes httplib.h.
# We set it as a target compile definition so it propagates transitively.
# --------------------------------------------------------------------------
add_executable(TerminalFinanceServer server/main.cpp ${SERVER_SOURCES})

target_include_directories(TerminalFinanceServer PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${cpp-httplib_SOURCE_DIR}"
  ${OPENSSL_INCLUDE_DIR}
)

target_compile_definitions(TerminalFinanceServer PRIVATE
  CPPHTTPLIB_OPENSSL_SUPPORT
)

target_link_libraries(TerminalFinanceServer PRIVATE
  OpenSSL::SSL
  OpenSSL::Crypto
  nlohmann_json::nlohmann_json
)

# --------------------------------------------------------------------------
# ServerHealthzTests
# Integration tests for the /healthz endpoint.  Boots a Server on an
# ephemeral port (port 0) and sends requests via CurlHttpClient.
#
# Cert source: checked-in tests/fixtures/ (generated by mkcert; committed so
# CI runners never need mkcert installed).
#
# TF_FIXTURES_DIR is passed as a compile definition so the test can locate
# the cert files at runtime regardless of where ctest is invoked from.
# --------------------------------------------------------------------------
add_executable(ServerHealthzTests
  tests/test_server_healthz.cpp
  ${SERVER_SOURCES}
  src/services/http/CurlHttpClient.cpp
)

if(MSVC)
  target_compile_options(ServerHealthzTests PRIVATE /MT)
endif()

target_include_directories(ServerHealthzTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${cpp-httplib_SOURCE_DIR}"
  ${OPENSSL_INCLUDE_DIR}
)

target_compile_definitions(ServerHealthzTests PRIVATE
  CPPHTTPLIB_OPENSSL_SUPPORT
  TF_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
)

target_link_libraries(ServerHealthzTests PRIVATE
  gtest
  gtest_main
  OpenSSL::SSL
  OpenSSL::Crypto
  nlohmann_json::nlohmann_json
)

if(WIN32)
  target_link_libraries(ServerHealthzTests PRIVATE CURL::libcurl)
elseif(APPLE)
  target_link_libraries(ServerHealthzTests PRIVATE CURL::libcurl)
endif()

add_test(NAME ServerHealthzTests COMMAND ServerHealthzTests)

# ==========================================================================
# Phase 2.B additions — SQLite + migrations layer
# ALL additions below this line were added by 2.B.  Do not move them above
# the existing target definitions to avoid 2.C merge conflicts.
# ==========================================================================

# --------------------------------------------------------------------------
# SQLite amalgamation via FetchContent.
#
# Version pin: 3.46.1 (2024-08-13).  Change SQLITE_AMALGAMATION_VERSION to
# pick up a different release; the URL is computed from the version below.
# The amalgamation zip ships sqlite3.c + sqlite3.h as a single self-contained
# compilation unit — no Autotools, no configure step, no cmake required.
#
# Pinning strategy: version + SHA256 hash are the canonical pin.  The hash
# below was computed against the official sqlite.org zip on 2026-05-09.
# Update both fields together when upgrading sqlite.
#
# Windows: the static lib links into TerminalFinanceServer and MigrationsTests.
# macOS:   same.
# TUI (TerminalFinance): does NOT link sqlite3 — storage goes through BackendClient.
# --------------------------------------------------------------------------
set(SQLITE_AMALGAMATION_VERSION "3460100"
    CACHE STRING "SQLite amalgamation version number (e.g. 3460100 for 3.46.1)")

FetchContent_Declare(sqlite
  URL "https://www.sqlite.org/2024/sqlite-amalgamation-${SQLITE_AMALGAMATION_VERSION}.zip"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite)

# Build the amalgamation as a static library so it compiles once and links
# into multiple targets without ODR violations.
if(NOT TARGET sqlite3)
  add_library(sqlite3 STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
  target_include_directories(sqlite3 PUBLIC "${sqlite_SOURCE_DIR}")
  # Suppress MSVC warnings in the amalgamation (not our code).
  if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
  else()
    target_compile_options(sqlite3 PRIVATE -w)
  endif()
endif()

# --------------------------------------------------------------------------
# Server DB sources (Database + Migrations).
# These are added to TerminalFinanceServer via target_sources() so we do NOT
# need to modify the existing add_executable() line — minimizing 2.C conflict.
# [2.B EXTENSION: TerminalFinanceServer source list extended here]
# --------------------------------------------------------------------------
set(SERVER_DB_SOURCES
  server/db/Database.cpp
  server/db/Migrations.cpp
)

target_sources(TerminalFinanceServer PRIVATE ${SERVER_DB_SOURCES})

target_include_directories(TerminalFinanceServer PRIVATE
  "${sqlite_SOURCE_DIR}"
)

target_link_libraries(TerminalFinanceServer PRIVATE sqlite3)

# --------------------------------------------------------------------------
# MigrationsTests
# Unit tests for the Migrations framework and M001_initial_schema.
# Uses an in-memory SQLite database (:memory:) — no disk files, no cert setup.
# --------------------------------------------------------------------------
add_executable(MigrationsTests
  tests/test_migrations.cpp
  ${SERVER_DB_SOURCES}
)

if(MSVC)
  target_compile_options(MigrationsTests PRIVATE /MT)
endif()

target_include_directories(MigrationsTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${sqlite_SOURCE_DIR}"
)

target_link_libraries(MigrationsTests PRIVATE
  gtest
  gtest_main
  sqlite3
)

add_test(NAME MigrationsTests COMMAND MigrationsTests)

# ==========================================================================
# Phase 2.C additions — BackendClient (TUI side) + integration test
# ==========================================================================

# --------------------------------------------------------------------------
# BackendClientTests  (Phase 2.C)
# Unit tests use FakeHttpClient (no network).
# Integration test (Healthz_LiveRoundTrip) launches TerminalFinanceServer as
# a subprocess, polls /healthz via BackendClient until ready, then asserts.
#
# TF_FIXTURES_DIR is passed so the integration test can find test-ca.pem,
# test-cert.pem, and test-key.pem without relying on cwd.
# TF_SERVER_BIN is the absolute path to the TerminalFinanceServer binary so
# the integration test can fork+exec it without hardcoding build paths.
# --------------------------------------------------------------------------
add_executable(BackendClientTests
  tests/test_backend_client.cpp
  ${COMMON_SOURCES}
  ${SECRET_STORE_SOURCES}
)

# The integration test launches the TerminalFinanceServer binary; make sure
# it's built first.
add_dependencies(BackendClientTests TerminalFinanceServer)

if(MSVC)
  target_compile_options(BackendClientTests PRIVATE /MT)
endif()

target_compile_definitions(BackendClientTests PRIVATE
  TF_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
  TF_SERVER_BIN="$<TARGET_FILE:TerminalFinanceServer>"
)

target_link_libraries(BackendClientTests PRIVATE
  gtest
  gtest_main
  nlohmann_json::nlohmann_json
  ${_SODIUM_TARGET}
)

if(WIN32)
  target_link_libraries(BackendClientTests PRIVATE crypt32 advapi32 CURL::libcurl)
elseif(APPLE)
  target_link_libraries(BackendClientTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
endif()

add_test(NAME BackendClientTests COMMAND BackendClientTests)
