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

# --------------------------------------------------------------------------
# tf_widgets — promoted FTXUI widgets (Phase 5).
#
# Pure rendering helpers that take primitive parameters and return an
# ftxui::Element / Component.  Compiled as an OBJECT library so:
#   1. TerminalFinance links the object files directly (no archive overhead).
#   2. WidgetSnapshotTests can link the same object files without pulling in
#      main.cpp or any DataStore/BackendClient state.
# --------------------------------------------------------------------------
set(WIDGET_SOURCES
  src/views/widgets/ui_net_worth.cpp
  src/views/widgets/ui_category_trends.cpp
  src/views/widgets/ui_shovel_intelligence.cpp
  src/views/widgets/ui_shovel_score.cpp
  src/views/widgets/ui_sync_status.cpp
  src/views/widgets/ui_updates.cpp
  src/views/widgets/consolidation_ui.cpp
)
add_library(tf_widgets OBJECT ${WIDGET_SOURCES})
target_link_libraries(tf_widgets PUBLIC ftxui::dom ftxui::screen ftxui::component)
target_include_directories(tf_widgets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

enable_testing()

# --------------------------------------------------------------------------
# Main executable
# --------------------------------------------------------------------------
add_executable(TerminalFinance src/main.cpp src/views/DashboardView.cpp ${COMMON_SOURCES} $<TARGET_OBJECTS:tf_widgets>)

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
else()
  # Linux / other POSIX: only CURL needed; no platform-specific frameworks.
  target_link_libraries(TerminalFinance PRIVATE CURL::libcurl)
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
else()
  # Linux / other POSIX: only CURL needed; no platform-specific frameworks.
  target_link_libraries(DataStoreTests PRIVATE CURL::libcurl)
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
else()
  # Linux / other POSIX: only CURL needed; no platform-specific frameworks.
  target_link_libraries(CurlHttpClientTests PRIVATE CURL::libcurl)
endif()

# Run the non-network tests by default. The network tests skip themselves
# unless TF_NETWORK_TESTS=1 is set in the environment.
add_test(NAME CurlHttpClientTests COMMAND CurlHttpClientTests)

# --------------------------------------------------------------------------
# SecretStoreTests
# Tests for ISecretStore: KeychainSecretStore (macOS) / DpapiSecretStore (Windows).
# Only built on platforms with a real ISecretStore implementation. On Linux
# (CI runner), SECRET_STORE_SOURCES is empty and the test file references the
# concrete platform implementations directly — so this target is skipped.
# --------------------------------------------------------------------------
if(WIN32 OR APPLE)
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
endif()

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
else()
  # Linux / other POSIX: only CURL needed; no platform-specific frameworks.
  target_link_libraries(ServerHealthzTests PRIVATE CURL::libcurl)
endif()

add_test(NAME ServerHealthzTests COMMAND ServerHealthzTests)

# ==========================================================================
# Phase 2.B / Phase 4.E additions — SQLCipher (AES-256 encrypted SQLite)
# ALL additions below this line were added by 2.B and extended by 4.E.
# Do not move them above the existing target definitions to avoid 2.C conflicts.
# ==========================================================================

# --------------------------------------------------------------------------
# SQLCipher — AES-256 encrypted SQLite.  Replaces the plain sqlite-amalgamation
# from Phase 2.B.  SQLCipher is the canonical at-rest encryption solution
# per V0_2_PLAN.md §c (Q1=A locked) and Phase 4 plan §g.
#
# Phase 4.E decision: Path (a) — use the installed SQLCipher library.
# SQLCipher does NOT ship a CMake-ready amalgamation for FetchContent (its
# amalgamation is produced by Autotools at configure time).  Building from
# source via FetchContent would require invoking Autotools — fragile on macOS
# and unsupported on Windows MSVC.  Instead, we follow the same probe
# strategy as libsodium and libcurl:
#
#   1. macOS / Homebrew: probe `brew --prefix sqlcipher`, set SQLCIPHER_ROOT,
#      find_library + find_path, create IMPORTED STATIC target.
#   2. Windows (vcpkg): TODO — sqlcipher vcpkg port available as `sqlcipher`.
#      find_package or find_library via vcpkg toolchain.
#   3. Fallback: FATAL_ERROR with install instructions.
#
# Target name: SQLCipher::sqlcipher (replaces the old sqlite3 target from 2.B).
# Include path: SQLCIPHER_ROOT/include/sqlcipher — SQLCipher installs sqlite3.h
#   under a sqlcipher/ subdirectory to avoid colliding with system SQLite.
#   We set the include path so <sqlite3.h> resolves to the SQLCipher version.
#
# STOP CONDITION: if Homebrew sqlcipher is missing and FetchContent source-
#   build is unavailable, the build FAILS with a message prompting the operator
#   to install sqlcipher via Homebrew.  Do NOT paper over a missing encryption
#   dependency.
# --------------------------------------------------------------------------

if(APPLE AND NOT DEFINED SQLCIPHER_ROOT)
  find_program(_BREW_BIN_SC brew)
  if(_BREW_BIN_SC)
    execute_process(
      COMMAND "${_BREW_BIN_SC}" --prefix sqlcipher
      OUTPUT_VARIABLE _BREW_SQLCIPHER_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _BREW_SQLCIPHER_RESULT
      ERROR_QUIET
    )
    if(_BREW_SQLCIPHER_RESULT EQUAL 0 AND IS_DIRECTORY "${_BREW_SQLCIPHER_PREFIX}")
      set(SQLCIPHER_ROOT "${_BREW_SQLCIPHER_PREFIX}" CACHE PATH
          "Homebrew sqlcipher prefix" FORCE)
      message(STATUS "SQLCipher: using Homebrew sqlcipher at ${SQLCIPHER_ROOT}")
    endif()
  endif()
endif()

set(_SQLCIPHER_FOUND FALSE)

if(WIN32)
  # Windows: sqlcipher is available in vcpkg.
  # Install with: vcpkg install sqlcipher:x64-windows-static
  # Then pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
  find_library(_SQLCIPHER_LIB
    NAMES sqlcipher
    REQUIRED
  )
  # vcpkg installs the sqlcipher header at <vcpkg-installed>/include/sqlcipher/sqlite3.h.
  # We need the include directory to BE that sqlcipher/ subdir so that
  # `#include <sqlite3.h>` resolves to the SQLCipher build (not plain SQLite).
  # PATH_SUFFIXES tells find_path to look in <base>/sqlcipher/ and to RETURN
  # that subdir as the result (mirroring the macOS Homebrew layout where
  # `<prefix>/include/sqlcipher/sqlite3.h` is the canonical path).
  find_path(_SQLCIPHER_INCLUDE
    NAMES sqlite3.h
    PATH_SUFFIXES sqlcipher
    REQUIRED
  )
  if(_SQLCIPHER_LIB AND _SQLCIPHER_INCLUDE)
    add_library(SQLCipher::sqlcipher STATIC IMPORTED GLOBAL)
    set_target_properties(SQLCipher::sqlcipher PROPERTIES
      IMPORTED_LOCATION "${_SQLCIPHER_LIB}"
      INTERFACE_INCLUDE_DIRECTORIES "${_SQLCIPHER_INCLUDE}"
    )
    set(_SQLCIPHER_FOUND TRUE)
    message(STATUS "SQLCipher: Windows — library at ${_SQLCIPHER_LIB}, includes at ${_SQLCIPHER_INCLUDE}")
  endif()
endif()

if(NOT _SQLCIPHER_FOUND)
  # macOS / Linux: search in SQLCIPHER_ROOT (Homebrew prefix) then system paths.
  find_library(_SQLCIPHER_LIB
    NAMES sqlcipher
    HINTS "${SQLCIPHER_ROOT}/lib"
    REQUIRED
  )
  # SQLCipher installs headers under include/sqlcipher/ — use that as the
  # include root so that #include <sqlite3.h> resolves to the SQLCipher build
  # (not the system plain SQLite).
  # PATH_SUFFIXES sqlcipher covers Debian's libsqlcipher-dev which installs to
  # /usr/include/sqlcipher/sqlite3.h without SQLCIPHER_ROOT being set.
  find_path(_SQLCIPHER_INCLUDE_DIR
    NAMES sqlite3.h
    HINTS "${SQLCIPHER_ROOT}/include/sqlcipher"
    PATH_SUFFIXES sqlcipher
    REQUIRED
  )
  if(_SQLCIPHER_LIB AND _SQLCIPHER_INCLUDE_DIR)
    add_library(SQLCipher::sqlcipher STATIC IMPORTED GLOBAL)
    set_target_properties(SQLCipher::sqlcipher PROPERTIES
      IMPORTED_LOCATION "${_SQLCIPHER_LIB}"
      INTERFACE_INCLUDE_DIRECTORIES "${_SQLCIPHER_INCLUDE_DIR}"
    )
    set(_SQLCIPHER_FOUND TRUE)
    message(STATUS "SQLCipher: library at ${_SQLCIPHER_LIB}, "
                   "includes at ${_SQLCIPHER_INCLUDE_DIR}")
  endif()
endif()

if(NOT _SQLCIPHER_FOUND)
  if(APPLE)
    message(FATAL_ERROR
      "SQLCipher was not found on this macOS system.\n"
      "Install via Homebrew: brew install sqlcipher\n"
      "Then re-run cmake.")
  elseif(WIN32)
    message(FATAL_ERROR
      "SQLCipher was not found. On Windows, install via vcpkg:\n"
      "  vcpkg install sqlcipher:x64-windows-static\n"
      "Then pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake to cmake.")
  else()
    message(FATAL_ERROR
      "SQLCipher was not found. Install via your system package manager "
      "(e.g. apt install libsqlcipher-dev) then re-run cmake.")
  endif()
endif()

# SQLCipher requires OpenSSL::Crypto for its AES implementation.
# OpenSSL is already a required dependency (found earlier in this file).
# Link it into SQLCipher's IMPORTED target so all consumers inherit it.
set_property(TARGET SQLCipher::sqlcipher APPEND PROPERTY
  INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
)

# --------------------------------------------------------------------------
# Server DB sources (Database + Migrations).
# These are added to TerminalFinanceServer via target_sources() so we do NOT
# need to modify the existing add_executable() line — minimizing 2.C conflict.
# [2.B EXTENSION: TerminalFinanceServer source list extended here]
# [4.E: sqlite3 target → SQLCipher::sqlcipher]
# --------------------------------------------------------------------------
set(SERVER_DB_SOURCES
  server/db/Database.cpp
  server/db/Migrations.cpp
)

target_sources(TerminalFinanceServer PRIVATE ${SERVER_DB_SOURCES})

# No additional include_directories needed — SQLCipher::sqlcipher's
# INTERFACE_INCLUDE_DIRECTORIES propagates the include path transitively.

target_link_libraries(TerminalFinanceServer PRIVATE SQLCipher::sqlcipher)

# --------------------------------------------------------------------------
# MigrationsTests
# Unit tests for the Migrations framework and M001_initial_schema.
# Uses an in-memory SQLCipher database (:memory:) — no disk files, no cert setup.
# [4.E: sqlite3 target → SQLCipher::sqlcipher; passes kTestKey in test code]
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
)

target_link_libraries(MigrationsTests PRIVATE
  gtest
  gtest_main
  ${_SODIUM_TARGET}
  SQLCipher::sqlcipher
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
else()
  # Linux / other POSIX: only CURL needed; no platform-specific frameworks.
  target_link_libraries(BackendClientTests PRIVATE CURL::libcurl)
endif()

add_test(NAME BackendClientTests COMMAND BackendClientTests)

# ==========================================================================
# Phase 3.B additions — AuthService (TUI side) + AuthServiceTests
# ==========================================================================
target_sources(TerminalFinance PRIVATE src/services/AuthService.cpp)

add_executable(AuthServiceTests
  tests/test_auth_service.cpp
  ${COMMON_SOURCES}
  src/services/AuthService.cpp
)

if(MSVC)
  target_compile_options(AuthServiceTests PRIVATE /MT)
endif()

target_link_libraries(AuthServiceTests PRIVATE
  gtest
  gtest_main
  nlohmann_json::nlohmann_json
  ${_SODIUM_TARGET}
)

if(WIN32)
  target_link_libraries(AuthServiceTests PRIVATE crypt32 advapi32 CURL::libcurl)
elseif(APPLE)
  target_link_libraries(AuthServiceTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
else()
  # Linux / other POSIX: only CURL needed; no platform-specific frameworks.
  target_link_libraries(AuthServiceTests PRIVATE CURL::libcurl)
endif()

add_test(NAME AuthServiceTests COMMAND AuthServiceTests)

# ==========================================================================
# Phase 3.A additions — server-side auth subsystem
# ==========================================================================
set(SERVER_AUTH_SOURCES
  server/auth/PassphraseHash.cpp
  server/auth/Totp.cpp
  server/auth/EnrollmentToken.cpp
  server/auth/Session.cpp
  server/auth/AuthHandlers.cpp
)

set(SERVER_AUDIT_SOURCES
  server/audit/StubAuditLog.cpp
)

set(SERVER_AUDIT_REAL_SOURCES
  server/audit/CanonicalBytes.cpp
  server/audit/Sanitizer.cpp
  server/audit/SqlAuditLog.cpp
)

target_sources(TerminalFinanceServer PRIVATE
  ${SERVER_AUTH_SOURCES}
  ${SERVER_AUDIT_SOURCES}
  ${SERVER_AUDIT_REAL_SOURCES}
)

target_link_libraries(TerminalFinanceServer PRIVATE
  ${_SODIUM_TARGET}
)

# --------------------------------------------------------------------------
# AuthPassphraseTests
# --------------------------------------------------------------------------
add_executable(AuthPassphraseTests
  tests/test_auth_passphrase.cpp
  server/auth/PassphraseHash.cpp
)

if(MSVC)
  target_compile_options(AuthPassphraseTests PRIVATE /MT)
endif()

target_include_directories(AuthPassphraseTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(AuthPassphraseTests PRIVATE
  gtest gtest_main ${_SODIUM_TARGET}
)

add_test(NAME AuthPassphraseTests COMMAND AuthPassphraseTests)

# --------------------------------------------------------------------------
# AuthTotpTests
# --------------------------------------------------------------------------
add_executable(AuthTotpTests
  tests/test_auth_totp.cpp
  server/auth/Totp.cpp
)

if(MSVC)
  target_compile_options(AuthTotpTests PRIVATE /MT)
endif()

target_include_directories(AuthTotpTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(AuthTotpTests PRIVATE
  gtest gtest_main ${_SODIUM_TARGET}
)

add_test(NAME AuthTotpTests COMMAND AuthTotpTests)

# --------------------------------------------------------------------------
# AuthEnrollmentTokenTests
# --------------------------------------------------------------------------
add_executable(AuthEnrollmentTokenTests
  tests/test_auth_enrollment_token.cpp
  server/auth/EnrollmentToken.cpp
  ${SERVER_DB_SOURCES}
)

if(MSVC)
  target_compile_options(AuthEnrollmentTokenTests PRIVATE /MT)
endif()

target_include_directories(AuthEnrollmentTokenTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(AuthEnrollmentTokenTests PRIVATE
  gtest gtest_main ${_SODIUM_TARGET} SQLCipher::sqlcipher
)

add_test(NAME AuthEnrollmentTokenTests COMMAND AuthEnrollmentTokenTests)

# --------------------------------------------------------------------------
# AuthSessionTests
# --------------------------------------------------------------------------
add_executable(AuthSessionTests
  tests/test_auth_session.cpp
  server/auth/Session.cpp
  ${SERVER_DB_SOURCES}
)

if(MSVC)
  target_compile_options(AuthSessionTests PRIVATE /MT)
endif()

target_include_directories(AuthSessionTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(AuthSessionTests PRIVATE
  gtest gtest_main ${_SODIUM_TARGET} SQLCipher::sqlcipher
)

add_test(NAME AuthSessionTests COMMAND AuthSessionTests)

# --------------------------------------------------------------------------
# AuthRateLimitTests — unit tests for the in-process rate-limit map
# (bounded growth + window expiry).  Pulls in AuthHandlers.cpp via its
# internal namespace surface so no HTTP plumbing is needed.
# --------------------------------------------------------------------------
add_executable(AuthRateLimitTests
  tests/test_auth_rate_limit.cpp
  server/auth/AuthHandlers.cpp
  server/auth/EnrollmentToken.cpp
  server/auth/PassphraseHash.cpp
  server/auth/Session.cpp
  server/auth/Totp.cpp
  ${SERVER_DB_SOURCES}
  ${SERVER_AUDIT_REAL_SOURCES}
)

if(MSVC)
  target_compile_options(AuthRateLimitTests PRIVATE /MT)
endif()

# AuthHandlers.cpp includes httplib.h; httplib.h requires
# CPPHTTPLIB_OPENSSL_SUPPORT when the project links against OpenSSL.
target_compile_definitions(AuthRateLimitTests PRIVATE
  CPPHTTPLIB_OPENSSL_SUPPORT=1
)

target_include_directories(AuthRateLimitTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${cpp-httplib_SOURCE_DIR}"
)

target_link_libraries(AuthRateLimitTests PRIVATE
  gtest gtest_main ${_SODIUM_TARGET} SQLCipher::sqlcipher
  nlohmann_json::nlohmann_json
  OpenSSL::SSL OpenSSL::Crypto
)

add_test(NAME AuthRateLimitTests COMMAND AuthRateLimitTests)

# --------------------------------------------------------------------------
# AuthEndpointsTests — integration: launches TerminalFinanceServer.
# --------------------------------------------------------------------------
add_executable(AuthEndpointsTests
  tests/test_auth_endpoints.cpp
  src/services/http/CurlHttpClient.cpp
)

add_dependencies(AuthEndpointsTests TerminalFinanceServer)

if(MSVC)
  target_compile_options(AuthEndpointsTests PRIVATE /MT)
endif()

target_compile_definitions(AuthEndpointsTests PRIVATE
  TF_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
  TF_SERVER_BIN="$<TARGET_FILE:TerminalFinanceServer>"
)

target_include_directories(AuthEndpointsTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(AuthEndpointsTests PRIVATE
  gtest gtest_main ${_SODIUM_TARGET} nlohmann_json::nlohmann_json
)

if(WIN32)
  target_link_libraries(AuthEndpointsTests PRIVATE crypt32 advapi32 CURL::libcurl)
elseif(APPLE)
  target_link_libraries(AuthEndpointsTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
else()
  # Linux / other POSIX: only CURL needed; no platform-specific frameworks.
  target_link_libraries(AuthEndpointsTests PRIVATE CURL::libcurl)
endif()

add_test(NAME AuthEndpointsTests COMMAND AuthEndpointsTests)

# ==========================================================================
# Phase 4.A additions — BLAKE2b-chained audit log writer + tests
# ALL additions below this line were added by 4.A. Do not move above existing.
# ==========================================================================

# Shared audit real-implementation sources for test targets.
set(AUDIT_IMPL_SOURCES
  server/audit/CanonicalBytes.cpp
  server/audit/Sanitizer.cpp
  server/audit/SqlAuditLog.cpp
)

# --------------------------------------------------------------------------
# AuditCanonicalBytesTests
# Regression gate for AUDIT.md F-4 (no double-counting of tsUnixNanos).
# Only depends on CanonicalBytes.cpp (no sqlite3 needed).
# --------------------------------------------------------------------------
add_executable(AuditCanonicalBytesTests
  tests/test_audit_chain_canonical_bytes_no_double_count.cpp
  server/audit/CanonicalBytes.cpp
)

if(MSVC)
  target_compile_options(AuditCanonicalBytesTests PRIVATE /MT)
endif()

target_include_directories(AuditCanonicalBytesTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(AuditCanonicalBytesTests PRIVATE
  gtest gtest_main ${_SODIUM_TARGET} nlohmann_json::nlohmann_json
)

add_test(NAME AuditCanonicalBytesTests COMMAND AuditCanonicalBytesTests)

# --------------------------------------------------------------------------
# AuditReplayTests
# Replay-verifier ships FIRST (verifier-before-writer discipline).
# Uses hand-rolled chains via raw SQL; does not depend on SqlAuditLog.
# --------------------------------------------------------------------------
add_executable(AuditReplayTests
  tests/test_audit_replay.cpp
  server/audit/CanonicalBytes.cpp
  ${SERVER_DB_SOURCES}
)

if(MSVC)
  target_compile_options(AuditReplayTests PRIVATE /MT)
endif()

target_include_directories(AuditReplayTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(AuditReplayTests PRIVATE
  gtest gtest_main ${_SODIUM_TARGET} SQLCipher::sqlcipher nlohmann_json::nlohmann_json
)

add_test(NAME AuditReplayTests COMMAND AuditReplayTests)

# --------------------------------------------------------------------------
# AuditSanitizerTests
# Unit tests for the sanitizer (no DB, no sodium).
# --------------------------------------------------------------------------
add_executable(AuditSanitizerTests
  tests/test_audit_sanitizer.cpp
  server/audit/Sanitizer.cpp
)

if(MSVC)
  target_compile_options(AuditSanitizerTests PRIVATE /MT)
endif()

target_include_directories(AuditSanitizerTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(AuditSanitizerTests PRIVATE
  gtest gtest_main nlohmann_json::nlohmann_json
)

add_test(NAME AuditSanitizerTests COMMAND AuditSanitizerTests)

# --------------------------------------------------------------------------
# AuditWriterTests
# Integration test: records events via SqlAuditLog; runs verify_chain.
# --------------------------------------------------------------------------
add_executable(AuditWriterTests
  tests/test_audit_writer.cpp
  ${AUDIT_IMPL_SOURCES}
  ${SERVER_DB_SOURCES}
)

if(MSVC)
  target_compile_options(AuditWriterTests PRIVATE /MT)
endif()

target_include_directories(AuditWriterTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(AuditWriterTests PRIVATE
  gtest gtest_main ${_SODIUM_TARGET} SQLCipher::sqlcipher nlohmann_json::nlohmann_json
)

add_test(NAME AuditWriterTests COMMAND AuditWriterTests)

# ==========================================================================
# Phase 4.E additions — SqlCipherTests
# Unit tests for the SQLCipher integration: key application, encryption
# round-trip, mismatch detection, and key-format validation.
# ==========================================================================

# --------------------------------------------------------------------------
# SqlCipherTests
# Tests the Database class's SQLCipher key plumbing:
#   - cipher_version PRAGMA is reachable (SQLCipher, not plain SQLite)
#   - wrong key → throw on first query
#   - correct key → row readable after reopen
#   - no key on encrypted file → throw on first prepare
#   - invalid key format (non-hex, wrong length) → throw at construction
# --------------------------------------------------------------------------
add_executable(SqlCipherTests
  tests/test_sqlcipher.cpp
  ${SERVER_DB_SOURCES}
)

if(MSVC)
  target_compile_options(SqlCipherTests PRIVATE /MT)
endif()

target_include_directories(SqlCipherTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(SqlCipherTests PRIVATE
  gtest
  gtest_main
  ${_SODIUM_TARGET}
  SQLCipher::sqlcipher
)

add_test(NAME SqlCipherTests COMMAND SqlCipherTests)

# ==========================================================================
# Phase 4.B additions — data CRUD endpoints + RemoteBackendStorageService
# ==========================================================================

# --------------------------------------------------------------------------
# Server-side data handler sources: session middleware + 5 CRUD handlers +
# entity-membership authorization helper.
# Added via target_sources() to avoid modifying the existing add_executable().
# --------------------------------------------------------------------------
set(SERVER_DATA_SOURCES
  server/auth/SessionMiddleware.cpp
  server/data/EntityMembership.cpp
  server/data/EntitiesHandler.cpp
  server/data/AccountsHandler.cpp
  server/data/TransactionsHandler.cpp
  server/data/CategoriesHandler.cpp
  server/data/BudgetsHandler.cpp
)

target_sources(TerminalFinanceServer PRIVATE ${SERVER_DATA_SOURCES})

# Phase 5: GET /supplier-map endpoint (session-gated).
# Lives in its own source group to keep the data/ handler set focused on
# CRUD endpoints.  Reuses SessionMiddleware (already in SERVER_DATA_SOURCES).
set(SERVER_DISCOVERY_SOURCES
  server/discovery/SupplierMapHandler.cpp
)
target_sources(TerminalFinanceServer PRIVATE ${SERVER_DISCOVERY_SOURCES})

# --------------------------------------------------------------------------
# TUI-side: RemoteBackendStorageService (IStorageService backed by server API).
# save() is a documented stub returning false (write-through deferred to Phase 5).
# --------------------------------------------------------------------------
target_sources(TerminalFinance PRIVATE src/services/RemoteBackendStorageService.cpp)

# --------------------------------------------------------------------------
# DataEndpointsTests — integration: launches TerminalFinanceServer on port 19845.
# Exercises entity/account/transaction/category/budget CRUD + auth guardrails.
# --------------------------------------------------------------------------
add_executable(DataEndpointsTests
  tests/test_data_endpoints.cpp
  src/services/http/CurlHttpClient.cpp
)

add_dependencies(DataEndpointsTests TerminalFinanceServer)

if(MSVC)
  target_compile_options(DataEndpointsTests PRIVATE /MT)
endif()

target_compile_definitions(DataEndpointsTests PRIVATE
  TF_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
  TF_SERVER_BIN="$<TARGET_FILE:TerminalFinanceServer>"
)

target_include_directories(DataEndpointsTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(DataEndpointsTests PRIVATE
  gtest gtest_main ${_SODIUM_TARGET} nlohmann_json::nlohmann_json
)

if(WIN32)
  target_link_libraries(DataEndpointsTests PRIVATE crypt32 advapi32 CURL::libcurl)
elseif(APPLE)
  target_link_libraries(DataEndpointsTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
else()
  # Linux / other POSIX: only CURL needed; no platform-specific frameworks.
  target_link_libraries(DataEndpointsTests PRIVATE CURL::libcurl)
endif()

add_test(NAME DataEndpointsTests COMMAND DataEndpointsTests)

# --------------------------------------------------------------------------
# RemoteBackendStorageServiceTests — unit tests using FakeHttpClient.
# No network, no server subprocess.
# --------------------------------------------------------------------------
add_executable(RemoteBackendStorageServiceTests
  tests/test_remote_backend_storage_service.cpp
  ${COMMON_SOURCES}
  src/services/RemoteBackendStorageService.cpp
)

if(MSVC)
  target_compile_options(RemoteBackendStorageServiceTests PRIVATE /MT)
endif()

target_link_libraries(RemoteBackendStorageServiceTests PRIVATE
  gtest gtest_main nlohmann_json::nlohmann_json ${_SODIUM_TARGET}
)

if(WIN32)
  target_link_libraries(RemoteBackendStorageServiceTests PRIVATE crypt32 advapi32 CURL::libcurl)
elseif(APPLE)
  target_link_libraries(RemoteBackendStorageServiceTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
else()
  # Linux / other POSIX: only CURL needed; no platform-specific frameworks.
  target_link_libraries(RemoteBackendStorageServiceTests PRIVATE CURL::libcurl)
endif()

add_test(NAME RemoteBackendStorageServiceTests COMMAND RemoteBackendStorageServiceTests)

# ==========================================================================
# Phase 4.C additions — PlaidTokenBroker (server-side) + PlaidService refactor
# ALL additions below this line were added by 4.C. Do not move above existing.
# ==========================================================================

# --------------------------------------------------------------------------
# Wire PlaidTokenBroker into TerminalFinanceServer.
# PlaidTokenBroker.cpp depends on: Database.h/cpp (SERVER_DB_SOURCES),
# EnvelopeEncryption.cpp (CRYPTO_SOURCES), Zeroize.h, libsodium, sqlcipher.
# --------------------------------------------------------------------------
target_sources(TerminalFinanceServer PRIVATE
  server/plaid/PlaidTokenBroker.cpp
  src/services/crypto/EnvelopeEncryption.cpp
)

# --------------------------------------------------------------------------
# PlaidTokenBrokerTests
# Unit tests for PlaidTokenBroker: round-trip, AAD-binding, clear, no-token,
# multi-account isolation, two-callable variant, overwrite.
# Uses an in-memory SQLCipher database; no network, no TF_MASTER_KEY env var.
# --------------------------------------------------------------------------
add_executable(PlaidTokenBrokerTests
  tests/test_plaid_token_broker.cpp
  server/plaid/PlaidTokenBroker.cpp
  src/services/crypto/EnvelopeEncryption.cpp
  ${SERVER_DB_SOURCES}
)

if(MSVC)
  target_compile_options(PlaidTokenBrokerTests PRIVATE /MT)
endif()

target_include_directories(PlaidTokenBrokerTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(PlaidTokenBrokerTests PRIVATE
  gtest gtest_main ${_SODIUM_TARGET} SQLCipher::sqlcipher
)

add_test(NAME PlaidTokenBrokerTests COMMAND PlaidTokenBrokerTests)

# --------------------------------------------------------------------------
# PlaidServiceRefactorTests
# Compile-time and runtime checks that:
#   - Account has no plaid_access_token field.
#   - Account has is_plaid_linked field.
#   - IPlaidService public API is account_id-based (no raw access_token).
#   - StubPlaidService satisfies the new interface.
# --------------------------------------------------------------------------
add_executable(PlaidServiceRefactorTests
  tests/test_plaid_service_refactor.cpp
  src/services/PlaidService.cpp
  src/utils/Logger.h
)

if(MSVC)
  target_compile_options(PlaidServiceRefactorTests PRIVATE /MT)
endif()

target_include_directories(PlaidServiceRefactorTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(PlaidServiceRefactorTests PRIVATE
  gtest gtest_main
)

add_test(NAME PlaidServiceRefactorTests COMMAND PlaidServiceRefactorTests)

# ==========================================================================
# Phase 4.D additions — PlaidApiClient + PlaidSyncScheduler
# ALL additions below this line were added by 4.D. Do not move above existing.
# ==========================================================================

# --------------------------------------------------------------------------
# Wire PlaidApiClient and PlaidSyncScheduler into TerminalFinanceServer.
# Also adds CurlHttpClient (needed by PlaidApiClient via main.cpp).
# --------------------------------------------------------------------------
target_sources(TerminalFinanceServer PRIVATE
  server/plaid/PlaidApiClient.cpp
  server/plaid/PlaidSyncScheduler.cpp
  src/services/http/CurlHttpClient.cpp
)

target_link_libraries(TerminalFinanceServer PRIVATE
  CURL::libcurl
)

# --------------------------------------------------------------------------
# PlaidSyncSchedulerTests
# Unit tests for PlaidSyncScheduler + PlaidApiClient.
# Uses MockPlaidApiClient (no real Plaid API calls) and StubAuditLog.
# Uses an in-memory SQLCipher database.
# --------------------------------------------------------------------------
add_executable(PlaidSyncSchedulerTests
  tests/test_plaid_sync_scheduler.cpp
  server/plaid/PlaidApiClient.cpp
  server/plaid/PlaidSyncScheduler.cpp
  server/plaid/PlaidTokenBroker.cpp
  server/audit/StubAuditLog.cpp
  server/audit/CanonicalBytes.cpp
  server/audit/Sanitizer.cpp
  server/audit/SqlAuditLog.cpp
  src/services/crypto/EnvelopeEncryption.cpp
  src/services/http/CurlHttpClient.cpp
  ${SERVER_DB_SOURCES}
)

if(MSVC)
  target_compile_options(PlaidSyncSchedulerTests PRIVATE /MT)
endif()

target_include_directories(PlaidSyncSchedulerTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(PlaidSyncSchedulerTests PRIVATE
  gtest
  gtest_main
  ${_SODIUM_TARGET}
  SQLCipher::sqlcipher
  nlohmann_json::nlohmann_json
)

if(WIN32)
  target_link_libraries(PlaidSyncSchedulerTests PRIVATE crypt32 advapi32 CURL::libcurl)
elseif(APPLE)
  target_link_libraries(PlaidSyncSchedulerTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
else()
  # Linux / other POSIX: only CURL needed; no platform-specific frameworks.
  target_link_libraries(PlaidSyncSchedulerTests PRIVATE CURL::libcurl)
endif()

add_test(NAME PlaidSyncSchedulerTests COMMAND PlaidSyncSchedulerTests)

# ==========================================================================
# Phase 4.F additions — V01Migrator (v0.1 → server migration tool)
# ALL additions below this line were added by 4.F. Do not move above existing.
# ==========================================================================

# --------------------------------------------------------------------------
# Wire V01Migrator into the main TerminalFinance binary.
# BackendClientAdapter (in V01Migrator.cpp) depends on BackendClient which is
# already in COMMON_SOURCES.
# --------------------------------------------------------------------------
target_sources(TerminalFinance PRIVATE src/migration/V01Migrator.cpp)

# --------------------------------------------------------------------------
# V01MigratorTests
# Unit tests using FakeBackendClient — no network required.
# 7 test cases covering empty JSON, create, 409 skip, 404 error, large
# batches, plaid_access_token omission, and multi-error recording.
# --------------------------------------------------------------------------
add_executable(V01MigratorTests
  tests/test_v01_migrator.cpp
  src/migration/V01Migrator.cpp
  src/services/BackendClient.cpp
  src/services/http/CurlHttpClient.cpp
)

if(MSVC)
  target_compile_options(V01MigratorTests PRIVATE /MT)
endif()

target_link_libraries(V01MigratorTests PRIVATE
  gtest
  gtest_main
  nlohmann_json::nlohmann_json
  ${_SODIUM_TARGET}
)

if(WIN32)
  target_link_libraries(V01MigratorTests PRIVATE crypt32 advapi32 CURL::libcurl)
elseif(APPLE)
  target_link_libraries(V01MigratorTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
else()
  # Linux / other POSIX: only CURL needed; no platform-specific frameworks.
  target_link_libraries(V01MigratorTests PRIVATE CURL::libcurl)
endif()

add_test(NAME V01MigratorTests COMMAND V01MigratorTests)

# ==========================================================================
# Phase 5 additions — DiscoveryService JSON loader + /supplier-map endpoint
# ==========================================================================

# --------------------------------------------------------------------------
# DiscoveryMerchantVelocityTests
# Unit tests for DiscoveryService against realistic merchant strings
# (StarbucksUS123, AMZN MKTPLACE * SUBSCRIBE, COSTCO WHSE #0123, USPS PO 12345
# etc.) plus deterministic MoM velocity assertions.
# --------------------------------------------------------------------------
add_executable(DiscoveryMerchantVelocityTests
  tests/test_discovery_merchant_velocity.cpp
  src/services/DiscoveryService.cpp
)

if(MSVC)
  target_compile_options(DiscoveryMerchantVelocityTests PRIVATE /MT)
endif()

target_include_directories(DiscoveryMerchantVelocityTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

# TF_SUPPLIER_MAP_FIXTURE points to the canonical JSON committed at
# data/supplier_map.json so the test can load it from any CWD ctest picks.
target_compile_definitions(DiscoveryMerchantVelocityTests PRIVATE
  TF_SUPPLIER_MAP_FIXTURE="${CMAKE_SOURCE_DIR}/data/supplier_map.json"
)

target_link_libraries(DiscoveryMerchantVelocityTests PRIVATE
  gtest
  gtest_main
  nlohmann_json::nlohmann_json
)

add_test(NAME DiscoveryMerchantVelocityTests COMMAND DiscoveryMerchantVelocityTests)

# --------------------------------------------------------------------------
# SupplierMapEndpointTests
# Integration test for GET /supplier-map. Boots TerminalFinanceServer on
# port 19846 (distinct from data-endpoint tests' 19845), enrolls a user,
# logs in, and exercises the auth/unauth/malformed paths.
# --------------------------------------------------------------------------
add_executable(SupplierMapEndpointTests
  tests/test_supplier_map_endpoint.cpp
  src/services/http/CurlHttpClient.cpp
)

add_dependencies(SupplierMapEndpointTests TerminalFinanceServer)

if(MSVC)
  target_compile_options(SupplierMapEndpointTests PRIVATE /MT)
endif()

target_compile_definitions(SupplierMapEndpointTests PRIVATE
  TF_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
  TF_SERVER_BIN="$<TARGET_FILE:TerminalFinanceServer>"
  TF_SUPPLIER_MAP_FIXTURE="${CMAKE_SOURCE_DIR}/data/supplier_map.json"
)

target_include_directories(SupplierMapEndpointTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(SupplierMapEndpointTests PRIVATE
  gtest
  gtest_main
  nlohmann_json::nlohmann_json
  ${_SODIUM_TARGET}
)

if(WIN32)
  target_link_libraries(SupplierMapEndpointTests PRIVATE crypt32 advapi32 CURL::libcurl)
elseif(APPLE)
  target_link_libraries(SupplierMapEndpointTests PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
    CURL::libcurl
  )
else()
  target_link_libraries(SupplierMapEndpointTests PRIVATE CURL::libcurl)
endif()

add_test(NAME SupplierMapEndpointTests COMMAND SupplierMapEndpointTests)

# --------------------------------------------------------------------------
# WidgetSnapshotTests (Phase 5 TUI snapshot harness)
# Renders each FTXUI widget from src/views/widgets/ at 80x24 and compares
# to a checked-in fixture under tests/snapshot/fixtures/.  Set
# TF_UPDATE_SNAPSHOTS=1 to capture/refresh fixtures.  Links against the
# tf_widgets OBJECT library only — no DataStore / BackendClient deps.
# --------------------------------------------------------------------------
add_executable(WidgetSnapshotTests
  tests/test_widgets_snapshot.cpp
  $<TARGET_OBJECTS:tf_widgets>
)
target_link_libraries(WidgetSnapshotTests PRIVATE
  gtest
  gtest_main
  ftxui::dom
  ftxui::screen
  ftxui::component
)
# Run with CWD = source dir so the default fixture path resolves.
add_test(
  NAME WidgetSnapshotTests
  COMMAND WidgetSnapshotTests
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# --------------------------------------------------------------------------
# ConsolidationServiceTests (Phase 5)
# Locks in: cross-account transfer dedup behavior, same-account duplicate
# dedup, idempotency (f(f(x)) == f(x)), hash-stability across runs.
# --------------------------------------------------------------------------
add_executable(ConsolidationServiceTests
  tests/test_consolidation_service.cpp
  src/services/ConsolidationService.cpp
  src/services/PlaidService.cpp
)
if(MSVC)
  target_compile_options(ConsolidationServiceTests PRIVATE /MT)
endif()
target_include_directories(ConsolidationServiceTests PRIVATE
  "${CMAKE_SOURCE_DIR}"
)
target_link_libraries(ConsolidationServiceTests PRIVATE
  gtest
  gtest_main
  nlohmann_json::nlohmann_json
)
if(WIN32)
  target_link_libraries(ConsolidationServiceTests PRIVATE crypt32 winhttp advapi32)
endif()
add_test(NAME ConsolidationServiceTests COMMAND ConsolidationServiceTests)
