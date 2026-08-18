# OVERDARE Luau Language Server (Unofficial)

An implementation of a language server for the [Luau](https://github.com/Roblox/luau)
programming language, targeting [OVERDARE](https://www.overdare.com/) projects.

This is an unofficial, community-maintained project and is not affiliated with, endorsed
by, or sponsored by OVERDARE.

This is a fork of [JohnnyMorganz/luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) (MIT
licensed), which the vast majority of this codebase's language-server plumbing still comes
from. This fork replaces the Roblox-specific platform integration with an OVERDARE-specific
one: OVERDARE API type definitions (scraped from [docs.overdare.com](https://docs.overdare.com))
instead of Roblox's, and `.ovdrjm`-based sourcemap generation instead of Rojo's.

## Getting Started

This fork is published on [OpenVSX](https://open-vsx.org) as `wonjin.overdare-lsp` (used by
editors like Cursor); search "OVERDARE" in your editor's extension marketplace. It's not yet
on the VS Code Marketplace.

You can also build and install the extension locally:

```sh
git clone --recurse-submodules https://github.com/oneone-jin/overdare-lsp.git
cd overdare-lsp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --target Luau.LanguageServer.CLI --config RelWithDebInfo
cd ..

# Bundle the server binary into the extension so it works with no extra config
mkdir -p editors/code/bin
cp build/luau-lsp editors/code/bin/server            # macOS/Linux
# copy build\RelWithDebInfo\luau-lsp.exe editors\code\bin\server.exe   # Windows

# Mirror the README/CHANGELOG into the extension so they're bundled into the vsix
cp README.md editors/code/README.md
cp CHANGELOG.md editors/code/CHANGELOG.md

cd editors/code
npm install
npx @vscode/vsce package --out overdare-lsp.vsix
```

Then, in VS Code: Extensions panel → `...` menu → **Install from VSIX...** → select the
generated `.vsix`, then reload the window.

Alternatively, check out [Getting Started for Language Server Clients](editors/README.md)
to set up your own client for a different editor.

### For General Users

The language server will start working immediately for general Luau code. There is built-in support
for Luau's generalised [require-by-string semantics](https://rfcs.luau.org/new-require-by-string-semantics.html), using `require("./module")`.

To provide global type definitions for a custom environment, specify `luau-lsp.types.definitionFiles`.
Corresponding documentation is configured using `luau-lsp.types.documentationFiles`.

### For OVERDARE Users

By default (`luau-lsp.platform.type: "overdare"`), the OVERDARE type definitions and
documentation are preloaded out of the box.

The language server detects your workspace's `*.ovdrjm` project file (OVERDARE Studio's
project format) automatically and keeps a `sourcemap.json` in sync with it — no manual
sourcemap generator to configure. The instance tree, including OVERDARE's flat `Lua/` script
export folder (with `Name`, `Name_1`, `Name_2`, ... disambiguating same-named sibling
scripts), is resolved from the live `.ovdrjm` file on every Studio save/play/publish. Add
`sourcemap.json` to your `.gitignore`.

- `luau-lsp.sourcemap.enabled`: Whether sourcemap support is enabled (default: on)
- `luau-lsp.sourcemap.autogenerate`: Whether the sourcemap is automatically regenerated from `.ovdrjm`. If disabled, the server still listens for manual changes to `sourcemap.json` (default: on)
- `luau-lsp.sourcemap.sourcemapFile`: What sourcemap file to use (default: `sourcemap.json`)

> Note: in the diagnostics type checker, the types for DataModel (DM) instances will resolve to `any`. This is a current limitation to reduce false positives.
> However, autocomplete and hover intellisense will correctly resolve the DM type.
> To enable this mode for diagnostics, set `luau-lsp.diagnostics.strictDatamodelTypes` (off by default).
> [Read more](https://github.com/JohnnyMorganz/luau-lsp/issues/83#issuecomment-1192865024).

## Standalone

The tool can run standalone, similar to [`luau-analyze`](https://github.com/JohnnyMorganz/luau-analyze-rojo), to provide type and lint warnings in CI, with full sourcemap resolution and API types support.
The entry point for the analysis tool is `luau-lsp analyze`.

Install the binary and run `luau-lsp --help` for more information.

## Configuration

There are 2 types of configuration styles for the language server. General configuration is provided by `.luaurc` files,
which allow you to configure language strictness, lints, and require aliases. More information is available in Luau's [RFC documentation](https://rfcs.luau.org/config-luaurc.html).

The second configuration style is specific to the language server. See `luau-lsp` in your editor's settings for more details.

## Supported Features

- [x] Diagnostics (incl. type errors)
- [x] Autocompletion
- [x] Hover
- [x] Signature Help
- [x] Go To Definition
- [x] Go To Type Definition
- [x] Find References
- [x] Document Link
- [x] Document Symbol
- [x] Color Provider
- [x] Rename
- [x] Semantic Tokens
- [x] Inlay Hints
- [x] Documentation Comments ([Moonwave Style](https://github.com/evaera/moonwave) - supporting both `--- comment` and `--[=[ comment ]=]`, but must be next to statement)
- [x] Code Actions
- [x] Workspace Symbols
- [x] Folding Range
- [x] Call Hierarchy

The following are extra features defined in the LSP specification, but most likely do not apply to Luau or are not necessary.
They can be investigated at a later time:

- [ ] Go To Declaration (do not apply)
- [ ] Go To Implementation (do not apply)
- [ ] Code Lens (not necessary)
- [ ] Document Highlight (not necessary - editor highlighting is sufficient)
- [ ] Selection Range (not necessary - editor selection is sufficient)
- [ ] Inline Value (applies for debuggers only)
- [ ] Moniker
- [ ] Formatting (see [stylua](https://github.com/JohnnyMorganz/StyLua))
- [ ] Type Hierarchy (Luau currently does not provide any [public] ways to define type hierarchies)

## Crash Reporting

The language server implements opt-in crash reporting, using [Sentry](https://sentry.io/).

On VSCode, this is configured via the setting `luau-lsp.server.crashReporting.enabled`.
When a crash is encountered, an out-of-process crash handler will upload the crash details to Sentry via HTTP.

When a crash is reported, the report stores the following information:

- Crash reason and thread stack trace
- Device metadata: OS name, version and CPU architecture
- Dynamic libraries loaded into the process (including filesystem paths)

This information is transferred through a [Minidump](https://docs.sentry.io/platforms/native/guides/minidumps/#what-is-a-minidump) file.
This file is not stored after processing. No general usage data is recorded.

Crash Reporting is only available for Windows and macOS, and is not active for Standalone mode (`luau-lsp analyze`).
Note this fork hasn't set up its own Sentry project — crash reporting has no destination configured unless you provide your own.

## Build From Source

Submodules are required to build the project. You should use `--recurse-submodules` when you initially clone the project; e.g.

```sh
git clone https://github.com/oneone-jin/overdare-lsp.git --recurse-submodules
```

To compile the project, execute the following commands in the project root directory.

```sh
git submodule update --init --recursive
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target Luau.LanguageServer.CLI --config Release
```

You can build `Luau.LanguageServer.Test` for unit tests.
Some tests make assumptions about relative file paths.
When running tests, ensure that your current working directory is set to the root of the repository.

## Regenerating OVERDARE's Type Definitions

`scripts/dumpOverdareTypes.py` scrapes the OVERDARE API reference docs and merges the result
into `scripts/globalTypes.d.luau` (classes/enums/datatypes replaced or added; `GetService`'s
service whitelist pruned to only real OVERDARE services). Re-run it after OVERDARE ships new
API surface:

```sh
python3 scripts/dumpOverdareTypes.py --merge-base scripts/globalTypes.d.luau --merged-output scripts/globalTypes.d.luau
cp scripts/globalTypes.d.luau scripts/globalTypes.d.lua   # keep both copies identical
```

## License

MIT - see [LICENSE.md](LICENSE.md). This project is a fork of
[JohnnyMorganz/luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) (Copyright (c) 2022
JohnnyMorganz), also MIT licensed. The bundled VS Code extension additionally ships a few
third-party JS libraries under their own permissive licenses; see
[editors/code/THIRD-PARTY-NOTICES.md](editors/code/THIRD-PARTY-NOTICES.md).
