#!/usr/bin/env python3
"""Check that every public C++ header compiles standalone.

Gate B (stabilization) regression mechanism for the header self-containment
defects S1/S2: a public header must compile from a translation unit that
includes only that header (plus an empty main), using only its own explicitly
declared dependencies. Headers that rely on transitive includes or on the
includer's include order fail here even when the full build passes — which is
exactly how the ps2_memory.h <string> defect (S1) shipped: GCC's <iostream>
supplies <string> transitively, MSVC's header graph does not.

Two checks run per header:

  1. Standalone compilation: `#include "<header>"` + `int main(){return 0;}`
     under the requested compiler and C++ standard, with the union of all
     public include dirs plus the dependency cache on the path. (The union
     mirrors how consumers build: a header's declared #includes must resolve
     without help from another header included earlier in the same TU.)

  2. Direct standard-include audit (targeted, not exhaustive): for a small
     curated mapping of standard-library symbol -> owning header, a header
     that uses the symbol in its own code must directly include that header.
     This is the check that catches S1 under GCC, where check 1 passes via
     transitive <iostream> -> <string>. The mapping is deliberately small:
     extending it is welcome, but each new pair must be verified not to flag
     headers that legitimately obtain the symbol through a project header
     they already include (that would be a contract change, not a bug fix).

Build-gated headers: gs_parallel_backend.h is a compile error by design
unless PS2X_HAS_PARALLEL_GS=1 (see the header comment); the script passes
-DPS2X_HAS_PARALLEL_GS=1 for that header only, i.e. it checks the header in
its supported (adapter-enabled) configuration.

Usage:
    python3 tools/check_public_headers.py [--repo ROOT] [--deps DIR]
        [--compiler CXX] [--std c++20] [--jobs N]

  --repo      Repository root (default: parent of this script's directory).
  --deps      Dependency cache dir (default: $ZMBOR_DEPS_DIR, else
              ~/workspace/zmbor-deps if it exists, else none — headers that
              need dependency headers then fail loudly, which is correct).
              Two layouts are accepted per dependency: <deps>/<name> (the
              local anonymous-clone cache) and <deps>/<name>-src (a CMake
              FetchContent _deps dir after configure, so CI can run this
              script with --deps <build>/_deps).
  --compiler  Compiler binary (default: $CXX, else g++).
  --std       C++ standard flag (default: c++20).
  --jobs      Parallel compile jobs (default: 4).

Exit code 0 iff every header passes both checks. CI wiring of this script
into build.yml is Gate F work; this script is the check itself.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

MODULES = ["ps2xRuntime", "ps2xRecomp", "ps2xAnalyzer", "ps2xIOP"]
DEP_NAMES = ["elfio", "toml11", "fmt", "nlohmann_json", "libdwarf",
             "rabbitizer", "raylib", "parallel-gs"]

# Curated symbol -> required direct include (check 2). Keep this list short
# and proven: each pair must correspond to a real defect class, not a guess.
DIRECT_INCLUDE_MAP = [
    (re.compile(r"\bstd::(?:string|to_string)\b"), "<string>"),
    (re.compile(r"\bsize_t\b"), "<cstddef>"),
]

# Headers compiled with an extra -D (build-gated headers only).
EXTRA_DEFINES = {
    "runtime/gs/gs_parallel_backend.h": ["-DPS2X_HAS_PARALLEL_GS=1"],
}


def strip_comments(text):
    """Remove // and /* */ comments (best-effort; headers here are simple)."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    lines = []
    for line in text.splitlines():
        idx = line.find("//")
        if idx != -1:
            line = line[:idx]
        lines.append(line)
    return "\n".join(lines)


def direct_includes(text):
    incs = set()
    for m in re.finditer(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]',
                         text, flags=re.M):
        incs.add(m.group(1))
    return incs


def check_header(args, header_path, rel, include_flags):
    """Return (rel, ok, detail)."""
    with open(header_path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    code = strip_comments(text)
    # Code lines exclude preprocessor directives: a symbol mentioned only in
    # an #include line or a macro definition header is not a use.
    code_lines = [ln for ln in code.splitlines()
                  if not ln.lstrip().startswith("#")]
    code_only = "\n".join(code_lines)
    problems = []

    # Check 2: direct standard includes.
    incs = direct_includes(text)
    for pattern, required in DIRECT_INCLUDE_MAP:
        if pattern.search(code_only):
            bare = required.strip("<>")
            if required not in incs and bare not in incs:
                problems.append(
                    "uses %s but does not directly include %s"
                    % (pattern.pattern, required))

    # Check 1: standalone compilation.
    with tempfile.NamedTemporaryFile("w", suffix=".cpp",
                                     delete=False) as tu:
        tu.write('#include "%s"\nint main(){return 0;}\n' % rel)
        tu_path = tu.name
    try:
        cmd = ([args.compiler, "-std=" + args.std, "-fsyntax-only"]
               + include_flags
               + EXTRA_DEFINES.get(rel, [])
               + [tu_path])
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            first = [ln for ln in (proc.stderr or "").splitlines()
                     if "error" in ln][:3]
            problems.append("standalone compile failed: "
                            + ("; ".join(first) if first else "see stderr"))
    finally:
        os.unlink(tu_path)

    if problems:
        return (rel, False, "; ".join(problems))
    return (rel, True, "")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=None)
    ap.add_argument("--deps", default=None)
    ap.add_argument("--compiler", default=None)
    ap.add_argument("--std", default="c++20")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    repo = args.repo or os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))
    compiler = args.compiler or os.environ.get("CXX", "g++")
    args.compiler = compiler

    include_dirs = []
    for mod in MODULES:
        d = os.path.join(repo, mod, "include")
        if os.path.isdir(d):
            include_dirs.append(d)
    deps = args.deps or os.environ.get("ZMBOR_DEPS_DIR")
    if deps is None:
        cand = os.path.expanduser("~/workspace/zmbor-deps")
        deps = cand if os.path.isdir(cand) else None
    if deps:
        for name in DEP_NAMES:
            for base in (os.path.join(deps, name),
                         os.path.join(deps, name + "-src")):
                for sub in ("include", "src", ""):
                    d = os.path.join(base, sub) if sub else base
                    if os.path.isdir(d):
                        include_dirs.append(d)
    else:
        print("warning: no dependency cache dir; headers needing "
              "dependency headers will fail", file=sys.stderr)
    include_flags = ["-I" + d for d in include_dirs]

    headers = []
    for mod in MODULES:
        inc_root = os.path.join(repo, mod, "include")
        for dirpath, _dirnames, filenames in os.walk(inc_root):
            for fn in sorted(filenames):
                if fn.endswith(".h"):
                    full = os.path.join(dirpath, fn)
                    headers.append((full, os.path.relpath(full, inc_root)))
    headers.sort(key=lambda h: h[1])
    print("compiler: %s -std=%s" % (compiler, args.std))
    print("headers: %d" % len(headers))

    failures = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(
            lambda h: check_header(args, h[0], h[1], include_flags),
            headers))
    for rel, ok, detail in sorted(results):
        if not ok:
            failures.append((rel, detail))
            print("FAIL: %s\n    %s" % (rel, detail))
    print("PASS=%d FAIL=%d" % (len(headers) - len(failures), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
