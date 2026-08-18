# Sourcemap Generation

A sourcemap file maps paths on your file system to paths in the OVERDARE DataModel, and vice-versa.

The Luau Language Server looks for a `sourcemap.json` file in the root of your workspace.

## Generation Options

The extension automatically detects your OVERDARE Studio project's `*.ovdrjm` file and regenerates `sourcemap.json` from it whenever it changes - no configuration needed.

A couple of settings are available if you need them:

- `luau-lsp.sourcemap.sourcemapFile`: what sourcemap file to use (default: `sourcemap.json`)
- `luau-lsp.sourcemap.autogenerate`: disable this if you want to manage the sourcemap yourself instead of having it regenerated automatically

## Sourcemap Structure

If you need to generate your own sourcemap, it should follow this structure:

```json
{
  "name": "Game",
  "className": "DataModel",
  "children": [
    {
      "name": "ReplicatedStorage",
      "className": "ReplicatedStorage",
      "children": [
        {
          "name": "Library",
          "className": "ModuleScript",
          "filePaths": ["ReplicatedStorage/Library.luau"]
        },
        {
          "name": "Logging",
          "className": "ModuleScript",
          "filePaths": ["ReplicatedStorage/Logging.luau"]
        }
      ]
    },
    {
      "name": "ServerScriptService",
      "className": "ServerScriptService",
      "children": [
        ...
      ]
    }
    ...
  ]
}
```
