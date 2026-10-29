#!/usr/bin/env python3
"""Package the Android shared library as a flat LeviLauncher .levipack."""

from __future__ import annotations

import argparse
import json
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath


def read_header_value(header: Path, macro: str) -> str:
    text = header.read_text(encoding="utf-8")
    match = re.search(rf'^\s*#define\s+{re.escape(macro)}\s+"([^"]*)"\s*$', text, re.MULTILINE)
    if not match:
        raise ValueError(f"{macro} is missing from {header}")
    return match.group(1)


def safe_files(root: Path):
    if not root.exists():
        return
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Refusing to package symlink: {path}")
        if not path.is_file():
            continue
        if path.name in {".DS_Store", ".gitkeep"}:
            continue
        yield path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--icon", required=True, type=Path)
    parser.add_argument("--resources", required=True, type=Path)
    parser.add_argument("--version-header", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    library = args.library.resolve()
    icon = args.icon.resolve()
    resources = args.resources.resolve()
    header = args.version_header.resolve()
    output = args.output.resolve()

    if not library.is_file() or library.suffix != ".so":
        parser.error(f"--library must point to a built .so file: {library}")
    if not icon.is_file():
        parser.error(f"mod icon does not exist: {icon}")
    if not header.is_file():
        parser.error(f"version header does not exist: {header}")

    try:
        name = read_header_value(header, "BETTERLAUNCH_NAME")
        version = read_header_value(header, "BETTERLAUNCH_VERSION")
        author = read_header_value(header, "BETTERLAUNCH_AUTHOR")
    except (OSError, ValueError) as error:
        parser.error(str(error))

    manifest = {
        "type": "preload-native",
        "name": name,
        "author": author,
        "version": version,
        "entry": f"lib/arm64-v8a/{library.name}",
        "icon": "icon.png",
    }

    output.parent.mkdir(parents=True, exist_ok=True)
    resource_files = list(safe_files(resources))
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9) as package:
        package.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        package.write(icon, "icon.png")
        package.write(library, PurePosixPath(manifest["entry"]).as_posix())
        for resource in resource_files:
            relative = resource.relative_to(resources).as_posix()
            package.write(resource, f"resources/{relative}")

    if not (resources / "launch_animation.mp4").is_file():
        print(
            "WARNING: no MP4 was found in resources/. Add "
            "resources/launch_animation.mp4 before building for playback.",
            file=sys.stderr,
        )
    print(f"Packaged {output} ({output.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
