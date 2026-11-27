"""Collect the actual target's Rust, embedded asset and native runtime notices."""
import argparse
import json
from pathlib import Path
import subprocess
from build import ROOT, MAC

def license_files(directory):
    names = ("license", "licence", "copying", "notice", "thirdpartynotices", "copyright")
    return sorted(p for p in directory.rglob("*") if p.is_file() and
                  (p.name.lower().startswith(names) or "_license" in p.name.lower())
                  and p.suffix.lower() not in (".rs", ".cs", ".cpp", ".h", ".json", ".py")
                  and ".git" not in p.parts)

def collect():
    target = "aarch64-apple-darwin" if MAC else "x86_64-pc-windows-msvc"
    def cargo(*args):
        return subprocess.check_output(["cargo", *args], cwd=ROOT, encoding="utf-8")
    selected = set(cargo("tree", "--locked", "-p", "pdf-reader", "--target", target, "--edges",
                         "normal", "--prefix", "none", "--format", "{p}", "--no-dedupe").splitlines())
    packages = json.loads(cargo("metadata", "--locked", "--filter-platform", target, "--format-version", "1"))
    notices = {}
    inventory = []
    def add(label, files):
        for path in files:
            text = path.read_text(encoding="utf-8-sig", errors="replace").strip()
            if text:
                owners = notices.setdefault(text, [])
                if label not in owners:
                    owners.append(label)
    for p in sorted(packages["packages"], key=lambda p: p["name"]):
        if not p["source"]: continue
        label = f"{p['name']} v{p['version']}"
        if label not in selected: continue
        inventory.append(label + "  " + str(p["license"]))
        add(label, license_files(Path(p["manifest_path"]).parent))
    if not MAC:
        assets = json.loads((ROOT / "apps/windows/obj/project.assets.json").read_text(encoding="utf-8-sig"))
        folders = [Path(p) for p in assets["packageFolders"]]
        for label, pkg in sorted(assets["libraries"].items()):
            if pkg["type"] != "package": continue
            directory = next(f / pkg["path"] for f in folders if (f / pkg["path"]).is_dir())
            add(label, license_files(directory))
        # .NET's self-contained framework packs are SDK downloads and may be
        # outside project.assets.json's resolved package libraries.
        runtime = json.loads((ROOT / "apps/windows/bin/x64/Release/net8.0-windows10.0.19041.0/win-x64/offline-pdf-translator.runtimeconfig.json").read_text())
        for framework in runtime["runtimeOptions"].get("includedFrameworks", []):
            name, version = framework["name"], framework["version"]
            for folder in folders:
                directory = folder / (name.lower() + ".runtime.win-x64") / version
                if directory.is_dir():
                    add(name + " " + version, license_files(directory))
                    break
    sections = ["Offline PDF Translator — Built with PLaMo", target, "\n".join(inventory)]
    sections.extend("--- " + ", ".join(owners) + " ---\n" + text for text, owners in notices.items())
    return "\n\n".join(sections) + "\n"

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(collect(), encoding="utf-8")
