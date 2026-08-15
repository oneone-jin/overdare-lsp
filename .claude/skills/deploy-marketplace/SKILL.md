---
name: deploy-marketplace
description: One-time checklist to publish overdare-lsp to the VS Code Marketplace and OpenVSX (for Cursor). Use when the user says "마켓플레이스 배포", "openvsx 배포", or wants to walk through publishing steps in order.
---

# Deploy to VS Code Marketplace + OpenVSX

**This skill is temporary.** Once the first successful publish to both registries is
confirmed, replace it with a `ci-cd` skill that covers only the ongoing
release/version-bump/publish workflow (ideally via `.github/workflows/`), and delete this
one - the one-time account-setup steps below won't be needed again.

Publishing targets both registries because the user's devs use both VS Code (Microsoft
Marketplace) and Cursor (a VS Code fork that can't legally use the Microsoft Marketplace,
and historically defaults to OpenVSX). Same vsix, two registries.

## Checklist

### 1. Pre-flight checks on `editors/code/package.json`
- [ ] `publisher` field (`overdare`) matches an account you actually control - **still not
      verified as of this writing, this is the actual blocker for step 2**
- [x] `repository`/`homepage`/`bugs` URLs (`oneone-jin/overdare-lsp`) point at a public repo
- [x] `icon` path (`assets/icon.png`) resolves to a real file
- [x] `license` field / `LICENSE.md` present at repo root, and also copied into
      `editors/code/LICENSE.md` so vsce bundles it into the vsix (vsce looks for a license
      file next to the extension's own `package.json`, not the repo root)
- [ ] `README.md` reads correctly as a Marketplace listing page (it's currently written
      around "build locally + vsix install", not "install from Marketplace" - reword the
      install section once this is live)
- [x] `CHANGELOG.md` backfilled with every unreleased user-facing change since v1.69.0
      (ovdrjm pipeline, platform rename, whitelist/completion fixes, datatype fixes,
      rebranding) - commit `308ad1b`

### 2. Register accounts + get tokens (one-time, per registry)

**Microsoft Marketplace:**
- [ ] Create/sign into an Azure DevOps organization at https://dev.azure.com
- [ ] User settings → Personal access tokens → New Token → Organization: *All accessible
      organizations* → Scopes: **Marketplace → Manage**
- [ ] Save the PAT somewhere safe (shown once)
- [ ] Create the publisher at https://marketplace.visualstudio.com/manage → publisher ID
      must exactly match `package.json`'s `"publisher"` field

**OpenVSX:**
- [ ] Sign in at https://open-vsx.org (GitHub login works) and agree to the publisher
      agreement
- [ ] Generate an access token from your OpenVSX profile settings
- [ ] Register a namespace matching the `publisher` field:
      `npx ovsx create-namespace overdare -p <openvsx-token>`

### 3. Build + package a vsix per platform

Do this once per OS/arch you need to support (macOS arm64/x64, Linux x64/arm64, Windows
x64/arm64) - **no cross-compilation**, each needs its own native build. Use the
`fix-and-build` skill's "package a platform-specific vsix" section for the exact commands;
summary:
```bash
# after building the CLI on this machine
mkdir -p editors/code/bin && cp build/luau-lsp editors/code/bin/server   # or server.exe + copy on Windows
cd editors/code && npm install
npx @vscode/vsce package --target <platform> --out /tmp/overdare-lsp-<platform>.vsix
```

- [ ] macOS arm64 vsix built
- [ ] macOS x64 vsix built
- [ ] Windows x64 vsix built
- [ ] (optional) Linux / arm64 variants, if there are users on those platforms

### 4. Publish to both registries, per platform

```bash
cd editors/code
npx @vscode/vsce login overdare        # first time only, prompts for the Marketplace PAT
npx @vscode/vsce publish --target <platform>

npx ovsx publish --packagePath /tmp/overdare-lsp-<platform>.vsix -p <openvsx-token>
```
- [ ] Published to Microsoft Marketplace for every built platform
- [ ] Published to OpenVSX for every built platform

### 5. Verify
- [ ] Search "OVERDARE" in VS Code's Extensions view → installs and activates cleanly
- [ ] Search "OVERDARE" in Cursor's Extensions view (or install via `.vsix` from OpenVSX) →
      same
- [ ] Open a real `.ovdrjm` project, confirm sourcemap generation + `game` global +
      completion all work post-install (not just "it activated")

### 6. Once all of the above is done
- [ ] Tell the user the checklist is fully green
- [ ] Create a new `ci-cd` skill covering the ongoing release workflow. Note:
      `.github/workflows/release.yml` (inherited from upstream, still intact) already has
      both `vsce publish` and `ovsx publish` steps wired up per-platform, gated behind
      `MARKETPLACE_TOKEN`/`OVSX_TOKEN` repo secrets - so "ongoing CI/CD" may mostly mean
      *adding those two secrets in GitHub repo settings* (using the tokens from step 2
      above) and confirming the workflow's trigger (tag push? manual dispatch? check the
      `on:` block) rather than writing new automation from scratch. Verify this before
      assuming anything needs to be built.
- [ ] Delete this `deploy-marketplace` skill directory - it was scaffolding for the one-time
      account setup, not something that needs to persist
