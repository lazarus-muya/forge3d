# SPDX-License-Identifier: Apache-2.0
"""Check that every forge3d source file starts with the SPDX licence header.

C/C++/HLSL/GLSL files need `// SPDX-License-Identifier: Apache-2.0`; Python
and CMake files need the `#` form. The header must be in the first 3 lines
(after a shebang, if any). Exit code 1 lists the offending files.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SPDX = "SPDX-License-Identifier: Apache-2.0"
SLASH = {".c", ".cpp", ".h", ".hpp", ".inl", ".hlsl", ".hlsli", ".fx", ".glsl", ".comp", ".vert", ".frag"}
HASH = {".py", ".cmake"}
HASH_NAMES = {"CMakeLists.txt"}
CMD = {".cmd", ".bat"}
# Third-party code, generated output and test data keep their own headers.
SKIP_DIRS = {".git", "build", "out", "vcpkg_installed", "_deps", "third_party", "data"}


def candidate_files():
    try:
        listed = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard"],
                                cwd=ROOT, capture_output=True, text=True, check=True).stdout.splitlines()
        paths = [ROOT / p for p in listed]
    except (OSError, subprocess.CalledProcessError):
        paths = [p for p in ROOT.rglob("*") if p.is_file()]
    for p in paths:
        rel = p.relative_to(ROOT)
        if SKIP_DIRS.intersection(rel.parts[:-1]) or not p.is_file():
            continue
        if p.suffix in SLASH or p.suffix in HASH or p.name in HASH_NAMES or p.suffix in CMD:
            yield p


def expected_prefix(p):
    if p.suffix in SLASH:
        return "// " + SPDX
    if p.suffix in CMD:
        return "rem " + SPDX
    return "# " + SPDX


def has_header(p):
    prefix = expected_prefix(p)
    try:
        lines = p.read_text(encoding="utf-8").splitlines()[:4]
    except UnicodeDecodeError:
        return False
    if lines and lines[0].startswith("#!"):
        lines = lines[1:]
    return any(line.strip().lower() == prefix.lower() for line in lines[:3])


def main():
    files = list(candidate_files())
    missing = [p for p in files if not has_header(p)]
    for p in missing:
        print(f"missing licence header: {p.relative_to(ROOT).as_posix()}  (expected '{expected_prefix(p)}')")
    print(f"check_license_headers: {len(files) - len(missing)}/{len(files)} files OK")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
