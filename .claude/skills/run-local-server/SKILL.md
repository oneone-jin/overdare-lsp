---
name: run-local-server
description: Start/verify the overdare-lsp language server locally, either standalone or via the VS Code extension. Use when the user says things like "서버 띄워줘", "run the LSP server locally", or asks to try out the language server after a build.
---

# Run Local Server (overdare-lsp)

Workflow for starting the LSP server locally to try out a change. Assumes `build/luau-lsp`
already exists (run the `fix-and-build` skill first if not).

## Option 1 — VS Code extension (recommended: gives an actual editor experience)

1. Install/compile the extension client:
   ```bash
   cd editors/code
   npm install
   npm run compile
   ```
2. Open the `editors/code` folder in VS Code and press `F5` (or run the "Run Extension"
   launch config) — this opens an Extension Development Host window.
3. In that new window, point the extension at the locally built binary instead of its
   bundled one. Add to the dev host's `settings.json`:
   ```json
   "luau-lsp.server.path": "<repo-root>/build/luau-lsp"
   ```
4. Open a `.luau` file in the dev host window — the server attaches automatically.
   Confirm it's working via hover/completion/diagnostics on that file.

## Option 2 — Standalone process (quick sanity check only)

The `lsp` subcommand talks JSON-RPC over stdio — running it bare will just sit waiting
for input, which is expected, not a hang:
```bash
./build/luau-lsp lsp
```
Ctrl+C to exit. Useful only to confirm the binary starts without crashing; it gives no
visible feedback since there's no client sending requests.

For an actual functional check without a full editor, prefer `analyze` instead (see the
`fix-and-build` skill's verify step) — it runs the same type-checking/diagnostics logic
non-interactively against a file.

## Report

State which option was used and what was observed (extension attached successfully /
diagnostics shown / process started without crashing, etc).
