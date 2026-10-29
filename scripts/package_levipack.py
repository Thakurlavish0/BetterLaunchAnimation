#!/usr/bin/env python3
"""Package the Android native mod using LeviLauncher Preloader's .levipack layout."""

from __future__ import annotations

import argparse
import json
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath


def read_header_value(header: Path, macro: str) -> str:
    text = header.read_text(encoding="utf-8")
    match = re.search(
        rf'^\s*#define\s+{re.escape(macro)}\s+"([^"]*)"\s*$',
        text,
        re.MULTILINE,
    )
    if not match:
        raise ValueError(f"{macro} is missing from {header}")
    return match.group(1)


def safe_files(root: Path):
    if not root.exists():
        return
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Refusing to package symlink: {path}")
        if path.is_file() and path.name not in {".DS_Store", ".gitkeep"}:
            yield path


def verify_mp4(resources: Path) -> bool:
    video = resources / "launch_animation.mp4"
    if not video.exists():
        return False
    if not video.is_file():
        raise ValueError(f"Animation path is not a file: {video}")
    with video.open("rb") as stream:
        header = stream.read(12)
    if len(header) < 12 or header[4:8] != b"ftyp":
        raise ValueError(
            f"{video} is not a valid MP4 container (expected an 'ftyp' box). "
            "Replace it with the actual animation video."
        )
    return True


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
        has_animation = verify_mp4(resources)
        name = read_header_value(header, "BETTERLAUNCH_NAME")
        version = read_header_value(header, "BETTERLAUNCH_VERSION")
        author = read_header_value(header, "BETTERLAUNCH_AUTHOR")
        description = read_header_value(header, "BETTERLAUNCH_DESCRIPTION")
        resource_files = list(safe_files(resources))
    except (OSError, ValueError) as error:
        parser.error(str(error))

    entry = library.name
    archive_files = ["icon.png"] + [
        f"resources/{path.relative_to(resources).as_posix()}"
        for path in resource_files
    ]
    manifest = {
        "type": "preload-native",
        "name": name,
        "author": author,
        "description": description,
        "version": version,
        "entry": entry,
        "icon": "icon.png",
        "overwrite_files": archive_files,
        "overwrite_folders": [],
    }

    output.parent.mkdir(parents=True, exist_ok=True)
    expected_entries = {"manifest.json", "icon.png", entry, *archive_files[1:]}
    with zipfile.ZipFile(
        output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as package:
        package.writestr(
            "manifest.json", json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
        )
        package.write(icon, "icon.png")
        package.write(library, entry)
        for resource in resource_files:
            archive_path = PurePosixPath(
                "resources", resource.relative_to(resources).as_posix()
            ).as_posix()
            package.write(resource, archive_path)

    with zipfile.ZipFile(output, "r") as package:
        actual_entries = set(package.namelist())
        if actual_entries != expected_entries:
            raise RuntimeError(
                f"Unexpected .levipack entries: {sorted(actual_entries)}"
            )
        parsed_manifest = json.loads(package.read("manifest.json"))
        if parsed_manifest != manifest:
            raise RuntimeError(".levipack manifest verification failed")
        if package.getinfo(entry).file_size != library.stat().st_size:
            raise RuntimeError("Packaged native library verification failed")
        if package.getinfo("icon.png").file_size != icon.stat().st_size:
            raise RuntimeError("Packaged icon verification failed")

    if not has_animation:
        print(
            "WARNING: resources/launch_animation.mp4 is missing; the mod will "
            "fall back to Minecraft's normal launch animation.",
            file=sys.stderr,
        )
    print(f"Packaged {output} ({output.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
