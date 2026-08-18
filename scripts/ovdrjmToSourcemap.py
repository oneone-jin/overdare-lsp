# Converts an OVERDARE Studio project's .ovdrjm file into a sourcemap.json compatible
# with the existing SourceNode::fromJson schema (name/className/filePaths/children),
# so the rest of the Roblox-platform pipeline can consume it unmodified.
#
# Usage: python3 ovdrjmToSourcemap.py <project.ovdrjm> <lua-folder> [-o sourcemap.json]

import argparse
import json
import os

SCRIPT_CLASSES = {"Script", "LocalScript", "ModuleScript"}


def build_tree(node, lua_dir, name_counts):
    instance_type = node["InstanceType"]
    name = node["Name"]

    file_paths = []
    if instance_type in SCRIPT_CLASSES:
        count = name_counts.get(name, 0)
        suffix = "" if count == 0 else f"_{count}"
        name_counts[name] = count + 1
        file_paths.append(os.path.join(lua_dir, f"{name}{suffix}.lua"))

    children = [build_tree(child, lua_dir, name_counts) for child in node.get("LuaChildren", [])]

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
    return build_tree(data["Root"], lua_dir, name_counts={})


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
