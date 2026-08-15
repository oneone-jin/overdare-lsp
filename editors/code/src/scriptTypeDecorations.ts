import * as vscode from "vscode";
import * as path from "path";
import * as utils from "./utils";

// Badges/colors overlaid on Script/LocalScript/ModuleScript files in the Explorer, based
// on the className each file resolves to in sourcemap.json (see ovdrjmSourcemap.ts and
// SourceNode::fromJson - same schema either way, whether the sourcemap was generated from
// an .ovdrjm or a Rojo project). This is deliberately separate from the language server:
// it's Explorer-only decoration, doesn't affect diagnostics/completion/etc.
//
// VS Code has no API for swapping a file's icon image per-file at runtime - only this
// FileDecorationProvider badge+color overlay on top of whatever icon the active icon theme
// already draws.

interface SourceMapNode {
  name: string;
  className: string;
  filePaths?: string[];
  children?: SourceMapNode[];
}

interface ScriptDecorationSpec {
  badge: string;
  color: vscode.ThemeColor;
  tooltip: string;
}

const DECORATIONS: Record<string, ScriptDecorationSpec> = {
  ModuleScript: {
    badge: "M",
    color: new vscode.ThemeColor("charts.purple"),
    tooltip: "ModuleScript",
  },
  Script: {
    badge: "S",
    color: new vscode.ThemeColor("disabledForeground"),
    tooltip: "Script",
  },
  LocalScript: {
    badge: "L",
    color: new vscode.ThemeColor("charts.blue"),
    tooltip: "LocalScript",
  },
};

// Windows fsPaths can differ in drive-letter casing/separators between what sourcemap.json
// contains and what VS Code hands us in provideFileDecoration - normalize both sides so
// lookups actually hit.
const normalizeKey = (fsPath: string): string => {
  const normalized = path.normalize(fsPath);
  return process.platform === "win32" ? normalized.toLowerCase() : normalized;
};

const flatten = (
  node: SourceMapNode,
  baseDir: string,
  out: Map<string, string>,
): void => {
  for (const filePath of node.filePaths ?? []) {
    out.set(normalizeKey(path.resolve(baseDir, filePath)), node.className);
  }
  for (const child of node.children ?? []) {
    flatten(child, baseDir, out);
  }
};

class ScriptTypeDecorationProvider implements vscode.FileDecorationProvider {
  private readonly emitter = new vscode.EventEmitter<
    vscode.Uri | vscode.Uri[] | undefined
  >();
  readonly onDidChangeFileDecorations = this.emitter.event;

  // sourcemap.json path -> (normalized file path -> className). Keyed per-sourcemap so
  // refreshing one workspace folder never clobbers another's entries.
  private readonly bySourcemap = new Map<string, Map<string, string>>();

  provideFileDecoration(
    uri: vscode.Uri,
  ): vscode.FileDecoration | undefined {
    const key = normalizeKey(uri.fsPath);
    for (const classNames of this.bySourcemap.values()) {
      const className = classNames.get(key);
      if (className === undefined) {
        continue;
      }
      const spec = DECORATIONS[className];
      return spec
        ? { badge: spec.badge, color: spec.color, tooltip: spec.tooltip }
        : undefined;
    }
    return undefined;
  }

  async refresh(sourcemapPath: string): Promise<void> {
    let classNames = new Map<string, string>();
    try {
      const contents = await vscode.workspace.fs.readFile(
        vscode.Uri.file(sourcemapPath),
      );
      const root: SourceMapNode = JSON.parse(
        new TextDecoder().decode(contents),
      );
      flatten(root, path.dirname(sourcemapPath), classNames);
    } catch {
      // Missing/unreadable (deleted, or mid-write) - fall through with an empty map so any
      // previously-decorated files for this sourcemap get their decoration cleared below,
      // rather than keeping stale entries around.
    }

    const previous = this.bySourcemap.get(sourcemapPath);
    this.bySourcemap.set(sourcemapPath, classNames);

    const affected = new Set<string>([
      ...(previous?.keys() ?? []),
      ...classNames.keys(),
    ]);
    if (affected.size > 0) {
      this.emitter.fire(Array.from(affected, (p) => vscode.Uri.file(p)));
    }
  }

  clear(sourcemapPath: string): void {
    const previous = this.bySourcemap.get(sourcemapPath);
    this.bySourcemap.delete(sourcemapPath);
    if (previous && previous.size > 0) {
      this.emitter.fire(Array.from(previous.keys(), (p) => vscode.Uri.file(p)));
    }
  }
}

export const registerScriptTypeDecorations = (
  context: vscode.ExtensionContext,
): void => {
  const provider = new ScriptTypeDecorationProvider();
  context.subscriptions.push(
    vscode.window.registerFileDecorationProvider(provider),
  );

  const watchedSourcemapPaths = new Set<string>();

  const watchFolder = (workspaceFolder: vscode.WorkspaceFolder) => {
    const config = vscode.workspace.getConfiguration(
      "luau-lsp.sourcemap",
      workspaceFolder,
    );
    const sourcemapFileName =
      config.get<string>("sourcemapFile") ?? "sourcemap.json";
    const sourcemapUri = utils.resolveUri(
      workspaceFolder.uri,
      sourcemapFileName,
    );
    const sourcemapPath = sourcemapUri.fsPath;

    if (watchedSourcemapPaths.has(sourcemapPath)) {
      return;
    }
    watchedSourcemapPaths.add(sourcemapPath);

    const watcher = vscode.workspace.createFileSystemWatcher(
      new vscode.RelativePattern(workspaceFolder, sourcemapFileName),
    );
    watcher.onDidCreate(() => provider.refresh(sourcemapPath));
    watcher.onDidChange(() => provider.refresh(sourcemapPath));
    watcher.onDidDelete(() => provider.clear(sourcemapPath));
    context.subscriptions.push(watcher);

    // Pick up a sourcemap that already exists from a previous session/generation run.
    utils.exists(sourcemapUri).then((found) => {
      if (found) {
        provider.refresh(sourcemapPath);
      }
    });
  };

  for (const folder of vscode.workspace.workspaceFolders ?? []) {
    watchFolder(folder);
  }

  context.subscriptions.push(
    vscode.workspace.onDidChangeWorkspaceFolders((e) => {
      for (const folder of e.added) {
        watchFolder(folder);
      }
    }),
  );
};
