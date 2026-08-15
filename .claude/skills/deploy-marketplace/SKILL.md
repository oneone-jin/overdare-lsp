---
name: deploy-marketplace
description: Publish overdare-lsp updates to the VS Code Marketplace and OpenVSX (for Cursor). Use when the user says "마켓플레이스 배포", "openvsx 배포", "버전 올려서 배포", or wants to ship a new version to either/both registries.
---

# Deploy to VS Code Marketplace + OpenVSX

Publishing targets both registries because devs use both VS Code (Microsoft Marketplace) and
Cursor (a VS Code fork that can't legally use the Microsoft Marketplace, and historically
defaults to OpenVSX). Same vsix, two registries.

**Status as of v1.69.7**: published to both registries as `wonjin.overdare-lsp`, Windows
(`win32-x64`) only - verified live via the Marketplace Gallery API and
`open-vsx.org/api/wonjin/overdare-lsp` on 2026-08-16 (212 downloads so far). This machine can
only build for the OS it's running on, no cross-compilation. macOS/Linux need their own
native build + publish pass, on a machine of that OS.

## One-time setup (already done, for reference)

- `publisher` in `editors/code/package.json` is `wonjin` (the `overdare` publisher ID wasn't
  actually registered, so this account is used instead)
- `repository`/`homepage`/`bugs` point at `oneone-jin/overdare-lsp`, now public
- `LICENSE.md` is copied into `editors/code/LICENSE.md` so vsce bundles it (vsce looks for a
  license file next to the extension's own `package.json`, not the repo root)
- OpenVSX namespace `wonjin` is registered
- `OVSX_TOKEN` is set as a permanent Windows user env var (`setx OVSX_TOKEN "..."`) on this
  machine, and also registered as a GitHub Actions repo secret
- Marketplace: publisher account exists under `wonjin`, but there's no PAT set up yet - see
  "Publishing to the Marketplace" below for the workaround in use until one exists

## Repeatable process for a new version

1. **Bump the version** in three places (they must all match):
   - `CMakeLists.txt`: `set(LSP_VERSION "X.Y.Z")`
   - `editors/code/package.json`: `"version": "X.Y.Z"`
   - Add a `CHANGELOG.md` entry under a new `## [X.Y.Z] - <date>` heading
   - Neither registry allows republishing an already-used version number, so this is required
     even for a docs-only or metadata-only change.

2. **Rebuild the CLI** (see the `fix-and-build` skill for full build commands):
   ```powershell
   cmake --build build --target Luau.LanguageServer.CLI --config RelWithDebInfo
   .\build\RelWithDebInfo\luau-lsp.exe --version   # sanity-check it printed the new version
   ```

3. **Refresh what gets bundled into the vsix** - these are gitignored and regenerated from
   the repo root each time (see `editors/code/.gitignore`):
   ```powershell
   Copy-Item build\RelWithDebInfo\luau-lsp.exe editors\code\bin\server.exe -Force
   Copy-Item README.md editors\code\README.md -Force
   Copy-Item CHANGELOG.md editors\code\CHANGELOG.md -Force
   ```

4. **Sync `package-lock.json`** to the new version and **package**:
   ```powershell
   cd editors\code
   npm install
   npx @vscode/vsce package --target win32-x64 --out ..\..\overdare-lsp-windows.vsix
   ```

5. **Publish to OpenVSX** (CLI, using the env var token so it's never typed into chat/logs):
   ```powershell
   $OVSX_TOKEN = [System.Environment]::GetEnvironmentVariable("OVSX_TOKEN","User")
   npx ovsx publish --packagePath ..\..\overdare-lsp-windows.vsix -p $OVSX_TOKEN
   ```

6. **Publish to the Marketplace** - no PAT is set up yet, so this has been done via the web
   UI instead of `vsce publish`:
   - Go to https://marketplace.visualstudio.com/manage, publisher `wonjin`
   - Upload `overdare-lsp-windows.vsix` directly as a new version
   - Once a PAT exists (Azure DevOps → Personal access tokens → Marketplace: Manage scope),
     this step can switch to `npx @vscode/vsce publish --target win32-x64` instead, which
     also lets `.github/workflows/release.yml`'s existing `vsce publish` step work (it's
     already wired up, just needs the `MARKETPLACE_TOKEN` repo secret added - `OVSX_TOKEN` is
     already there, so only the Marketplace half of that workflow is currently blocked)

7. **Commit + push** the version bump and whatever code/doc changes prompted the release, then
   verify:
   ```
   curl -s -X POST "https://marketplace.visualstudio.com/_apis/public/gallery/extensionquery" \
     -H "Content-Type: application/json" -H "Accept: application/json;api-version=3.0-preview.1" \
     -d '{"filters":[{"criteria":[{"filterType":7,"value":"wonjin.overdare-lsp"}]}],"flags":914}'
   ```
   (checks the Marketplace Gallery API directly - the human-facing listing page is a
   client-rendered SPA and can lag behind what the API already reflects)

## Notes

- `editors/code/bin/`, `editors/code/README.md`, `editors/code/CHANGELOG.md` are all
  gitignored build artifacts, regenerated fresh every package - never commit them.
- A stale already-running language server process won't pick up a newly-installed extension
  version until the editor window is fully reloaded (`Developer: Reload Window`) - worth
  knowing if a just-published fix "isn't working" for someone who already had the extension
  installed.
