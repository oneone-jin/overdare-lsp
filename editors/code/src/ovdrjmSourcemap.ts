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

// Studio disambiguates scripts that share a Name within the flat Lua/ folder by appending a
// numeric suffix (Name.lua, Name_1.lua, Name_2.lua, ...). The exact suffix each node gets
// depends on Studio's own internal export order, not this .ovdrjm tree's traversal order, and
// the suffixes aren't contiguous or small - one real project had 45 nodes all named "HitData"
// backed by files suffixed _6, _12, _14..._26, _253..._285. So don't guess a suffix and probe
// for it (a bounded probe range will miss large suffixes, and - if two different guesses both
// land on the same real file - silently point two different nodes at one file, corrupting the
// require graph); instead read the directory once and hand out real files in ascending-suffix
// order, matching them to same-named nodes in traversal order.
const NAME_SUFFIX_RE = /^(.*?)(?:_(\d+))?\.lua$/;

// Groups the actual Name[_N].lua files in luaDir by base Name, each mapped to a list of real
// filenames sorted bare-name-first then by ascending numeric suffix - Studio's own numbering
// convention for a fresh duplicate. Returns an empty map if luaDir doesn't exist yet.
const indexLuaFiles = async (
  luaDir: string,
): Promise<Map<string, string[]>> => {
  const groups = new Map<string, Array<[number, string]>>();
  let entries: [string, vscode.FileType][];
  try {
    entries = await vscode.workspace.fs.readDirectory(
      vscode.Uri.file(luaDir),
    );
  } catch {
    return new Map();
  }

  for (const [filename] of entries) {
    const m = NAME_SUFFIX_RE.exec(filename);
    if (!m) {
      continue;
    }
    const base = m[1];
    const suffix = m[2] !== undefined ? parseInt(m[2], 10) : -1;
    const list = groups.get(base);
    if (list) {
      list.push([suffix, filename]);
    } else {
      groups.set(base, [[suffix, filename]]);
    }
  }

  const result = new Map<string, string[]>();
  for (const [base, items] of groups) {
    items.sort((a, b) => a[0] - b[0]);
    result.set(
      base,
      items.map(([, filename]) => filename),
    );
  }
  return result;
};

const buildTree = (
  node: OvdrjmNode,
  luaDir: string,
  fileIndex: Map<string, string[]>,
  cursors: Map<string, number>,
): SourceMapNode => {
  const filePaths: string[] = [];
  if (SCRIPT_CLASSES.has(node.InstanceType)) {
    const available = fileIndex.get(node.Name) ?? [];
    const index = cursors.get(node.Name) ?? 0;
    cursors.set(node.Name, index + 1);
    if (index < available.length) {
      filePaths.push(path.join(luaDir, available[index]));
    }
    // else: more tree nodes share this Name than there are physical .lua files for it - leave
    // filePaths empty rather than invent/reuse a path, which would either point at a file that
    // doesn't exist or silently collide with another node's real file.
  }

  const children = (node.LuaChildren ?? []).map((child) =>
    buildTree(child, luaDir, fileIndex, cursors),
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

// Byte-swaps a UTF-16BE buffer into UTF-16LE order in place, so it can be decoded with the
// same "utf-16le" TextDecoder label used for the (far more common, Windows-authored) LE case
// below - avoids depending on whether the "utf-16be" label is supported by the runtime.
const swapUtf16ByteOrder = (bytes: Uint8Array): Uint8Array => {
  const swapped = new Uint8Array(bytes.length);
  for (let i = 0; i + 1 < bytes.length; i += 2) {
    swapped[i] = bytes[i + 1];
    swapped[i + 1] = bytes[i];
  }
  return swapped;
};

// Legacy OVERDARE Studio versions wrote .ovdrjm as UTF-16LE with a BOM (Unreal's
// FFileHelper::SaveStringToFile default when saving an FString without explicitly requesting
// UTF-8) - current versions write plain UTF-8. Sniff the BOM rather than assuming an encoding,
// since both can show up on disk depending on which Studio version exported the project.
const decodeOvdrjmBytes = (bytes: Uint8Array): string => {
  if (bytes.length >= 2 && bytes[0] === 0xff && bytes[1] === 0xfe) {
    return new TextDecoder("utf-16le").decode(bytes.subarray(2));
  }
  if (bytes.length >= 2 && bytes[0] === 0xfe && bytes[1] === 0xff) {
    return new TextDecoder("utf-16le").decode(
      swapUtf16ByteOrder(bytes.subarray(2)),
    );
  }
  return new TextDecoder("utf-8").decode(bytes); // also strips a utf-8 BOM (EF BB BF) if present
};

export const convertOvdrjmToSourcemap = async (
  ovdrjmPath: string,
  luaDir: string,
): Promise<SourceMapNode> => {
  const contents = await vscode.workspace.fs.readFile(
    vscode.Uri.file(ovdrjmPath),
  );
  const data = JSON.parse(decodeOvdrjmBytes(contents));
  const fileIndex = await indexLuaFiles(luaDir);
  return buildTree(data.Root, luaDir, fileIndex, new Map());
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
