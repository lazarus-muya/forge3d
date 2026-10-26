# SPDX-License-Identifier: Apache-2.0
"""Checks that a .forge file is a standard ZIP archive, using Python's zipfile.

    python tools/check_forge_zip.py tests/data/forge/v1_basic.forge

An independent reader for forge3d's own archive code: the central directory
parses, manifest.json (stored) is valid JSON naming every chunk, and every entry's
CRC verifies. zstd entries (ZIP method 93) are only decompressed where zipfile
supports them (Python 3.14+); older Pythons check them by listing only.
"""
import json
import sys
import zipfile

ZSTD = 93


def main(path):
    zstd_ok = hasattr(zipfile, "ZIP_ZSTANDARD")
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if "manifest.json" not in names:
            print(f"{path}: no manifest.json", file=sys.stderr)
            return 1
        manifest = json.loads(archive.read("manifest.json"))
        if manifest.get("format") != "forge3d.project":
            print(f"{path}: manifest format is {manifest.get('format')!r}", file=sys.stderr)
            return 1
        for row in manifest.get("chunks", []):
            if row["name"] not in names:
                print(f"{path}: chunk {row['name']} listed but missing", file=sys.stderr)
                return 1
        for info in archive.infolist():
            if info.compress_type not in (zipfile.ZIP_STORED, ZSTD):
                print(f"{path}: {info.filename} uses method {info.compress_type}", file=sys.stderr)
                return 1
            if info.compress_type == ZSTD and not zstd_ok:
                continue
            data = archive.read(info)  # raises BadZipFile on a CRC mismatch
            if len(data) != info.file_size:
                print(f"{path}: {info.filename} has the wrong size", file=sys.stderr)
                return 1
    print(f"{path}: OK ({len(names)} entries{'' if zstd_ok else ', zstd entries listed only'})")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
