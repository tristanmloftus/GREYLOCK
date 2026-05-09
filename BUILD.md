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
  the vcpkg toolchain file, vcpkg will automatically install `libsodium` and `curl`
  from `vcpkg.json` before configuring. No manual `vcpkg install` needed.
- The triplet must be `x64-windows-static` to match the `/MT` runtime used by
  `DataStoreTests`. Using the default dynamic triplet will produce linker errors.
- DPAPI (`crypt32`, `advapi32`) and WinHTTP (`winhttp`) are linked automatically
  via the `if(WIN32)` block in `CMakeLists.txt`. No extra flags needed.
- `HKCU\Software\TerminalFinance\Tokens` is the registry path for stored Plaid
  access tokens. It will be created on first use. To inspect it in regedit, look
  under `HKEY_CURRENT_USER\Software\TerminalFinance\Tokens`.

---

## Dependencies overview

| Dep | Windows source | macOS source | Purpose |
|-----|---------------|-------------|---------|
| ftxui | FetchContent (GitHub) | FetchContent | TUI framework |
| nlohmann/json | FetchContent (GitHub) | FetchContent | JSON |
| googletest | FetchContent (GitHub) | FetchContent | Unit tests |
| libsodium | vcpkg | brew | Crypto (Phase 1+) |
| curl / libcurl | vcpkg | brew | HTTP client (Phase 0.B+) |
| crypt32 / advapi32 / winhttp | Windows SDK | N/A | DPAPI, registry, HTTP |
| Security.framework / CoreFoundation | N/A | Xcode SDK | macOS Keychain |
