#!/usr/bin/env python3
"""Build the pinned PDFium and export the GN probe's resolved link closure."""
import argparse
import json
import os
import platform
import re
import shlex
import shutil

from common import ROOT, WORK, TARGETS, depot_environment, lock, run, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, choices=TARGETS)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    mac = args.target.startswith("macos-")
    if mac != (platform.system() == "Darwin"):
        raise SystemExit("Run PDFium's native build on the target operating system.")
    source = WORK / "pdfium-workspace/pdfium"
    out = source / "out" / args.target
    out.mkdir(parents=True, exist_ok=True)
    probe = source / "offline_probe"
    probe.mkdir(exist_ok=True)
    shutil.copyfile(ROOT / "tests/integration/pdf_probe.cpp", probe / "main.cpp")
    (probe / "BUILD.gn").write_text('''executable("offline_pdfium_link_probe") {
  sources = [ "main.cpp" ]
  deps = [ "//:pdfium" ]
  include_dirs = [ "//public" ]
}
''')
    settings = {
        "is_debug": args.target.endswith("-debug"),
        "is_component_build": False, "pdf_is_complete_lib": True,
        "pdf_is_standalone": False, "pdf_enable_v8": False,
        "pdf_enable_xfa": False, "pdf_use_skia": False,
        "pdf_use_agg": True, "pdf_enable_freetype": True,
        "pdf_bundle_freetype": True, "icu_use_data_file": False,
        "use_thin_lto": False, "use_remoteexec": False,
        "use_allocator_shim": False, "use_partition_alloc_as_malloc": False,
        "symbol_level": 0, "target_cpu": "arm64" if "arm64" in args.target else "x64",
    }
    if mac:
        settings["mac_deployment_target"] = "14.0"
    else:
        settings["is_clang"] = True
    gn_args = "\n".join(k + " = " + json.dumps(v) for k, v in settings.items()) + "\n"
    (out / "args.gn").write_text(gn_args)
    env = depot_environment()
    gn = source / "buildtools" / ("mac/gn" if mac else "win/gn.exe")
    ninja = source / "third_party/ninja" / ("ninja" if mac else "ninja.exe")
    # Fail on misspelled or removed upstream options.
    run([gn, "gen", out, "--fail-on-unused-args",
         "--root-target=//offline_probe:offline_pdfium_link_probe"], cwd=source, env=env)
    run([ninja, "-d", "keeprsp", "-C", out, "-j", args.jobs,
         "offline_pdfium_link_probe"], cwd=source, env=env)
    description = json.loads(run([gn, "desc", out,
                                 "//offline_probe:offline_pdfium_link_probe", "--format=json",
                                 "--root-target=//offline_probe:offline_pdfium_link_probe"],
                                cwd=source, env=env, capture=True))
    target = next(iter(description.values()))
    rsp = out / "offline_pdfium_link_probe.rsp"
    if not rsp.exists():
        # The exact output naming is fixed by the upstream toolchain.
        candidates = list(out.glob("*offline_pdfium_link_probe*.rsp"))
        if len(candidates) != 1:
            raise RuntimeError("Cannot locate GN link response file")
        rsp = candidates[0]
    words = shlex.split(rsp.read_text())
    archives = []
    declared_libraries = target.get("libs", [])
    for word in words:
        if word.endswith((".a", ".lib")):
            p = (out / word).resolve()
            if not p.is_file():
                if word in declared_libraries:
                    continue
                raise RuntimeError("Missing link archive: " + str(p))
            archives.append(str(p))
    if not archives:
        raise RuntimeError("GN probe produced no static archives")
    revision = run(["git", "rev-parse", "HEAD"], cwd=source, capture=True).strip()
    if revision != lock()["dependencies"]["PDFium"]["revision"]:
        raise RuntimeError("PDFium checkout differs from lock")
    libraries = [str(source / v[2:]) if v.startswith("//") else v for v in target.get("libs", [])]
    libraries = [v for v in libraries if v not in archives]
    manifest = {
        "schema": 1, "revision": revision, "target": args.target,
        "configuration": "Debug" if settings["is_debug"] else "Release",
        "gn_args": settings, "gn_description": target,
        "archives": archives, "private_runtime_archives": [a for a in archives if "libc++" in a],
        "public_includes": [str(source / "public")], "public_definitions": [],
        "system_libraries": libraries,
        "frameworks": [v.removesuffix(".framework") for v in target.get("frameworks", [])],
        "consumer_link_options": ["-Wl,-ObjC", "-fuse-ld=" + str(source / "third_party/llvm-build/Release+Asserts/bin/ld64.lld")] if mac else [],
        "crt": "private Chromium libc++ and OS runtime" if mac else "static-debug" if settings["is_debug"] else "static-release",
        "deps_sha256": sha256(source / "DEPS"),
        "resolved_dependencies": run(["gclient", "revinfo", "--actual"],
                                     cwd=source.parent, env=env, capture=True),
        "compiler": run([source / "third_party/llvm-build/Release+Asserts/bin" / ("clang++" if mac else "clang-cl.exe"),
                         "--version"], capture=True),
        "sdk": run(["xcrun", "--show-sdk-version"], capture=True).strip() if mac else os.environ.get("WindowsSDKVersion", "not recorded"),
        "resolved_gn_arguments": run([gn, "args", out, "--list", "--short"],
                                     cwd=source, env=env, capture=True),
    }
    path = WORK / "pdfium" / args.target / "pdfium-build-manifest.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, indent=2) + "\n")
    print("Wrote", path)


if __name__ == "__main__":
    main()
