# Watches an OVERDARE Studio project's .ovdrjm file and regenerates sourcemap.json
# whenever it changes (Studio rewrites .ovdrjm on every save, play, and publish).
# The LSP already watches sourcemap.json for changes and reloads automatically, so
# this script is the only piece needed to keep it in sync with the live project.
#
# Usage: python3 ovdrjmWatch.py <project.ovdrjm> <lua-folder> [-o sourcemap.json] [--interval 0.5]

import argparse
import json
import os
import sys
import time

from ovdrjmToSourcemap import convert

# Studio writes the file non-atomically; a poll can land mid-write and see truncated JSON.
READ_RETRY_ATTEMPTS = 5
READ_RETRY_DELAY_SECONDS = 0.1


def convert_with_retry(ovdrjm_path, lua_dir):
    for attempt in range(READ_RETRY_ATTEMPTS):
        try:
            return convert(ovdrjm_path, lua_dir)
        except (json.JSONDecodeError, OSError):
            if attempt == READ_RETRY_ATTEMPTS - 1:
                raise
            time.sleep(READ_RETRY_DELAY_SECONDS)


def watch(ovdrjm_path, lua_dir, output_path, interval_seconds):
    last_mtime = None
    print(f"Watching {ovdrjm_path} (every {interval_seconds}s) -> {output_path}")

    while True:
        try:
            mtime = os.path.getmtime(ovdrjm_path)
        except OSError as e:
            print(f"[warn] could not stat {ovdrjm_path}: {e}", file=sys.stderr)
            time.sleep(interval_seconds)
            continue

        if mtime != last_mtime:
            try:
                tree = convert_with_retry(ovdrjm_path, lua_dir)
                with open(output_path, "w", encoding="utf-8") as f:
                    json.dump(tree, f, indent=2)
                last_mtime = mtime
                print(f"[ok] regenerated {output_path} ({time.strftime('%H:%M:%S')})")
            except (json.JSONDecodeError, OSError) as e:
                print(f"[warn] failed to convert {ovdrjm_path}: {e}", file=sys.stderr)
                # don't update last_mtime - retry on the next poll even if mtime is unchanged

        time.sleep(interval_seconds)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ovdrjm", help="Path to the .ovdrjm project file")
    parser.add_argument("lua_dir", help="Path to the flat Lua/ folder (used to build filePaths)")
    parser.add_argument("-o", "--output", default="sourcemap.json")
    parser.add_argument("--interval", type=float, default=0.5, help="Poll interval in seconds")
    args = parser.parse_args()

    try:
        watch(args.ovdrjm, args.lua_dir, args.output, args.interval)
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
