# Converts an OVERDARE Studio project's .ovdrjm file into a sourcemap.json compatible
# with the existing SourceNode::fromJson schema (name/className/filePaths/children),
# so the rest of the Roblox-platform pipeline can consume it unmodified.
#
# Usage: python3 ovdrjmToSourcemap.py <project.ovdrjm> <lua-folder> [-o sourcemap.json]

import argparse
import json
import os
import re

SCRIPT_CLASSES = {"Script", "LocalScript", "ModuleScript"}

# Studio disambiguates scripts that share a Name within the flat Lua/ folder by appending a
# numeric suffix (Name.lua, Name_1.lua, Name_2.lua, ...). The exact suffix each node gets
# depends on Studio's own internal export order, not this .ovdrjm tree's traversal order, and
# the suffixes aren't contiguous or small - one real project had 45 nodes all named "HitData"
# backed by files suffixed _6, _12, _14..._26, _253..._285. So don't guess a suffix and probe
# for it (a bounded probe range will miss large suffixes, and - if two different guesses both
# land on the same real file - silently point two different nodes at one file, corrupting the
# require graph); instead read the directory once and hand out real files in ascending-suffix
# order, matching them to same-named nodes in traversal order.
NAME_SUFFIX_RE = re.compile(r"^(.*?)(?:_(\d+))?\.lua$")


def index_lua_files(lua_dir):
    """Groups the actual Name[_N].lua files in lua_dir by base Name, each mapped to a list of
    real filenames sorted bare-name-first then by ascending numeric suffix - Studio's own
    numbering convention for a fresh duplicate. Returns {} if lua_dir doesn't exist yet."""
    groups = {}
    try:
        entries = os.listdir(lua_dir)
    except OSError:
        return groups
    for entry in entries:
        m = NAME_SUFFIX_RE.match(entry)
        if not m:
            continue
        base, suffix = m.group(1), m.group(2)
        groups.setdefault(base, []).append((int(suffix) if suffix is not None else -1, entry))
    for base, items in groups.items():
        items.sort(key=lambda pair: pair[0])
        groups[base] = [filename for _, filename in items]
    return groups


def build_tree(node, lua_dir, file_index, cursors):
    instance_type = node["InstanceType"]
    name = node["Name"]

    file_paths = []
    if instance_type in SCRIPT_CLASSES:
        available = file_index.get(name, [])
        index = cursors.get(name, 0)
        cursors[name] = index + 1
        if index < len(available):
            file_paths.append(os.path.join(lua_dir, available[index]))
        # else: more tree nodes share this Name than there are physical .lua files for it -
        # leave file_paths empty rather than invent/reuse a path, which would either point at
        # a file that doesn't exist or silently collide with another node's real file.

    children = [build_tree(child, lua_dir, file_index, cursors) for child in node.get("LuaChildren", [])]

    return {
        "name": name,
        "className": instance_type,
        "filePaths": file_paths,
        "children": children,
        # kept for downstream lookups back into the .ovdrjm (not read by SourceNode::fromJson)
        "actorGuid": node.get("ActorGuid"),
        "objectKey": node.get("ObjectKey"),
    }


def read_ovdrjm_text(ovdrjm_path):
    """Legacy OVERDARE Studio versions wrote .ovdrjm as UTF-16LE with a BOM (Unreal's
    FFileHelper::SaveStringToFile default when saving an FString without explicitly requesting
    UTF-8) - current versions write plain UTF-8. Sniff the BOM rather than assuming an
    encoding, since both can show up on disk depending on which Studio version exported the
    project."""
    with open(ovdrjm_path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16")
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw.decode("utf-8-sig")
    return raw.decode("utf-8")


def convert(ovdrjm_path, lua_dir):
    data = json.loads(read_ovdrjm_text(ovdrjm_path))
    return build_tree(data["Root"], lua_dir, index_lua_files(lua_dir), cursors={})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ovdrjm", help="Path to the .ovdrjm project file")
    parser.add_argument("lua_dir", help="Path to the flat Lua/ folder (used to build filePaths)")
    parser.add_argument("-o", "--output", default="sourcemap.json")
    args = parser.parse_args()

    tree = convert(args.ovdrjm, args.lua_dir)

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(tree, f, indent=2)

    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
