#!/usr/bin/env python3
"""Fetch locked official sources, then build PDFium with GN/Ninja.

The application CMake configuration never downloads dependencies.
"""
import argparse
import json
from pathlib import Path
import sys
import zipfile

from common import ROOT, WORK, TARGETS, checkout, depot_environment, download, lock, run, sha256


def prepare_llama(spec, src, target):
    entry = spec["dependencies"]["llama.cpp"]
    upstream = src / "llama.cpp"
    checkout(entry, upstream)
    if target.startswith("windows-x64-"):
        from windows_llama import prepare_windows_worktree
        patch, = (p for p in spec["patches"] if p["dependency"] == "llama.cpp"
                  and target in p["targets"])
        prepare_windows_worktree(upstream, src / "windows-x64/llama.cpp", entry["revision"], patch)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, choices=TARGETS)
    parser.add_argument("--fetch-only", action="store_true")
    parser.add_argument("--skip-pdfium", action="store_true")
    args = parser.parse_args()
    spec = lock()
    deps = spec["dependencies"]
    checkout(deps["wxWidgets"], WORK / "src/wxWidgets")
    # Keep the official checkout for the common license audit, but the Windows
    # app no longer builds the patched llama.cpp worktree.
    if args.target.startswith("windows-"):
        checkout(deps["llama.cpp"], WORK / "src/llama.cpp")
        run(["cargo", "+1.100.0", "fetch", "--locked", "--target", "x86_64-pc-windows-msvc"],
            cwd=ROOT / "src/platform/windows/plamo-rs")
    else:
        prepare_llama(spec, WORK / "src", args.target)

    sqlite = deps["SQLite"]
    archive = WORK / "downloads/sqlite.zip"
    download(sqlite["url"], archive, sqlite.get("sha256"))
    sqlite_dir = WORK / "src/SQLite"
    sqlite_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        for name in ("sqlite3.c", "sqlite3.h", "sqlite3ext.h"):
            candidates = [n for n in z.namelist() if n.endswith("/" + name)]
            if len(candidates) != 1:
                raise RuntimeError("Unexpected SQLite archive contents")
            (sqlite_dir / name).write_bytes(z.read(candidates[0]))

    font = deps["NotoSansCJKjp-Regular"]
    font_path = WORK / "resources/NotoSansCJKjp-Regular.otf"
    download(font["url"], font_path, font.get("sha256"))
    download(font["license_url"], WORK / "resources/NotoSansCJK-LICENSE.txt", font["license_sha256"])

    # Record observed hashes separately until reviewed and promoted into the lock.
    observed = {"SQLite": sha256(archive), "NotoSansCJKjp-Regular": sha256(font_path)}
    (WORK / "observed-download-hashes.json").write_text(json.dumps(observed, indent=2) + "\n")
    if args.skip_pdfium:
        return
    checkout(deps["depot_tools"], WORK / "src/depot_tools")
    workspace = WORK / "pdfium-workspace"
    workspace.mkdir(parents=True, exist_ok=True)
    config = {"name": "pdfium", "url": deps["PDFium"]["url"], "managed": False,
              "custom_deps": {}, "custom_vars": {"checkout_configuration": "minimal"}}
    (workspace / ".gclient").write_text("solutions = " + repr([config]) + "\n")
    env = depot_environment()
    run(["gclient", "sync", "--no-history", "--shallow", "--nohooks", "--revision",
         "pdfium@" + deps["PDFium"]["revision"]], cwd=workspace, env=env)
    run(["gclient", "runhooks"], cwd=workspace, env=env)
    if not args.fetch_only:
        run([sys.executable, ROOT / "scripts/build_pdfium.py", "--target", args.target], env=env)


if __name__ == "__main__":
    main()
