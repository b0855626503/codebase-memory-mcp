#!/usr/bin/env python3
"""gen-compile-commands.py — Generate per-file compile_commands.json for clangd.

This project's Makefile.cbm uses unity-build: all main sources are compiled in a
single cc command. bear can only capture that as one entry, which is useless for
clangd. This script generates individual entries per source file using the known
compiler flags from the Makefile.

Usage:
  python3 scripts/gen-compile-commands.py                  # Full regenerate
  python3 scripts/gen-compile-commands.py --merge-only     # Merge bear output only (no rebuild)
"""

import json
import glob
import os
import sys
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ── Compiler flags extracted from Makefile.cbm ──────────────────────
# These must stay in sync with CFLAGS_PROD + GCC_ONLY_FLAGS + include paths.

CFLAGS_C = [
    "-std=c11",
    "-D_DEFAULT_SOURCE", "-D_GNU_SOURCE",
    "-Wall", "-Wextra", "-Werror",
    "-Wno-unused-parameter", "-Wno-sign-compare",
    "-Wno-format-truncation", "-Wno-unused-result",
    "-Wno-stringop-truncation", "-Wno-alloc-size-larger-than",
    "-O2", "-DCBM_BIND_TS_ALLOCATOR=1",
    "-Isrc", "-Ivendored", "-Ivendored/sqlite3",
    "-Ivendored/mimalloc/include",
    "-Iinternal/cbm",
    "-Iinternal/cbm/vendored/ts_runtime/include",
]

CFLAGS_CPP = [
    "-std=c++14",
    "-Wall", "-Wextra", "-Werror",
    "-Wno-unused-parameter",
    "-O2",
    "-Iinternal/cbm",
    "-Iinternal/cbm/vendored/ts_runtime/include",
    "-Iinternal/cbm/vendored",
]

# ── Source globs matching PROD_SRCS + EXISTING_C_SRCS ───────────────

SRC_GLOBS = [
    "src/foundation/*.c",
    "src/store/*.c",
    "src/cypher/*.c",
    "src/mcp/*.c",
    "src/discover/*.c",
    "src/graph_buffer/*.c",
    "src/pipeline/*.c",
    "src/simhash/*.c",
    "src/semantic/*.c",
    "src/traces/*.c",
    "src/watcher/*.c",
    "src/git/*.c",
    "src/cli/*.c",
    "src/ui/*.c",
    "src/main.c",
    "vendored/yyjson/yyjson.c",
    "internal/cbm/cbm.c",
    "internal/cbm/extract_defs.c",
    "internal/cbm/extract_calls.c",
    "internal/cbm/extract_imports.c",
    "internal/cbm/extract_usages.c",
    "internal/cbm/extract_unified.c",
    "internal/cbm/extract_semantic.c",
    "internal/cbm/extract_type_refs.c",
    "internal/cbm/extract_type_assigns.c",
    "internal/cbm/extract_env_accesses.c",
    "internal/cbm/extract_channels.c",
    "internal/cbm/extract_k8s.c",
    "internal/cbm/helpers.c",
    "internal/cbm/lang_specs.c",
    "internal/cbm/service_patterns.c",
    "internal/cbm/ac.c",
    "internal/cbm/lz4_store.c",
    "internal/cbm/zstd_store.c",
    "internal/cbm/sqlite_writer.c",
    "internal/cbm/lsp_all.c",
    "internal/cbm/ts_runtime.c",
    "internal/cbm/preprocessor.cpp",
]

# ── Bear-captured vendored objects (always kept from bear output) ──

BEAR_PATTERNS = [
    "grammar_",
    "sqlite3.c",
    "mimalloc",
    "tre_all.c",
    "lz4.c",
    "lz4hc.c",
    "zstd.c",
    "unixcoder_blob",
]


def is_bear_entry(filepath: str) -> bool:
    """Check if this file was compiled individually by Makefile (bear captures it)."""
    basename = os.path.basename(filepath)
    return any(p in basename or p in filepath for p in BEAR_PATTERNS)


def collect_project_sources() -> list[str]:
    """Collect all project source files from SRC_GLOBS."""
    files = []
    for pat in SRC_GLOBS:
        for f in sorted(glob.glob(os.path.join(ROOT, pat))):
            if os.path.basename(f).startswith("grammar_"):
                continue
            if is_bear_entry(f):
                continue
            files.append(f)
    return files


def make_entry(filepath: str) -> dict:
    """Create a compile_commands.json entry for a single source file."""
    if filepath.endswith(".cpp"):
        compiler = "c++"
        flags = list(CFLAGS_CPP)
    else:
        compiler = "cc"
        flags = list(CFLAGS_C)

    return {
        "directory": ROOT,
        "file": filepath,
        "arguments": [compiler] + flags + ["-c", filepath],
    }


def load_bear_entries() -> list[dict]:
    """Load bear-generated compile_commands.json, keeping only vendored entries."""
    bear_path = os.path.join(ROOT, "compile_commands.json")
    if not os.path.exists(bear_path):
        return []
    with open(bear_path) as f:
        try:
            entries = json.load(f)
        except json.JSONDecodeError:
            return []

    return [e for e in entries if is_bear_entry(e.get("file", ""))]


def main():
    merge_only = "--merge-only" in sys.argv

    if not merge_only:
        # Check if bear output exists
        bear_path = os.path.join(ROOT, "compile_commands.json")
        if not os.path.exists(bear_path) or os.path.getsize(bear_path) < 100:
            print("[gen-compile-commands] Running bear to capture vendored compilations...")
            subprocess.run(
                ["make", "-f", "Makefile.cbm", "clean-c"],
                cwd=ROOT, check=True,
            )
            subprocess.run(
                ["bear", "--", "make", "-f", "Makefile.cbm", "cbm", f"-j{os.cpu_count()}"],
                cwd=ROOT, check=True,
            )

    # Collect
    project_files = collect_project_sources()
    project_entries = [make_entry(f) for f in project_files]
    bear_entries = load_bear_entries()

    # Merge + deduplicate
    seen = set()
    all_entries = []
    for e in project_entries + bear_entries:
        f = e["file"]
        if f not in seen:
            seen.add(f)
            all_entries.append(e)

    # Sort by file path
    all_entries.sort(key=lambda e: e["file"])

    # Write
    output_path = os.path.join(ROOT, "compile_commands.json")
    with open(output_path, "w") as f:
        json.dump(all_entries, f, indent=2)
        f.write("\n")

    # Summary
    n_src = sum(1 for e in all_entries if "/src/" in e["file"])
    n_vendored = len(all_entries) - n_src
    print(f"[gen-compile-commands] {len(project_entries)} project + {len(bear_entries)} bear = {len(all_entries)} total entries")
    print(f"  src/: {n_src}   vendored/internal: {n_vendored}")
    print(f"  {output_path}")


if __name__ == "__main__":
    main()
