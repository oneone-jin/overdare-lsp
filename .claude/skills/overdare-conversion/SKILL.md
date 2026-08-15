---
name: overdare-conversion
description: Tracks the plan and progress for converting this luau-lsp fork (originally Roblox-targeted) into an OVERDARE-targeted language server. Use when the user asks "다음 작업 뭐야", "오버데어 변환 진행상황", or wants to resume/plan the conversion work.
---

# OVERDARE Conversion Plan

luau-lsp was built for Roblox (Rojo-style `sourcemap.json`, Roblox `DataModel` service
tree, Roblox Studio companion plugin). OVERDARE (built on Unreal Engine) has its own
project format that's structurally similar but not identical. This skill tracks the
agreed conversion plan and what's already done.

## Key facts established so far

- OVERDARE Studio project root: `overdare/*.ovdrjm` (JSON) — contains the FULL instance
  hierarchy (`Root.LuaChildren[]`, fields: `InstanceType`, `Name`, `ActorGuid`,
  `ObjectKey`, `Source` for script content inline). Rewritten on every Studio save,
  play, and publish.
- Alongside it, `overdare/Lua/*.lua` + `.uasset` — a FLAT mirror of script instances for
  external editing. `.uasset` is a real Unreal `LuaMachine` plugin asset
  (`/Script/LuaMachine.LuaCode`); it carries no back-reference (no ActorGuid/ObjectKey)
  to the tree.
- Flat filename collision rule (verified empirically): when multiple instances share the
  same `Name`, Studio appends `_1`, `_2`, ... in tree pre-order traversal order. No
  stable ID is embedded anywhere in the flat files — the mapping must be recomputed from
  the `.ovdrjm` tree every time, not read off disk.
- Because `.ovdrjm` is rewritten wholesale on every save, drift/stability of the
  suffix-numbering algorithm across edits is NOT a concern — only "does our replay
  algorithm match Studio's algorithm on any given snapshot" matters.
- `SourceNode::fromJson` (`src/platform/roblox/RobloxSourceNode.cpp`) expects exactly
  `{name, className, filePaths[], children[]}` — this is untouched Rojo-sourcemap-shaped
  JSON. We do NOT need to modify the C++ core to consume OVERDARE data; we only need an
  adapter that emits this exact shape from `.ovdrjm`.
- No machine-readable OVERDARE API dump exists (unlike Roblox's `Full-API-Dump.json`
  that `scripts/dumpRobloxTypes.py` consumes). `docs.overdare.com/development/api-reference`
  is HTML-only. OVERDARE API surface: ~100+ classes under `InstanceBase`, names/structure
  very close to Roblox (`Players`, `Workspace`, `DataStoreService`, ...) plus OVERDARE-only
  classes (`ActionRunner`, `ActionSequenceService`, `MaterialService`, etc. — seen in a
  real `.ovdrjm` dump).

## Agreed work order

1. **[DONE] Remove unneeded files** — Roblox Studio companion-plugin live-sync pathway,
   superseded by the `.ovdrjm` → `sourcemap.json` pipeline (step 3). Removed:
   - `src/platform/roblox/RobloxStudioPlugin.cpp` (deleted entirely)
   - `PluginNode` struct, `pluginInfo`/`pluginNodeAllocator`, `setPluginInfo`,
     `clearPluginManagedNodesFromSourcemap`, `hydrateSourcemapWithPluginInfo`,
     `onStudioPluginFullChange`/`onStudioPluginClear`, `handleNotification` override —
     all removed from `RobloxPlatform.hpp` + call sites in `RobloxSourcemap.cpp`
   - `CMakeLists.txt` entry for the deleted file
   - `editors/code/src/roblox.ts`: `startPluginServer`/`stopPluginServer`/
     `setupStudioPlugin`/`getStudioPluginValue`, the express HTTP server, "Setup Plugin"
     UI prompts, `luau-lsp.setupStudioPlugin` command
   - `editors/code/package.json`: `luau-lsp.studioPlugin.*` / `luau-lsp.plugin.*`
     settings, the command entry, `express`/`bytes` dependencies
   - Verified: `npm run check-types` clean, `Luau.LanguageServer.CLI` builds and links,
     `analyze --sourcemap=... ` smoke test passes
   - NOT touched (left as harmless dead field, out of scope): `SourceNode::pluginManaged`
     field + constructor param + JSON round-trip in `RobloxSourceNode.cpp` — removing
     requires updating every `SourceNode` constructor call site, deferred.

2. **[DONE] Project-root detection rule** — an OVERDARE project is identified purely by
   the presence of a `*.ovdrjm` file in the workspace (no user config needed, unlike
   Rojo's `rojoProjectFile` setting).
   - `editors/code/src/roblox.ts`: added `findOvdrjmFile(workspaceFolder)` — searches
     `*.ovdrjm` at the workspace root via `vscode.workspace.findFiles`, warns (doesn't
     fail) if more than one is found and uses the first.
   - Wired into `startSourcemapGeneration`'s `spawnChildProcess()` as a new priority
     branch: `customGeneratorCommand` (user override) > `.ovdrjm` detected (OVERDARE) >
     Rojo project file (fallback). This reused the *existing* sourcemap-generation
     process lifecycle (spawn/kill/disposables) instead of building a parallel one.
   - Verified: `tsc --noEmit` + `eslint` clean, `npm run compile` succeeds, and the exact
     command line the extension would spawn was run manually end-to-end (touching
     `.ovdrjm` → `sourcemap.json` regenerated).

3. **[DONE] `.ovdrjm` → `sourcemap.json` pipeline, wired into the extension**
   - `scripts/ovdrjmToSourcemap.py` — `convert(ovdrjm_path, lua_dir)` walks
     `Root.LuaChildren`, maps `InstanceType`→`className`, `Name`→`name`, computes
     `filePaths` for `Script`/`LocalScript`/`ModuleScript` nodes by replaying the
     dedup-suffix naming rule, keeps `actorGuid`/`objectKey` as extra (unused by C++,
     for future lookups back into `.ovdrjm`). Verified byte-for-byte match against a
     real `overdare/tt.ovdrjm` + `overdare/Lua/` pair (including `_1` duplicates).
   - `scripts/ovdrjmWatch.py` — polls `.ovdrjm` mtime (default 0.5s), regenerates on
     change via `ovdrjmToSourcemap.convert()`, retries up to 5x on transient JSON
     parse/OS errors (Studio writes non-atomically). No external deps (stdlib only).
   - `roblox.ts` now spawns `python3 <resolved-path>/ovdrjmWatch.py <ovdrjm> <ovdrjm-dir>/Lua -o sourcemap.json`
     automatically per step 2's detection, and kills it via the existing
     `sourcemapDisposables` cleanup (same path as Rojo's `--watch` child process) on
     deactivate/config-change/reload.
   - **Known gap to fix before step 6 (vsix build)**: the watcher script path is resolved
     as `context.extensionUri/../../scripts/ovdrjmWatch.py` — this only exists in the
     monorepo source tree (mirrors the existing "Debug" mode branch of `globalTypesUri`).
     A packaged vsix won't have `scripts/` bundled alongside it, so this will break for
     real distribution. Needs either bundling the script into the extension package or
     switching to a different resolution strategy before step 6.
   - Requires `python3` to be on the user's PATH — no check/error message for this yet.

4. **[DONE] API / service type dump** — no machine-readable OVERDARE dump exists (unlike
   Roblox's `Full-API-Dump.json`), BUT `docs.overdare.com` publishes a full Markdown mirror:
   `llms.txt` is a page index, and any doc page is available as Markdown by appending `.md`
   to its URL. Found 134 classes, 71 enums, 31 datatypes (English section).
   - `scripts/dumpOverdareTypes.py` — scrapes all of it and either (a) `--output` a
     standalone `.luau` file (self-stubs any external reference, for isolated testing) or
     (b) `--merge-base scripts/globalTypes.d.luau --merged-output <path>` to merge into
     the real base file: same-named classes/datatypes/enums get REPLACED in place
     (position preserved), OVERDARE-only ones get APPENDED, and the existing `ENUM_LIST`
     table gets patched with new enum entries. Also emits `--dump-json` for the raw
     scraped data (reusable without re-hitting the network).
   - Result on a real merge run: **124 classes replaced, 10 new; 52 enums replaced, 19
     new; 16 datatypes replaced, 14 new (7 new constructor tables)** — compiles with
     `luau-lsp analyze --definitions=...` at **zero errors**, verified against real usage
     (`Player`, `Humanoid`, `Part`, `CFrame.new()`, `RaycastParams.new()`, `ActionRunner`).
   - Real bugs found and fixed along the way (all handled automatically now, not manual
     cleanup): Markdown backslash-escapes leaking into identifiers (`L\_ECC\_Camera`);
     `RBXScriptSignal` needed `export type` syntax, not `declare extern type` w/ generics;
     class declaration order must have parents before children (`fix_class_ordering` now
     auto-detects and fixes violations, e.g. OVERDARE's `FormFactorPart`/`Part` hierarchy
     is reversed from Roblox's); doc-only placeholder type names (`Value`, `Array`,
     `Dictionary`, `Tuple`, `bool`, `table`, `function`) mapped to real Luau types;
     **`Enum`/`EnumItem` are also (mis)listed as "datatypes" in the OVERDARE docs — scraping
     and replacing them crashed (segfaulted) the type checker; now explicitly excluded**;
     `RaycastResult` already exists in the base file as a generic `export type` alias, not
     `declare extern type` — redeclaring it under both forms also segfaulted the type
     checker; now auto-detected via `find_export_type_names` and skipped rather than
     duplicated; `TeleportResult` is referenced but has no doc page at all (a genuine
     OVERDARE docs gap) — auto-stubbed as an empty `declare extern type` with a warning.
   - **Applied for real**: `scripts/globalTypes.d.luau` and `scripts/globalTypes.d.lua`
     (kept byte-identical, per repo convention) now contain the merged output. Full test
     suite (`Luau.LanguageServer.Test`, 859 cases) passes with no regressions.

5. **[DONE] `GetService` string-name validation for OVERDARE-only services** — turned out
   to NOT be hardcoded in C++ at all (that was a wrong guess in the step-4 writeup above).
   `src/platform/roblox/RobloxLuauExt.cpp:768` reads the allowed service list from
   `RobloxDefinitionsFileMetadata.SERVICES`, which is populated from a
   `--#METADATA#{...}` JSON comment on **line 1** of `globalTypes.d.luau` itself — pure
   data, no C++ change needed.
   - Compared OVERDARE's 134 scraped classes against the existing `SERVICES` list: only
     `ActionSequenceService` and `WorldRankService` were missing (`MaterialService`,
     `PhysicsService`, and the rest already existed from Roblox). Added both to the
     METADATA JSON in both `.d.luau`/`.d.lua`.
   - Also handled two OVERDARE-specific global function differences reported directly by
     the user (not scraped - OVERDARE's docs don't have a globals page, see step 4):
     removed `declare function warn<T...>(...: T...)` (OVERDARE doesn't support `warn`)
     and added `declare function isnil<T>(value: T): boolean` (OVERDARE's dedicated nil-check).
   - Verified: `game:GetService("ActionSequenceService")` / `("WorldRankService")` now
     resolve correctly, `isnil(x)` type-checks, `warn(...)` correctly errors as an unknown
     global. (The "859/859 passing" claimed at the end of step 4 was WRONG - see step 6.)
   - **Not yet swept**: no systematic diff of OVERDARE's full global-function surface vs
     Roblox's (only `warn`/`isnil` were checked, both user-reported). Other stdlib globals
     Roblox has that OVERDARE might not (or vice versa) haven't been audited.

6. **[DONE] Test suite** — found and fixed a real gap, then added OVERDARE-specific
   coverage.
   - **Important correction**: every "test suite passes" claim made earlier in this session
     (after step 1's `RobloxStudioPlugin.cpp` deletion) was checked against a **stale
     binary** (`build/Luau.LanguageServer.Test` dated Jul 25, i.e. from before this session's
     changes - only `Luau.LanguageServer.CLI` had been rebuilt, never the test target). When
     actually rebuilt, `tests/Sourcemap.test.cpp` failed to COMPILE: ~20 test cases called
     `onStudioPluginFullChange`/`onStudioPluginClear`/`PluginNode::fromJson`, all removed in
     step 1. **Lesson: rebuild `Luau.LanguageServer.Test` (not just the CLI) after touching
     `src/`, and check the binary's mtime before trusting a "passed" claim.**
   - Fixed by deleting the ~19 test cases that directly exercised the removed Studio-plugin
     live-push feature (no replacement - the feature is gone, not moved). Kept tests that
     only touched the still-present `SourceNode::pluginManaged` field (pure JSON
     serialization, e.g. `plugin_managed_flag_persists_through_sourcemap_reload`) since that
     field itself wasn't removed (see step 1's note on deferred cleanup). Rewrote
     `source_node_get_script_context_resolution` to use `loadSourcemap()` (Rojo-shaped JSON)
     instead of the deleted plugin-push API - the behavior it tests (script context
     resolution) is unrelated to the plugin feature and still worth covering.
   - Result: suite went from failing to compile → **840/840 passing** on a genuinely fresh
     build.
   - Added `tests/OverdareTypes.test.cpp` (registered in `CMakeLists.txt`), 4 new cases:
     - `production_global_types_definitions_file_loads_without_errors` - loads the REAL
       `scripts/globalTypes.d.luau` (not a testdata stub) and type-checks `Instance`/
       `Player`/`Humanoid` usage. Regression guard against a future `dumpOverdareTypes.py`
       run producing bad output (would have caught every bug found during step 4's
       development: syntax errors, the `Enum`/`EnumItem`/`RaycastResult` segfaults, etc).
     - `overdare_only_service_names_are_recognised_by_get_service` - covers step 5's
       METADATA `SERVICES` fix directly.
     - `isnil_global_is_available_and_warn_is_not` - covers step 5's global-function fix.
     - `ovdrjm_shaped_sourcemap_resolves_requires_with_dedup_suffixed_siblings` - loads a
       sourcemap shaped exactly like `ovdrjmToSourcemap.py`'s real output (OVERDARE-only
       service as a DataModel child + two same-`name` `Script` nodes disambiguated by a flat
       `_1` `filePaths` suffix) and verifies `require`/`GetService` resolve.
   - **Important pattern note for future tests touching real definitions**: don't use the
     shared `Fixture` + `loadDefinition("@roblox", ...)` for this - `Fixture`'s constructor
     already eagerly loads `tests/testdata/standard_definitions.d.luau` as `"@roblox"`
     (via `setupWithConfiguration` → `registerTypes`), so loading the real file under the
     same package name a second time redeclares every type and **segfaults** (same failure
     mode as step 4's `Enum`/`RaycastResult` bugs, just self-inflicted by the test). Instead,
     construct a fresh `CliClient` + `WorkspaceFolder` per test case, mirroring
     `tests/AnalyzeCli.test.cpp`'s `initCliClient`/`setupCliWorkspace` helpers (re-implemented
     locally in `OverdareTypes.test.cpp` since those are file-local `static` functions, not
     exported).

7. **[DONE] Rename Roblox-branded files/identifiers to OVERDARE** — done before vsix
   packaging so the packaged extension doesn't ship internal Roblox naming. User explicitly
   chose full scope: internal C++ identifiers AND user-facing config strings (not just a
   cosmetic internal-only rename).
   - **Files renamed** (`git mv`, history preserved): `src/platform/roblox/` →
     `src/platform/overdare/`; `Roblox*.cpp` → `Overdare*.cpp` (8 files); `RobloxPlatform.hpp`
     → `OverdarePlatform.hpp`; `RobloxStringRequireSuggester.hpp` →
     `OverdareStringRequireSuggester.hpp`; `tests/RobloxTestConstants.h` →
     `tests/OverdareTestConstants.h`; `editors/code/src/roblox.ts` → `overdare.ts`.
     `CMakeLists.txt` updated to match.
   - **C++ identifiers renamed**: `RobloxPlatform`→`OverdarePlatform`,
     `RobloxCliClient`→`OverdareCliClient`, `RobloxColorVisitor`→`OverdareColorVisitor`,
     `RobloxDefinitionsFileMetadata`→`OverdareDefinitionsFileMetadata`,
     `RobloxFindImportsVisitor`→`OverdareFindImportsVisitor`,
     `RobloxSourcemapConfiguration`→`OverdareSourcemapConfiguration` (as
     `ClientOverdareSourcemapConfiguration`), `RobloxStringRequireSuggester`→
     `OverdareStringRequireSuggester`, `RobloxTestConstants`→`OverdareTestConstants`, plus
     local variable renames (`robloxPlatform`→`overdarePlatform`,
     `robloxMetadata`→`overdareMetadata`) for consistency.
   - **User-facing config renamed** (the risky/breaking part, explicitly approved): the
     `luau-lsp.platform.type` enum — C++ `LSPPlatformConfig::Roblox` → `::Overdare`, JSON
     string `"roblox"` → `"overdare"` (default value too) — and the CLI's `--platform`
     flag/choices to match. The internal LSP definitions-package key `"@roblox"` (used in
     `client->definitionsFiles`, log messages, the extension's definitions config, and the
     CLI's legacy `--definitions=path` auto-naming) → `"@overdare"` everywhere, including
     the auto-incrementing legacy suffix form (`@roblox1` → `@overdare1`).
   - **Deliberately left untouched** (verified each is either a real external reference or
     a distinct feature, not just leftover branding):
     - `.robloxrc` config filename (backwards-compat shim for a real legacy file format,
       not our branding to rename)
     - `github.com/Roblox/luau` URLs (real upstream repo links)
     - `clientsettingscdn.roblox.com` FFlags-sync endpoint and the "Sync FFlags" feature
       (real Roblox infrastructure the extension talks to; unrelated to platform branding)
     - The **separate** `luau-lsp.types.roblox` / `robloxSecurityLevel` toggle (C++ field
       `ClientTypesConfiguration::roblox`, JSON key `"roblox"`) - this is an independent
       opt-in "also load Roblox's public type definitions" feature, not the platform
       selector. **This DID have a real bug, now fixed** (see step 7.1 below): its
       `preLanguageServerStart` code path was downloading fresh Roblox types from
       `luau-lsp.pages.dev` into `@overdare`'s definitions slot whenever this toggle was on
       (true by default), silently discarding our merged OVERDARE `globalTypes.d.luau`.
     - `"@roblox/enum/..."` and the `"@roblox"`→`"@luau"` documentation-symbol rewrite in
       `OverdareLuauExt.cpp` (`fixDebugDocumentationSymbol`) - these key into an *external*
       Roblox-hosted documentation JSON (`API_DOCS` URL) by a fixed symbol format; renaming
       the string wouldn't change what that external file contains, so class/enum tooltips
       would just stop resolving. Left as `@roblox` intentionally.
   - **Bug found the same way as steps 4/6**: `tests/InlayHints.test.cpp`'s
     `inlay_hint_generics_and_extern_type` hardcoded `"@roblox/globaltype/Instance"` and
     broke when `Fixture`'s default package key became `"@overdare"` - confirms
     documentation symbols ARE generated from the loading package name dynamically (not a
     fixed literal), fixed by updating the test's expectation.
   - Also removed the dead `companion-plugin` walkthrough step from
     `editors/code/package.json` (referenced the Studio plugin feature deleted in step 1;
     missed at the time) and reworded the `platform`/`sourcemap` walkthrough steps'
     copy/`when` clauses for OVERDARE.
   - Verified: `tsc --noEmit` clean, full CLI + Test rebuild clean, **840/840 passing**,
     `analyze --platform overdare` end-to-end smoke test passes with the renamed package
     key showing in logs (`Loading definitions file: @overdare - ...`).

7.1. **[DONE] Fix `@overdare` definitions silently downloading pure Roblox types** —
   `editors/code/src/extension.ts`'s `handleExternalFiles` unconditionally queued every
   entry in `builtinDefinitionFiles` for network download (no `isExternalFile` check,
   unlike the settings-based `definitionFilesConfig` path right above it which did check).
   `overdare.ts`'s `preLanguageServerStart` always returned a `luau-lsp.pages.dev` URL for
   `@overdare` whenever `luau-lsp.types.roblox` was on (default true) - so the committed,
   merged `scripts/globalTypes.d.luau` was never actually loaded; a fresh pure-Roblox file
   got downloaded to `globalStorageUri` and used instead, both on first run and on the
   daily re-fetch (`shouldFetchDefinitions`).
   - Fixed: `handleExternalFiles`'s builtin-merge loop now checks `isExternalFile(url)` -
     a non-URL entry is used directly with no download, mirroring the existing
     settings-file behavior.
   - `overdare.ts` originally pointed `@overdare` at a local
     `scripts/globalTypes.d.luau` path instead of the `luau-lsp.pages.dev` URL - see 7.2
     for how this was superseded by inlining the file entirely.
   - Verified: `tsc --noEmit` + `eslint` clean, `npm run compile` succeeds.

7.2. **[DONE] Fix packaged-vsix path resolution for `globalTypes.d.luau` and the `.ovdrjm`
   watcher** — both 7.1's definitions path and step 3's watcher script resolved paths via
   `context.extensionUri/../../scripts/...`, which only exists in the monorepo source tree;
   a packaged vsix doesn't ship `scripts/` alongside it. Presented three options (bundle via
   a CI copy step matching the existing `editors/code/bin/server` precedent in
   `.github/workflows/release.yml`; inline `globalTypes.d.luau` into the JS bundle;
   port the Python watcher to TypeScript) - went with the latter two since they eliminate
   the packaging problem entirely rather than requiring an extra copy/sync step to remember.
   - **`globalTypes.d.luau` inlined into the extension bundle**: `esbuild.mjs` now has
     `loader: { ".luau": "text" }`; `overdare.ts` imports the file directly
     (`import overdareGlobalTypesSource from "../../../scripts/globalTypes.d.luau"`, 3
     levels up from `src/` - not 2, that was an easy off-by-one since other code in this
     file resolves relative to `context.extensionUri` which is one level higher than the
     source file itself). Added `src/assets.d.ts` for the ambient `*.d.luau` module type.
     Since the LSP server still needs an actual file path (not inline content) to load via
     `--definitions`, `ensureOverdareGlobalTypesWritten` writes the bundled string to
     `context.globalStorageUri/globalTypes.d.luau` once at startup, skipping the write if
     the content hasn't changed (avoids spurious file-watch reloads). Verified: content
     round-trips into `dist/extension.js` (`grep -c "declare extern type Player"` found it
     inlined).
   - **`.ovdrjm` watcher ported to `editors/code/src/ovdrjmSourcemap.ts`**
     (`convertOvdrjmToSourcemap` + `watchOvdrjm`, using `vscode.workspace.fs` and
     `setTimeout` polling instead of `child_process.spawn("python3", ...)`). Also removes
     the "user needs python3 on PATH" fragility flagged back in step 2/3. `scripts/
     ovdrjmWatch.py` deleted (dead code, nothing references it anymore); `scripts/
     ovdrjmToSourcemap.py` kept as a standalone CLI conversion tool since it's still useful
     outside the extension (manual runs, CI, etc.) - `dumpOverdareTypes.py` doesn't depend
     on it either way, they're unrelated scripts.
   - `startSourcemapGeneration` in `overdare.ts` restructured: the `.ovdrjm` detection now
     happens *before* `spawnChildProcess` is even defined (early-return with its own
     `addSourcemapDisposable(workspaceFolder, watchOvdrjm(...))` call) rather than being a
     branch inside the generic child-process flow - `watchOvdrjm` returns a plain
     `vscode.Disposable`, not something with `ChildProcess`-shaped `.stderr`/`.on(...)` the
     rest of that flow expects, so it genuinely doesn't fit the same abstraction as the
     Rojo/custom-command cases.
   - Verified: `convertOvdrjmToSourcemap`'s tree-building logic produces byte-identical
     output to the Python version against the real `overdare/tt.ovdrjm` fixture (diffed,
     only difference was a trailing newline). `tsc --noEmit` + `eslint` clean (a few
     pre-existing-style `naming-convention` warnings for PascalCase fields that must match
     the real `.ovdrjm` JSON schema - not errors, not new).
   - Both fixes together mean **no remaining known blocker for step 8** on the path-
     resolution front specifically (worth re-checking for others when actually packaging).

8. **[DONE] vsix test build** — `npx @vscode/vsce package --out /tmp/overdare-lsp-test.vsix`
   from `editors/code/` packages cleanly (`vscode:prepublish` runs `tsc --noEmit` +
   production esbuild automatically). Confirmed `dist/extension.js` in the vsix contains
   the inlined OVERDARE types (`declare extern type Player`, `declare function isnil`,
   `ActionSequenceService`) and `package.json`'s `luau-lsp.platform.type` defaults to
   `"overdare"`. No `bin/server` binary is bundled by this local packaging step (that only
   happens in `.github/workflows/release.yml`'s CI copy step) - for local testing, install
   the vsix then set `luau-lsp.server.path` to the locally built `build/luau-lsp` manually.
   - **Manual install-and-test round-trip caught a real bug**: `game:GetService("...")`
     autocomplete was suggesting ~300 Roblox-only services OVERDARE doesn't have. Root
     cause and fix: see the `Prune GetService SERVICES whitelist` work - the
     `--#METADATA#` `SERVICES` list was inherited verbatim from Roblox's dump (331 entries)
     and never pruned; cut to the 28 that intersect with our scraped OVERDARE class list,
     with `prune_services_metadata` added to `dumpOverdareTypes.py`'s merge step so this
     survives future re-scrapes. Verified in a second install-and-reload round-trip.
   - This is the intended manual verification loop for future changes touching the
     extension: `npm run compile` (or let `vsce package` do it) → `vsce package` → install
     the vsix in VS Code ("Install from VSIX...") → Reload Window → test in a real
     `.luau` file. Automated tests (`tsc`/`eslint`/the C++ test suite) don't exercise the
     actual packaged extension end-to-end, so this manual pass is what catches
     packaging-shape and runtime-config bugs like the SERVICES one.

9. **[DONE, partial scope] Branding/metadata** — `editors/code/package.json`:
   - `name`: `luau-lsp` → `overdare-lsp` (the marketplace/extension ID)
   - `displayName`: `Luau Language Server` → `OVERDARE Luau Language Server`
   - `description`: → `A Language Server for Luau on OVERDARE`
   - `publisher`: `JohnnyMorganz` → `overdare` (placeholder org id - this fork isn't
     published by the upstream author, so keeping their publisher ID would misattribute
     marketplace ownership; swap for the real publisher id before any real listing)
   - `homepage`/`repository`/`bugs`: → `https://github.com/oneone-jin/overdare-lsp`
   - `author` and the one genuine upstream issue-thread link
     (`github.com/JohnnyMorganz/luau-lsp/issues/83#...`, cited for its technical rationale)
     left as-is - real attribution/reference, not leftover branding.
   - Verified: `tsc --noEmit` clean (confirms `npm run` picked up the new package name),
     `vsce package` repackages cleanly with the new metadata.
   - **Deliberately NOT done** (scope call, not an oversight): renaming the `luau-lsp.*`
     config/command prefix. This is a ~117-occurrence footprint in `package.json` alone
     (every setting key, command id, `when` clause) plus every `vscode.workspace.
     getConfiguration("luau-lsp...")` call across the TS source - a large, purely-cosmetic,
     breaking change with no functional upside for internal testing (keeping a fork's
     upstream config namespace after a rebrand is a common, unremarkable pattern). Revisit
     only if/when this is heading toward a real separate marketplace listing.
   - **Also not touched at the time**: root `README.md`/`CHANGELOG.md`. `README.md` was
     since rewritten for the fork (see below) - `CHANGELOG.md` is still untouched upstream
     content.

10. **[DONE] Merged to `main` and pushed** — `feature-transition` (containing all of the
    above) was PR'd and merged into `main` (`origin/HEAD` was already `main` by default, so
    a plain `git clone` with no `-b` flag lands there - this bit a fresh Windows clone
    during manual testing, which needed `git checkout feature-transition` before the fixes
    were visible). Work now happens directly on `main`.

11. **[DONE] `README.md` rewritten** for the fork: no marketplace/OpenVSX listing (documents
    local build+`vsce package` install instead), `.ovdrjm`-based sourcemap as the primary
    flow with Rojo kept as the documented fallback for non-OVERDARE projects, the deleted
    Studio companion-plugin mention removed, `oneone-jin/overdare-lsp` clone URLs, and a new
    section on re-running `dumpOverdareTypes.py`. Upstream attribution and genuine technical
    references left as-is. `CHANGELOG.md` still untouched.

12. **[DONE] Local platform-specific vsix packaging verified** - bundled `bin/server`
    (macOS universal binary, both x64+arm64) into a vsix via the `fix-and-build` skill's new
    packaging step; confirmed it installs and runs with zero manual server-path config.
    Windows needs the equivalent build+bundle on a Windows machine (native binary, can't
    cross-compile) - `fix-and-build` has the PowerShell commands.

13. **[DONE] Pruned `Instance.new`/`IsA` class-name whitelists** - same root cause as step
    5's `GetService` fix, found via manual testing in the installed vsix (not caught by any
    automated test): `Instance.new("...")` and `IsA`/`FindFirstChildOfClass`/
    `FindFirstAncestorOfClass`/etc. completion were both suggesting hundreds of Roblox-only
    classes.
    - `CREATABLE_INSTANCES` (metadata whitelist behind `Instance.new`): 357 → 60, same
      intersection-with-scraped-classes technique as `SERVICES`.
      `prune_services_metadata` in `dumpOverdareTypes.py` generalized to handle both fields.
    - New `CLASSES` metadata field (all 134 real OVERDARE classes) added to
      `OverdareDefinitionsFileMetadata` (`src/include/Platform/OverdarePlatform.hpp`) and to
      the merge script. `OverdareCompletion.cpp`'s `"ClassNames"` tag handler - used by
      `IsA`/`FindFirstChildOfClass`/etc. - previously enumerated *every* Instance-derived
      type still declared in the file (there are ~2000 untouched Roblox ones with no
      whitelist to check against, unlike `GetService`/`Instance.new` which already had
      metadata lists). Now filters against `CLASSES` when present; falls back to unfiltered
      when absent (e.g. the small test fixture) so existing tests don't regress.
    - Verified via `analyze`: `Instance.new("Part")` passes, `Instance.new("BadgeService")`
      correctly errors. Full suite still 840/840.
    - **A proper automated completion regression test was attempted and abandoned** - not
      because the fix is unverified, but because two dead ends surfaced along the way,
      both worth remembering:
      - An ad-hoc `CliClient`+`WorkspaceFolder` completion test (mirroring the pattern used
        for `checkSimple` elsewhere in `OverdareTypes.test.cpp`) returned an empty
        `workspace.completion(...)` result unconditionally, for reasons not root-caused
        (tried: disabling fragment autocomplete, disabling sourcemap, forcing strict mode -
        none of it changed the empty result). `checkSimple`-based tests in the same file
        work fine, so something about the completion code path specifically doesn't like
        this harness. Worth another look with more time, but not this fix's fault.
      - Investigating further by driving the *real* `luau-lsp lsp` stdio process by hand
        (piping raw `Content-Length`-framed JSON-RPC) surfaced a genuine, **pre-existing**
        crash: `Thread::~Thread()` calls `std::terminate()` (an unjoined/undetached
        `std::thread` being destroyed) during `LanguageServer`'s shutdown, when the process
        sees the stdin pipe close. Reproduced identically on `main` from *before* this
        fix's changes, so it's unrelated to this work - confirmed via `git stash` + rebuild.
        Only triggered by an abrupt stdin close in this manual test, not by a client's
        normal `shutdown`/`exit` handshake, so it likely doesn't affect real VS Code
        sessions under normal operation - but could plausibly hit if VS Code ever kills the
        server process uncleanly (force-quit, host restart, crash). **Not yet fixed or filed
        as its own tracked item - worth a dedicated look** (find the un-joined `Thread`
        member and either `join()` or `detach()` it before/in `~LanguageServer()`).

14. **[DONE] Fixed datatype constructors/static-constants scraping (`Color3.fromRGB`,
    `CFrame.identity`, `Vector3.zero`, etc. missing from autocomplete)** - user-reported bug,
    root-caused in `dumpOverdareTypes.py`'s `parse_datatype_page`:
    - Constructor parsing filtered to only capture entries literally named `"new"`, silently
      dropping every other named static constructor (`Color3.fromRGB`/`fromHSV`/`fromHex`,
      `CFrame.fromEulerAnglesXYZ`/`fromEulerAnglesYXZ`/`fromMatrix`/`fromOrientation`/
      `lookAt`/`Angles`). Fixed by capturing all `### <name>` entries under `## Constructors`
      as `(name, params)` tuples and emitting each by its real name in
      `declare_datatype_constructor`, matching the real upstream Roblox convention (verified
      via `git show 71ab151:scripts/globalTypes.d.luau`).
    - Separately, the docs put both real instance properties (`Magnitude`, `X`, `Unit`, ...)
      and static namespace constants (`identity`, `zero`, `one`, `xAxis`, `yAxis`, `zAxis`,
      ...) under the same undifferentiated `## Properties` heading. These were all being
      emitted as instance members inside `declare extern type X with ... end`, so
      `CFrame.identity`/`Vector3.zero` etc. (accessed on the namespace, not an instance)
      didn't exist. Fixed with a naming heuristic - **lowercase-first-letter property names
      are static constants** (matches the real convention: PascalCase = instance property,
      camelCase = static constant) - splitting `properties` into `properties` (instance,
      stays in the `extern type` block) vs `staticProperties` (moved into the
      `declare X: { ... }` global table alongside constructors). `declare_datatype_constructor`
      now also fires when a datatype has static properties but zero named constructors.
    - Verified via `analyze` CLI: `Color3.fromRGB(255,0,0)`, `CFrame.identity`,
      `Vector3.zero`/`.one`, `CFrame.lookAt(...)` all type-check with no errors. Full
      re-scrape from scratch reproduced byte-identical results. 840/840 tests still pass
      (no C++ rebuild needed - only the data file changed). Committed as `1e1f811`.

15. **[DONE] Verified `export type` in `.lua` ModuleScripts resolves correctly across
    `require()`** - user question, not previously tested. Built a throwaway sourcemap +
    two-file test (`ReplicatedStorage/Types` ModuleScript exporting a type,
    `ServerScriptService/Main` Script requiring it via `game:GetService(...)`) and ran
    `luau-lsp analyze` with `--sourcemap`. Confirmed: `require()` resolves through the
    OVERDARE-hierarchy sourcemap exactly like Roblox/Rojo did, the exported type's fields
    are fully known (missing-field and wrong-field-type errors both fire correctly), and
    under `--!strict` a wrong-type field assignment inside the required type produces a
    proper `TypeError`. No bug found - this already works, inherited unchanged from
    upstream's require/module-resolution machinery. No code changes made; test files were
    scratch-only and not added to the repo.

16. **[DONE] Pruned `Enum.` global table and EnumX type-annotation completion to real
    OVERDARE enums** - user-reported ("Enum도 roblox타입들에 모두 포함되어 있는 것 같은데"),
    same root-cause family as steps 5/13 (`SERVICES`/`CREATABLE_INSTANCES`/`CLASSES`): the
    file has 626 `declare extern type EnumX extends EnumItem with end` blocks (Roblox's full
    dump), but only 71 are real OVERDARE enums, and `ENUM_LIST` (the type behind
    `declare Enum: ENUM_LIST`, i.e. `Enum.Material` etc.) was never pruned - only ever
    *appended to* when a scraped enum was brand new, so all 626 Roblox-only entries stayed
    visible via `Enum.` autocomplete forever.
    - Added `prune_enum_list()` to `dumpOverdareTypes.py`: fully rebuilds the `ENUM_LIST`
      table body from the scraped enum set on every merge (626 → 71), replacing the old
      incremental-append-only regex patch. Underlying `EnumX`/`EnumX_INTERNAL` extern type
      declarations for pruned-out enums are deliberately left in place (same call as
      CLASSES/instance types - too invasive to delete, and other declarations may still
      reference them), they're just no longer reachable through the `Enum` global.
    - That alone doesn't cover standalone type annotations (`local x: EnumFoo`), which go
      through the same core-engine completion path as `local x: SomeRobloxOnlyClass` did
      before the `handleCompletion` fix in step 13 - so added a new `ENUMS` metadata field
      (mirroring `CLASSES`) and extended `OverdarePlatform::handleCompletion`
      (`OverdareCompletion.cpp`) to also drop any suggested `EnumItem`-subclass type whose
      bare name (stripping the `Enum` prefix) isn't in `ENUMS`. `Enum`/`EnumItem` themselves
      are explicitly excluded from filtering (foundational plumbing types, not real enums).
    - Verified via `analyze` CLI (`Enum.Material` type-checks) and full re-scrape from
      scratch (reproducible: 626 → 71 both times). Rebuilt CLI + Test targets (touched C++
      this time, unlike steps 14/15) - 840/840 still pass.

## How to resume

When picking this back up, re-read this file first, then check `git log`/`git diff` to
confirm which of the DONE items are actually committed (vs. still local/uncommitted from
a prior session). Ask the user which numbered step to continue with rather than assuming.
