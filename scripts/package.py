"""Rebuild and verify the native release archive on the target OS."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile
from build import MAC, ROOT, TARGET, PRODUCT, EXE, build, copy_model, stage_reader_files, run
from common import sha256

def package(args):
    if args.target != TARGET + "-release":
        raise ValueError("Package on the target operating system.")
    if subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT).strip():
        raise ValueError("Commit reviewed changes before creating a release archive.")
    if (args.mac_identity or args.notary_profile) and not MAC:
        raise ValueError("Apple signing requires a Mac.")
    if args.notary_profile and not args.mac_identity:
        raise ValueError("Notarization requires --mac-identity.")
    if args.windows_cert and (MAC or not re.fullmatch(r"[A-Fa-f0-9]{40}", args.windows_cert)):
        raise ValueError("Expected a Windows certificate thumbprint.")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    dist = ROOT / "dist" / args.target / revision[:12]
    dist.mkdir(parents=True, exist_ok=True)
    destination = dist / ("offline-pdf-translator-" + args.target + ".zip")
    if destination.exists(): raise FileExistsError(destination)
    with tempfile.TemporaryDirectory(dir=dist) as temp:
        stage = Path(temp) / "payload"
        build(stage)
        copy_model(args.model_file, stage)
        stage_reader_files(stage)
        (stage / "BUILD-INFO.json").write_text(json.dumps({"commit": revision, "target": args.target,
            "cargo_lock_sha256": sha256(ROOT / "Cargo.lock")}, indent=2) + "\n")
        bundle = stage / (PRODUCT + ".app")
        if MAC and args.mac_identity:
            for item in [bundle / "Contents/Frameworks/libpdf_reader.dylib", bundle]:
                run(["codesign", "--force", "--options", "runtime", "--timestamp", "--sign", args.mac_identity, item])
            if args.notary_profile:
                submission = Path(temp) / "notary.zip"
                run(["ditto", "-c", "-k", "--keepParent", bundle, submission])
                run(["xcrun", "notarytool", "submit", submission, "--keychain-profile", args.notary_profile, "--wait"])
                run(["xcrun", "stapler", "staple", bundle])
        if args.windows_cert:
            for binary in [stage / (EXE + ".exe"), stage / "pdf_reader.dll"]:
                run(["signtool", "sign", "/sha1", args.windows_cert, "/fd", "SHA256", binary])
                run(["signtool", "verify", "/pa", binary])
        archive = Path(temp) / "release.zip"
        restored = Path(temp) / "restored"
        if MAC:
            run(["ditto", "-c", "-k", stage, archive])
            run(["ditto", "-x", "-k", archive, restored])
            run(["codesign", "--verify", "--deep", "--strict", restored / bundle.name])
        else:
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
                for file in stage.rglob("*"):
                    if file.is_file(): z.write(file, file.relative_to(stage))
            with zipfile.ZipFile(archive) as z: z.extractall(restored)
        for file in stage.rglob("*"):
            if file.is_file() and sha256(file) != sha256(restored / file.relative_to(stage)):
                raise RuntimeError("Archive verification failed: " + file.name)
        archive.rename(destination)
    print(destination)

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--target", default=TARGET + "-release")
    p.add_argument("--model-file", required=True, type=Path)
    p.add_argument("--windows-cert")
    p.add_argument("--mac-identity")
    p.add_argument("--notary-profile")
    package(p.parse_args())
