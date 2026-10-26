# SPDX-License-Identifier: Apache-2.0
"""Check that Diligent headers are only used inside engine/render/rhi/src.

Phases.md P1.1: nothing outside render/rhi includes a Diligent header, and the
RHI's public headers (engine/render/rhi/include) must not either.

A file fails if it mentions the `Diligent` namespace or includes a header whose
file name matches one from DiligentCore's / DiligentTools' interface folders.
Pass --diligent-dir <dir containing DiligentCore> (e.g. build/<preset>/_deps) to
check header names; without it only the namespace check runs.

The throw-away spikes/ directory and cmake/ are exempt.
"""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ALLOWED = ROOT / "engine" / "render" / "rhi" / "src"
EXEMPT_DIRS = {"spikes", "cmake", "build", "out", ".git", "docs", "_deps", "vcpkg_installed"}
SOURCE_EXT = {".c", ".cpp", ".h", ".hpp", ".inl", ".hlsl", ".hlsli"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
NAMESPACE_RE = re.compile(r"\bDiligent\s*::|namespace\s+Diligent\b")


def diligent_header_names(diligent_dir):
    names = set()
    for repo in ("DiligentCore", "DiligentTools"):
        base = diligent_dir / repo
        if base.is_dir():
            for p in base.rglob("interface/*"):
                if p.suffix in {".h", ".hpp"}:
                    names.add(p.name)
    return names


def files():
    try:
        out = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard"], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout.splitlines()
        paths = [ROOT / p for p in out]
    except (OSError, subprocess.CalledProcessError):
        paths = list(ROOT.rglob("*"))
    for p in paths:
        rel = p.relative_to(ROOT)
        if p.suffix in SOURCE_EXT and p.is_file() and not EXEMPT_DIRS.intersection(rel.parts):
            yield p


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--diligent-dir", type=pathlib.Path, help="directory containing DiligentCore/")
    args = parser.parse_args()
    headers = diligent_header_names(args.diligent_dir) if args.diligent_dir else set()
    if args.diligent_dir and not headers:
        print(f"warning: no Diligent headers found under {args.diligent_dir}", file=sys.stderr)

    problems = []
    checked = 0
    for path in files():
        if ALLOWED in path.parents:
            continue
        checked += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = path.relative_to(ROOT).as_posix()
        if NAMESPACE_RE.search(text):
            problems.append(f"{rel}: uses the Diligent namespace")
        for inc in INCLUDE_RE.findall(text):
            name = pathlib.PurePosixPath(inc).name
            if name in headers or "Diligent" in inc:
                problems.append(f"{rel}: includes Diligent header '{inc}'")
    for p in problems:
        print(p)
    print(f"check_diligent_includes: {checked} files checked, {len(headers)} Diligent header names, "
          f"{len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
