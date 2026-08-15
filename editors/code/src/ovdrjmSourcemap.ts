import * as vscode from "vscode";
import * as path from "path";

// TypeScript port of scripts/ovdrjmToSourcemap.py / ovdrjmWatch.py. Converts an OVERDARE
// Studio project's .ovdrjm file into a sourcemap.json compatible with the existing
// SourceNode::fromJson schema (name/className/filePaths/children). Ported to TS (rather
// than shelling out to python3) so it works identically in dev and in a packaged vsix,
// and doesn't require python3 on the user's PATH.

const SCRIPT_CLASSES = new Set(["Script", "LocalScript", "ModuleScript"]);

interface OvdrjmNode {
  InstanceType: string;
  Name: string;
  ActorGuid?: string;
  ObjectKey?: number;
  LuaChildren?: OvdrjmNode[];
}

interface SourceMapNode {
  name: string;
  className: string;
  filePaths: string[];
  children: SourceMapNode[];
  // Kept for downstream lookups back into the .ovdrjm - not read by SourceNode::fromJson.
  actorGuid?: string;
  objectKey?: number;
}

const buildTree = (
  node: OvdrjmNode,
  luaDir: string,
  nameCounts: Map<string, number>,
): SourceMapNode => {
  const filePaths: string[] = [];
  if (SCRIPT_CLASSES.has(node.InstanceType)) {
    const count = nameCounts.get(node.Name) ?? 0;
    const suffix = count === 0 ? "" : `_${count}`;
    nameCounts.set(node.Name, count + 1);
    filePaths.push(path.join(luaDir, `${node.Name}${suffix}.lua`));
  }

  const children = (node.LuaChildren ?? []).map((child) =>
    buildTree(child, luaDir, nameCounts),
  );

  return {
    name: node.Name,
    className: node.InstanceType,
    filePaths,
    children,
    actorGuid: node.ActorGuid,
    objectKey: node.ObjectKey,
  };
};

export const convertOvdrjmToSourcemap = async (
  ovdrjmPath: string,
  luaDir: string,
): Promise<SourceMapNode> => {
  const contents = await vscode.workspace.fs.readFile(
    vscode.Uri.file(ovdrjmPath),
  );
  const data = JSON.parse(new TextDecoder().decode(contents));
  return buildTree(data.Root, luaDir, new Map());
};

const READ_RETRY_ATTEMPTS = 5;
const READ_RETRY_DELAY_MS = 100;

// Studio writes .ovdrjm non-atomically; a poll can land mid-write and see truncated JSON.
const convertWithRetry = async (ovdrjmPath: string, luaDir: string) => {
  for (let attempt = 0; attempt < READ_RETRY_ATTEMPTS; attempt++) {
    try {
      return await convertOvdrjmToSourcemap(ovdrjmPath, luaDir);
    } catch (err) {
      if (attempt === READ_RETRY_ATTEMPTS - 1) {
        throw err;
      }
      await new Promise((resolve) =>
        setTimeout(resolve, READ_RETRY_DELAY_MS),
      );
    }
  }
  throw new Error("unreachable");
};

/**
 * Watches an OVERDARE Studio project's .ovdrjm file and regenerates the sourcemap
 * whenever it changes (Studio rewrites .ovdrjm on every save, play, and publish).
 * Returns a Disposable that stops the watcher.
 */
export const watchOvdrjm = (
  ovdrjmPath: string,
  luaDir: string,
  outputPath: string,
  log: (message: string) => void,
  intervalMs = 500,
): vscode.Disposable => {
  let lastMtimeMs: number | undefined;
  let stopped = false;

  const tick = async () => {
    if (stopped) {
      return;
    }

    let mtimeMs: number;
    try {
      const stat = await vscode.workspace.fs.stat(
        vscode.Uri.file(ovdrjmPath),
      );
      mtimeMs = stat.mtime;
    } catch (err) {
      log(`[warn] could not stat ${ovdrjmPath}: ${err}`);
      scheduleNext();
      return;
    }

    if (mtimeMs !== lastMtimeMs) {
      try {
        const tree = await convertWithRetry(ovdrjmPath, luaDir);
        await vscode.workspace.fs.writeFile(
          vscode.Uri.file(outputPath),
          new TextEncoder().encode(JSON.stringify(tree, null, 2)),
        );
        lastMtimeMs = mtimeMs;
        log(`[ok] regenerated ${outputPath}`);
      } catch (err) {
        log(`[warn] failed to convert ${ovdrjmPath}: ${err}`);
        // don't update lastMtimeMs - retry on the next tick even if mtime is unchanged
      }
    }

    scheduleNext();
  };

  const scheduleNext = () => {
    if (!stopped) {
      timer = setTimeout(tick, intervalMs);
    }
  };

  let timer: NodeJS.Timeout;
  log(`Watching ${ovdrjmPath} (every ${intervalMs}ms) -> ${outputPath}`);
  tick();

  return new vscode.Disposable(() => {
    stopped = true;
    clearTimeout(timer);
  });
};
