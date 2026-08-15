---
name: fix-and-build
description: Build overdare-lsp locally (CLI and/or tests) and verify the result. Use when the user says things like "빌드해줘", "build it", or asks to compile/rebuild and confirm it works.
---

# Build (overdare-lsp)

Workflow for building this repo locally and verifying the build. Steps 1-5 auto-adapt to
whatever OS/arch you're actually running on (macOS, Linux, Windows) - always detect first,
don't assume the machine matches a prior session.

## 0. Detect platform first

```bash
# macOS/Linux (bash)
uname -s   # Darwin | Linux
uname -m   # arm64/x86_64 (macOS) | x86_64/aarch64 (Linux)
```
```powershell
# Windows (PowerShell)
$env:PROCESSOR_ARCHITECTURE   # AMD64 | ARM64
```

This decides three things for the rest of the workflow: the CMake generator's output layout
(single-config vs multi-config), the built binary's filename, and (if packaging a vsix) the
`vsce --target` value. Reference table:

| Platform | `uname -s`/arch | CMake output binary | `vsce --target` |
|---|---|---|---|
| macOS Apple Silicon | Darwin / arm64 | `build/luau-lsp` | `darwin-arm64` |
| macOS Intel | Darwin / x86_64 | `build/luau-lsp` | `darwin-x64` |
| Linux x64 | Linux / x86_64 | `build/luau-lsp` | `linux-x64` |
| Linux arm64 | Linux / aarch64 | `build/luau-lsp` | `linux-arm64` |
| Windows x64 | AMD64 | `build\<Config>\luau-lsp.exe` | `win32-x64` |
| Windows arm64 | ARM64 | `build\<Config>\luau-lsp.exe` | `win32-arm64` |

macOS/Linux use a single-config generator (Ninja/Makefiles) by default, so the binary lands
directly at `build/luau-lsp` regardless of `--config`. Windows defaults to the Visual Studio
generator (multi-config), so the binary lands in a `<Config>`-named subfolder
(`build/Debug/luau-lsp.exe` or `build/RelWithDebInfo/luau-lsp.exe`) matching whatever
`--config` was passed to the build command - always confirm the actual path with a directory
listing rather than assuming, since it depends on which `--config` was used.

## Steps

1. **Check submodules** (only if they look stale, e.g. after a fresh clone or pull)
   ```bash
   git submodule update --init --recursive
   ```

2. **Configure** (only if `build/` doesn't exist yet or CMake cache is missing)
   ```bash
   mkdir -p build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Debug   # Debug is faster to iterate with; omit on Windows (see below)
   ```
   On Windows, `CMAKE_BUILD_TYPE` is ignored by the default multi-config (Visual Studio)
   generator - the config is chosen at build time via `--config` instead (step 3).

3. **Build the CLI**
   ```bash
   # macOS
   NUM_CPUS=$(sysctl -n hw.ncpu)
   # Linux
   NUM_CPUS=$(nproc)
   cmake --build . --target Luau.LanguageServer.CLI --config Debug -j$NUM_CPUS
   ```
   ```powershell
   # Windows
   cmake --build . --target Luau.LanguageServer.CLI --config RelWithDebInfo
   ```
   If nothing changed since the last build, the tool reports "no work to do" (Ninja) or
   skips the target (MSBuild) - that's expected, not a failure. Locate the actual output
   binary per the table in step 0 before referencing it in later steps.

4. **Build tests** (only when the user also wants tests, or the change touches core LSP logic)
   ```bash
   cmake --build . --target Luau.LanguageServer.Test --config Debug -j$NUM_CPUS   # macOS/Linux
   ```
   ```powershell
   cmake --build . --target Luau.LanguageServer.Test --config RelWithDebInfo   # Windows
   ```

5. **Verify**
   - Quick sanity check without a full LSP session:
     ```bash
     ./build/luau-lsp analyze --definitions=scripts/globalTypes.d.luau path/to/file.luau
     ```
     ```powershell
     .\build\RelWithDebInfo\luau-lsp.exe analyze --definitions=scripts/globalTypes.d.luau path/to/file.luau
     ```
   - Run tests (must run from repo root — they read `tests/testdata/` via relative paths):
     ```bash
     ./build/Luau.LanguageServer.Test --test-case="RelevantTestName"
     ```

6. **Report**
   - State whether the build succeeded, and surface any compiler errors or test failures verbatim.

## Optional: package a platform-specific vsix (self-contained, no manual server path)

Only do this when the user actually wants a vsix to install/share, not for routine
build-and-verify. Bundles the just-built binary into the extension so it works with zero
config, matching `.github/workflows/release.yml`'s CI pattern (which does the same thing per
OS in a build matrix, then publishes each with its own `--target`).

```bash
# macOS/Linux — from repo root, after step 3
mkdir -p editors/code/bin
cp build/luau-lsp editors/code/bin/server
chmod 777 editors/code/bin/server
cp README.md editors/code/README.md
cp CHANGELOG.md editors/code/CHANGELOG.md
cd editors/code
npm install    # first time only
npx @vscode/vsce package --target <darwin-arm64|darwin-x64|linux-x64|linux-arm64> \
  --out /tmp/overdare-lsp-<platform>.vsix
```
```powershell
# Windows — from repo root, after step 3 (adjust <Config> to match what you built)
New-Item -ItemType Directory -Force -Path editors\code\bin
Copy-Item build\<Config>\luau-lsp.exe editors\code\bin\server.exe
Copy-Item README.md editors\code\README.md
Copy-Item CHANGELOG.md editors\code\CHANGELOG.md
cd editors\code
npm install    # first time only
npx @vscode/vsce package --target win32-x64 --out ..\..\overdare-lsp-windows.vsix
```

`editors/code/bin/` is gitignored - it's a build artifact repopulated fresh each time, never
committed. `editors/code/README.md`/`CHANGELOG.md` are likewise gitignored and must be
re-copied from the repo root before every package - without them vsce silently packages
without a README (Marketplace/OpenVSX show a bare listing instead of the real README, which
also means any disclaimer text living only in the root README - e.g. the "unofficial,
not affiliated with OVERDARE" notice - never reaches end users). The `--target` flag only
affects marketplace/registry metadata (which platform variant a listing serves); for a raw
shared `.vsix` file it doesn't gate anything by itself - what actually determines which OS a
given vsix works on is simply which binary (`server` vs `server.exe`) got copied into `bin/`
before packaging. Installing the wrong platform's vsix fails at server-launch time with an
ENOENT-style error, not at install time.
