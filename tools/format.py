# SPDX-License-Identifier: Apache-2.0
"""Run clang-format over forge3d's C++ sources.

    python tools/format.py           # reformat in place
    python tools/format.py --check   # fail if any file needs formatting (CI)
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE_DIRS = ["engine", "editor", "tools", "tests", "spikes", "modules", "samples"]
EXTENSIONS = {".cpp", ".hpp", ".h", ".c", ".inl"}
SKIP_PARTS = {"build", "out", "vcpkg_installed", "data", "_deps"}


def sources():
    for d in SOURCE_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if p.suffix in EXTENSIONS and p.is_file() and not SKIP_PARTS.intersection(p.relative_to(ROOT).parts):
                yield p


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="only check, don't modify files")
    parser.add_argument("--clang-format", default="clang-format", help="clang-format executable")
    args = parser.parse_args()

    exe = shutil.which(args.clang_format)
    if not exe:
        print(f"error: {args.clang_format} not found (pip install clang-format==21.1.8)", file=sys.stderr)
        return 2
    files = [str(p) for p in sources()]
    mode = ["--dry-run", "--Werror"] if args.check else ["-i"]
    failed = False
    for i in range(0, len(files), 100):  # keep command lines short on Windows
        if subprocess.run([exe, *mode, *files[i:i + 100]]).returncode != 0:
            failed = True
    if args.check:
        print("clang-format: " + ("FAILED" if failed else f"{len(files)} files OK"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
