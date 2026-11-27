"""Build the native viewer on its host OS. All output here is development-time."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MAC = sys.platform == "darwin"
PRODUCT = "Offline PDF Translator"
EXE = "offline-pdf-translator"
TARGET = "macos-arm64" if MAC else "windows-x64"
OUTPUT = ROOT / "build" / "native" / TARGET


def run(args, **kwargs):
    subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def model_spec():
    lock = json.loads((ROOT / "deps/models.json").read_text(encoding="utf8"))
    return lock["macos" if MAC else "windows"]


def copy_model(path, output=OUTPUT):
    spec = model_spec()
    with path.open("rb") as source:
        h = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            h.update(chunk)
        digest = h.hexdigest()
    if digest != spec["sha256"]:
        raise ValueError(f"Model checksum mismatch: {path}")
    target = output / spec["filename"]
    if not target.exists() or not path.samefile(target):
        shutil.copy2(path, target)


def stage_reader_files(output):
    from collect_notices import collect
    for filename in ["README.md", "LICENSE"]:
        shutil.copy2(ROOT / filename, output)
    for language in ["en", "ja"]:
        prefix = "PLaMo-Community-" if MAC else "PLaMo-BF16-Community-"
        shutil.copy2(ROOT / "licenses" / (prefix + language + ".txt"), output)
    shutil.copy2(ROOT / "licenses/Rust-1.100.0-COPYRIGHT-library.html", output)
    (output / "THIRD_PARTY_NOTICES.txt").write_text(collect(), encoding="utf-8")


def build(output=OUTPUT):
    if MAC:
        if platform.machine() != "arm64":
            raise RuntimeError("Build the macOS viewer on Apple Silicon.")
        from prepare_llama import prepare
        prepare()
        env = dict(os.environ, MACOSX_DEPLOYMENT_TARGET="14.0")
        run(["cargo", "build", "--release", "--locked", "-p", "pdf-reader"], env=env)
        contents = output / (PRODUCT + ".app") / "Contents"
        for name in ["MacOS", "Frameworks", "Resources"]:
            (contents / name).mkdir(parents=True, exist_ok=True)
        library = contents / "Frameworks/libpdf_reader.dylib"
        shutil.copy2(ROOT / "target/release/libpdf_reader.dylib", library)
        run(["install_name_tool", "-id", "@rpath/libpdf_reader.dylib", library])
        run(["swiftc", "-O", "-swift-version", "5", "-parse-as-library", "-target", "arm64-apple-macosx14.0",
             "-I", "crates/reader/include", "-L", library.parent, "-lpdf_reader", "-Xlinker", "-rpath",
             "-Xlinker", "@executable_path/../Frameworks", "apps/macos/Viewer.swift", "-o", contents / "MacOS" / EXE], env=env)
        props = dict(CFBundleExecutable=EXE, CFBundleIdentifier="io.github.matsuvr.offline-pdf-translator",
                     CFBundleName=PRODUCT, CFBundlePackageType="APPL", CFBundleInfoDictionaryVersion="6.0",
                     CFBundleShortVersionString="0.4.0", CFBundleVersion="0.4.0", CFBundleIconFile="app-icon",
                     LSMinimumSystemVersion="14.0", NSHighResolutionCapable=True, NSPrincipalClass="NSApplication",
                     CFBundleDocumentTypes=[dict(CFBundleTypeName="PDF", CFBundleTypeRole="Viewer",
                                                 LSHandlerRank="Alternate", LSItemContentTypes=["com.adobe.pdf"])])
        (contents / "Info.plist").write_bytes(plistlib.dumps(props))
        shutil.copy2(ROOT / "resources/app-icon.icns", contents / "Resources")
        run(["codesign", "--force", "--sign", "-", library])
        run(["codesign", "--force", "--sign", "-", contents.parent])
        run(["codesign", "--verify", "--deep", "--strict", contents.parent])
    elif sys.platform == "win32":
        project = "apps/windows/Viewer.csproj"
        run(["dotnet", "restore", project, "--locked-mode", "-p:Platform=x64"])
        run(["dotnet", "publish", project, "--no-restore", "-c", "Release", "-p:Platform=x64", "-o", output])
    else:
        raise RuntimeError("Build on Windows x64 or macOS arm64.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-file", type=Path, help="Stage the verified bundled model beside the viewer")
    args = parser.parse_args()
    build()
    if args.model_file:
        copy_model(args.model_file)
        stage_reader_files(OUTPUT)
    print(OUTPUT)
