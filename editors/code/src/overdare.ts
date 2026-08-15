import * as vscode from "vscode";
import * as path from "path";
import { spawn } from "child_process";
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

const getRojoProjectFile = async (
  workspaceFolder: vscode.WorkspaceFolder,
  config: vscode.WorkspaceConfiguration,
  client: LanguageClient | undefined,
) => {
  let projectFile =
    config.get<string>("rojoProjectFile") ?? "default.project.json";
  const projectFileUri = utils.resolveUri(workspaceFolder.uri, projectFile);

  if (await utils.exists(projectFileUri)) {
    return projectFile;
  }

  // Search if there is a *.project.json file present in this workspace.
  const foundProjectFiles = await vscode.workspace.findFiles(
    new vscode.RelativePattern(workspaceFolder.uri, "*.project.json"),
  );

  if (foundProjectFiles.length === 0) {
    vscode.window
      .showWarningMessage(
        `Unable to find project file ${projectFile} for Rojo sourcemap generation. Configure a file in settings.`,
        "Configure Settings",
      )
      .then((value) => {
        if (value === "Configure Settings") {
          vscode.commands.executeCommand(
            "workbench.action.openWorkspaceSettings",
            "luau-lsp.sourcemap",
          );
        }
      });
    return undefined;
  } else if (foundProjectFiles.length === 1) {
    const fileName = utils.basenameUri(foundProjectFiles[0]);
    const option = await vscode.window.showWarningMessage(
      `Unable to find project file ${projectFile} for Rojo sourcemap generation. We found ${fileName} available`,
      `Set project file to ${fileName}`,
      "Cancel",
    );

    if (option === `Set project file to ${fileName}`) {
      config.update("rojoProjectFile", fileName);
      return fileName;
    } else {
      return undefined;
    }
  } else {
    const option = await vscode.window.showWarningMessage(
      `Unable to find project file ${projectFile} for Rojo sourcemap generation. We found ${foundProjectFiles.length} files available`,
      "Select project file",
      "Cancel",
    );
    if (option === "Select project file") {
      const files = foundProjectFiles.map((file) => utils.basenameUri(file));
      const selectedFile = await vscode.window.showQuickPick(files);
      if (selectedFile) {
        config.update("rojoProjectFile", selectedFile);
        selectedFile;
      } else {
        return undefined;
      }
    } else {
      return undefined;
    }
  }

  return undefined;
};

/// An OVERDARE Studio project root is identified by a `*.ovdrjm` file. Unlike Rojo
/// projects, this isn't something the user configures - if it's present, this
/// workspace is an OVERDARE project and .ovdrjm-based sourcemap generation takes
/// priority over the Rojo flow.
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
  context: vscode.ExtensionContext,
) => {
  cleanupSourcemapDisposables(workspaceFolder);

  const config = vscode.workspace.getConfiguration(
    "luau-lsp.sourcemap",
    workspaceFolder,
  );

  if (!config.get<boolean>("enabled") || !config.get<boolean>("autogenerate")) {
    return;
  }

  const customGeneratorCommand = config.get<string>("generatorCommand");
  const useVSCodeWatcher = config.get<boolean>("useVSCodeWatcher") ?? false;

  const loggingFunc = client ? client.info.bind(client) : console.log;
  loggingFunc(
    `Starting sourcemap generation for ${
      workspaceFolder.name
    } (${workspaceFolder.uri.toString(true)})`,
  );

  const cwd = workspaceFolder.uri.fsPath;

  // An OVERDARE project is identified by a *.ovdrjm file - if present, generate the
  // sourcemap in-process (ovdrjmSourcemap.ts) rather than falling through to the
  // Rojo-oriented child-process flow below, which doesn't apply here.
  if (!customGeneratorCommand) {
    const ovdrjmFile = await findOvdrjmFile(workspaceFolder);
    if (ovdrjmFile) {
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
      return;
    }
  }

  const spawnChildProcess = async () => {
    loggingFunc(
      `Spawning sourcemap generator for ${
        workspaceFolder.name
      } (${workspaceFolder.uri.toString(true)})`,
    );

    let childProcess;

    if (customGeneratorCommand && customGeneratorCommand.trim() !== "") {
      // TODO: should we support shell execution here?
      // It allows us to delegate to the shell for argument parsing
      // but it causes issues when VSCode shuts down, leaving a zombie process
      childProcess = spawn(customGeneratorCommand, {
        cwd,
        shell: true,
      });
    } else {
      // Check if the project file exists
      const projectFile = await getRojoProjectFile(
        workspaceFolder,
        config,
        client,
      );
      if (!projectFile) {
        return;
      }
      const rojoPath = config.get<string>("rojoPath") ?? "rojo";
      const sourcemapFileName =
        config.get<string>("sourcemapFile") ?? "sourcemap.json";
      const args = ["sourcemap", projectFile, "--output", sourcemapFileName];

      if (config.get<boolean>("includeNonScripts")) {
        args.push("--include-non-scripts");
      }

      if (!useVSCodeWatcher) {
        args.push("--watch");
      }

      childProcess = spawn(rojoPath, args, { cwd });
    }

    let stderr = "";
    childProcess.stderr.on("data", (data) => {
      stderr += data;
    });

    childProcess.on("error", (err) => {
      stderr += err.message;
    });

    childProcess.on("close", (code, signal) => {
      if (childProcess.killed) {
        return;
      }
      if (code !== 0) {
        let output = `Failed to update sourcemap for ${workspaceFolder.name}: `;
        let options = ["Retry"];

        if (customGeneratorCommand) {
          output += stderr;
          if (stderr === "") {
            output += "<no output>";
          }
          options.push("Configure Settings");
        } else {
          if (
            stderr.includes("Found argument 'sourcemap' which wasn't expected")
          ) {
            output +=
              "Your Rojo version doesn't have sourcemap support. Upgrade to Rojo v7.3.0+";
          } else if (
            stderr.includes("Found argument '--watch' which wasn't expected")
          ) {
            output +=
              "Your Rojo version doesn't have sourcemap watching support. Upgrade to Rojo v7.3.0+";
          } else if (
            stderr.includes("is not recognized") ||
            stderr.includes("ENOENT")
          ) {
            output += "Rojo not found. Configure your Rojo path in settings.";
            options.push("Configure Settings");
          } else {
            output += stderr;
          }
        }

        vscode.window.showWarningMessage(output, ...options).then((value) => {
          if (value === "Retry") {
            startSourcemapGeneration(client, workspaceFolder, context);
          } else if (value === "Configure Settings") {
            vscode.commands.executeCommand(
              "workbench.action.openWorkspaceSettings",
              "luau-lsp.sourcemap",
            );
          }
        });
      }
    });

    return childProcess;
  };

  if (useVSCodeWatcher) {
    spawnChildProcess();

    const watcher = vscode.workspace.createFileSystemWatcher(
      new vscode.RelativePattern(workspaceFolder, "**/*.{lua,luau}"),
      /* ignoreCreateEvents = */ false,
      /* ignoreChangeEvents = */ true,
      /* ignoreDeleteEvents = */ false,
    );

    let debounceTimer: NodeJS.Timeout;
    watcher.onDidCreate(() => {
      clearTimeout(debounceTimer);
      debounceTimer = setTimeout(spawnChildProcess, 1000);
    });
    watcher.onDidDelete(() => {
      clearTimeout(debounceTimer);
      debounceTimer = setTimeout(spawnChildProcess, 1000);
    });

    addSourcemapDisposable(workspaceFolder, watcher);
  } else {
    const childProcess = await spawnChildProcess();
    if (childProcess) {
      childProcess.on("close", (code) => {
        cleanupSourcemapDisposables(workspaceFolder);

        if (code === 0) {
          vscode.window
            .showWarningMessage(
              "Sourcemap generator ended. No further updates will be tracked. If the generator does not support file watching, enable luau-lsp.sourcemap.useVSCodeWatcher",
              "Restart",
              "Configure Settings",
            )
            .then((value) => {
              if (value === "Restart") {
                startSourcemapGeneration(client, workspaceFolder, context);
              } else if (value === "Configure Settings") {
                vscode.commands.executeCommand(
                  "workbench.action.openWorkspaceSettings",
                  "luau-lsp.sourcemap",
                );
              }
            });
        }
      });
      addSourcemapDisposable(
        workspaceFolder,
        new vscode.Disposable(() => {
          if (childProcess.killed) {
            return;
          }
          childProcess.kill();
        }),
      );
    }
  }
};

export const onActivate = async (
  platformContext: PlatformContext,
  context: vscode.ExtensionContext,
) => {
  const startSourcemapGenerationForAllFolders = () => {
    if (vscode.workspace.workspaceFolders) {
      for (const folder of vscode.workspace.workspaceFolders) {
        startSourcemapGeneration(platformContext.client, folder, context);
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
              startSourcemapGeneration(platformContext.client, folder, context);
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
