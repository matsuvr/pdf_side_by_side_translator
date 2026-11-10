"""Shared build helpers. All downloads are developer-only operations."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / ".work"
LOCK_PATH = ROOT / "deps/third_party.lock.json"
TARGETS = ("windows-x64-release", "windows-x64-debug", "macos-arm64-release",
           "macos-arm64-debug")


def lock():
    return json.loads(LOCK_PATH.read_text(encoding="utf-8"))


def run(args, cwd=ROOT, env=None, capture=False):
    print("+", " ".join(str(x) for x in args), flush=True)
    command = [str(x) for x in args]
    if os.name == "nt":
        # CreateProcess does not use the supplied environment's PATH or
        # PATHEXT to find batch tools such as depot_tools' gclient.bat.
        executable = shutil.which(command[0], path=(env or os.environ).get("PATH"))
        if executable:
            command[0] = executable
    return subprocess.run(command, cwd=cwd, env=env,
                          check=True, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url, destination, expected=None):
    destination = Path(destination)
    if destination.exists() and (expected is None or sha256(destination) == expected):
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".partial")
    print("Downloading", url, flush=True)
    with urllib.request.urlopen(url, timeout=120) as response, temporary.open("wb") as out:
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            out.write(chunk)
    if expected and sha256(temporary) != expected:
        temporary.unlink()
        raise RuntimeError("SHA-256 mismatch: " + str(destination))
    temporary.replace(destination)


def checkout(entry, destination):
    destination = Path(destination)
    if not (destination / ".git").exists():
        destination.mkdir(parents=True, exist_ok=True)
        run(["git", "init", destination])
        run(["git", "remote", "add", "origin", entry["url"]], cwd=destination)
    head = subprocess.run(["git", "rev-parse", "--verify", "HEAD"], cwd=destination,
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    current = head.stdout.strip() if head.returncode == 0 else None
    if current:
        assert_clean_checkout(destination)
    actual_origin = run(["git", "remote", "get-url", "origin"], cwd=destination, capture=True).strip()
    if actual_origin != entry["url"]:
        raise RuntimeError("Dependency origin differs from the official lock: " + destination.name)
    if current != entry["revision"]:
        run(["git", "fetch", "--depth=1", "origin", entry["revision"]], cwd=destination)
        run(["git", "checkout", "--detach", entry["revision"]], cwd=destination)
    if entry.get("submodules"):
        run(["git", "submodule", "update", "--init", "--recursive", "--depth=1"], cwd=destination)
    assert_clean_checkout(destination)


def assert_clean_checkout(directory):
    for options in (["diff", "--quiet", "--ignore-submodules=none"],
                    ["diff", "--cached", "--quiet", "--ignore-submodules=none"]):
        result = subprocess.run(["git", *options], cwd=directory)
        if result.returncode:
            raise RuntimeError("Tracked dependency source is modified; inspect before rebuilding: " + str(directory))


def depot_environment():
    env = os.environ.copy()
    # Do not let a developer's global include/library directories alter the
    # fixed graph (or make a host-only build tool link against local libraries).
    for key in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
                "LIBRARY_PATH", "CFLAGS", "CXXFLAGS", "LDFLAGS", "SDKROOT"):
        env.pop(key, None)
    depot = WORK / "src/depot_tools"
    env["PATH"] = str(depot) + os.pathsep + env["PATH"]
    env["DEPOT_TOOLS_UPDATE"] = "0"
    env["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"
    env["CIPD_CACHE_DIR"] = str(WORK / "cipd-cache")
    env["VPYTHON_VIRTUALENV_ROOT"] = str(WORK / "vpython")
    return env
