import * as vscode from "vscode";
import * as path from "path";
import { LanguageClient } from "vscode-languageclient/node";
import { AddArgCallback, PlatformContext } from "./extension";

import * as utils from "./utils";
import { watchOvdrjm } from "./ovdrjmSourcemap";
import { registerScriptTypeDecorations } from "./scriptTypeDecorations";
import overdareGlobalTypesSource from "../../../scripts/globalTypes.d.luau";

const API_DOCS = "https://luau-lsp.pages.dev/api-docs/en-us.json";
const LUAU_API_DOCS = "https://luau-lsp.pages.dev/api-docs/luau-en-us.json";

const textEncoder = new TextEncoder();

// OVERDARE's globalTypes.d.luau is inlined into the extension bundle at build time (see
// esbuild.mjs), so it's always available regardless of whether we're running from the
// monorepo source tree or a packaged vsix. We still need an on-disk copy for the language
// server to load via --definitions, so write it out (skipping the write if the content
// hasn't changed, to avoid needlessly triggering a file-watch reload).
const ensureOverdareGlobalTypesWritten = async (
  context: vscode.ExtensionContext,
) => {
  const outputUri = vscode.Uri.joinPath(
    context.globalStorageUri,
    "globalTypes.d.luau",
  );

  let existingContent: string | undefined;
  try {
    existingContent = new TextDecoder().decode(
      await vscode.workspace.fs.readFile(outputUri),
    );
  } catch {
    existingContent = undefined;
  }

  if (existingContent !== overdareGlobalTypesSource) {
    await vscode.workspace.fs.createDirectory(context.globalStorageUri);
    await vscode.workspace.fs.writeFile(
      outputUri,
      textEncoder.encode(overdareGlobalTypesSource),
    );
  }

  return outputUri;
};

const apiDocsUri = (context: vscode.ExtensionContext) => {
  return vscode.Uri.joinPath(context.globalStorageUri, "api-docs.json");
};

const luauApiDocsUri = (context: vscode.ExtensionContext) => {
  return vscode.Uri.joinPath(context.globalStorageUri, "luau-api-docs.json");
};

/// An OVERDARE Studio project root is identified by a `*.ovdrjm` file - this isn't
/// something the user configures, it's just detected in the workspace.
const findOvdrjmFile = async (
  workspaceFolder: vscode.WorkspaceFolder,
): Promise<vscode.Uri | undefined> => {
  const found = await vscode.workspace.findFiles(
    new vscode.RelativePattern(workspaceFolder, "*.ovdrjm"),
  );
  if (found.length === 0) {
    return undefined;
  }
  if (found.length > 1) {
    console.warn(
      `Multiple .ovdrjm files found in ${workspaceFolder.name}, using ${utils.basenameUri(found[0])}`,
    );
  }
  return found[0];
};

const sourcemapDisposables: Map<
  vscode.WorkspaceFolder,
  Array<vscode.Disposable>
> = new Map();

const addSourcemapDisposable = (
  workspaceFolder: vscode.WorkspaceFolder,
  disposable: vscode.Disposable,
) => {
  if (!sourcemapDisposables.get(workspaceFolder)) {
    sourcemapDisposables.set(workspaceFolder, []);
  }
  sourcemapDisposables.get(workspaceFolder)!.push(disposable);
};

const cleanupSourcemapDisposables = async (
  workspaceFolder: vscode.WorkspaceFolder,
) => {
  const disposables = sourcemapDisposables.get(workspaceFolder);
  if (disposables) {
    for (const disposable of disposables) {
      disposable.dispose();
    }
  }
  sourcemapDisposables.delete(workspaceFolder);
};

const startSourcemapGeneration = async (
  client: LanguageClient | undefined,
  workspaceFolder: vscode.WorkspaceFolder,
) => {
  cleanupSourcemapDisposables(workspaceFolder);

  const config = vscode.workspace.getConfiguration(
    "luau-lsp.sourcemap",
    workspaceFolder,
  );

  if (!config.get<boolean>("enabled") || !config.get<boolean>("autogenerate")) {
    return;
  }

  const loggingFunc = client ? client.info.bind(client) : console.log;
  loggingFunc(
    `Starting sourcemap generation for ${
      workspaceFolder.name
    } (${workspaceFolder.uri.toString(true)})`,
  );

  const ovdrjmFile = await findOvdrjmFile(workspaceFolder);
  if (!ovdrjmFile) {
    vscode.window.showWarningMessage(
      `No .ovdrjm project file found in ${workspaceFolder.name}. Open the folder containing your OVERDARE Studio project, or disable luau-lsp.sourcemap.autogenerate.`,
    );
    return;
  }

  const luaDir = path.join(path.dirname(ovdrjmFile.fsPath), "Lua");
  const sourcemapFileName =
    config.get<string>("sourcemapFile") ?? "sourcemap.json";
  const outputPath = utils.resolveUri(
    workspaceFolder.uri,
    sourcemapFileName,
  ).fsPath;

  loggingFunc(
    `Detected OVERDARE project (${utils.basenameUri(ovdrjmFile)}), using .ovdrjm-based sourcemap generation`,
  );

  addSourcemapDisposable(
    workspaceFolder,
    watchOvdrjm(ovdrjmFile.fsPath, luaDir, outputPath, loggingFunc),
  );
};

export const onActivate = async (
  platformContext: PlatformContext,
  context: vscode.ExtensionContext,
) => {
  const startSourcemapGenerationForAllFolders = () => {
    if (vscode.workspace.workspaceFolders) {
      for (const folder of vscode.workspace.workspaceFolders) {
        startSourcemapGeneration(platformContext.client, folder);
      }
    }
  };

  context.subscriptions.push(
    vscode.commands.registerCommand(
      "luau-lsp.regenerateSourcemap",
      startSourcemapGenerationForAllFolders,
    ),
  );

  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration("luau-lsp.sourcemap")) {
        if (vscode.workspace.workspaceFolders) {
          for (const folder of vscode.workspace.workspaceFolders) {
            const config = vscode.workspace.getConfiguration(
              "luau-lsp.sourcemap",
              folder,
            );

            if (
              !config.get<boolean>("enabled") ||
              !config.get<boolean>("autogenerate")
            ) {
              cleanupSourcemapDisposables(folder);
            } else {
              startSourcemapGeneration(platformContext.client, folder);
            }
          }
        }
      }
    }),
  );

  startSourcemapGenerationForAllFolders();

  registerScriptTypeDecorations(context);
};

export const preLanguageServerStart = async (
  context: vscode.ExtensionContext,
) => {
  // Load OVERDARE type definitions
  const typesConfig = vscode.workspace.getConfiguration("luau-lsp.types");
  const platformConfig = vscode.workspace.getConfiguration("luau-lsp.platform");

  // TODO: Cleanup when deprecated luau-lsp.types.roblox is deleted
  // We need to respect the new setting as well as the old setting. We check for "&&" since they are on by default
  if (
    platformConfig.get<string>("type") === "overdare" &&
    typesConfig.get<boolean>("roblox")
  ) {
    // OVERDARE's types are maintained locally in scripts/globalTypes.d.luau (merged from
    // the OVERDARE API docs by scripts/dumpOverdareTypes.py) rather than fetched from
    // luau-lsp.pages.dev, which only ever serves pure Roblox types and would silently
    // replace our merged OVERDARE definitions. There's only one unified file - the
    // Roblox-era per-security-level variants (robloxSecurityLevel) don't apply here.
    const globalTypesLocation = await ensureOverdareGlobalTypesWritten(context);
    return {
      definitions: {
        ["@overdare"]: {
          url: globalTypesLocation.fsPath,
          outputUri: globalTypesLocation,
        },
      },
      documentation: [{ url: API_DOCS, outputUri: apiDocsUri(context) }],
    };
  } else {
    return {
      definitions: undefined,
      documentation: [
        { url: LUAU_API_DOCS, outputUri: luauApiDocsUri(context) },
      ],
    };
  }
};

export const postLanguageServerStart = async (
  _platformContext: PlatformContext,
  _: vscode.ExtensionContext,
) => {};

export const onDeactivate = () => {
  return [
    ...Array.from(sourcemapDisposables.keys()).map((workspace) =>
      cleanupSourcemapDisposables(workspace),
    ),
  ];
};
