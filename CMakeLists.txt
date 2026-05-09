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

set(gtest_force_static_RUNTIME TRUE)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY_DBG "MultiThreadedDebug")
  set(CMAKE_MSVC_RUNTIME_LIBRARY_REL "MultiThreaded")
endif()

FetchContent_MakeAvailable(ftxui)
FetchContent_MakeAvailable(nlohmann_json)
FetchContent_MakeAvailable(googletest)

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
  src/utils/SyntheticGenerator.cpp
  src/utils/Validator.cpp
  src/utils/ConfigManager.cpp
  ${SECRET_STORE_SOURCES}
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
