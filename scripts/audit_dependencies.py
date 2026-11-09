#!/usr/bin/env python3
"""Inspect actual Mach-O load commands or PE imports, including delay imports.

Exit nonzero on external non-OS dependencies. This check supplements, and does
not replace, first launch in a clean offline machine.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess


WINDOWS_OS_DLLS = set("""
advapi32.dll avrt.dll bcrypt.dll bcryptprimitives.dll cabinet.dll cfgmgr32.dll
comctl32.dll comdlg32.dll credui.dll crypt32.dll cryptbase.dll cryptsp.dll
d2d1.dll d3d11.dll d3d12.dll dbghelp.dll dnsapi.dll dwmapi.dll dwrite.dll dxgi.dll
gdi32.dll gdi32full.dll gdiplus.dll hid.dll imm32.dll iphlpapi.dll kernel32.dll
kernelbase.dll mpr.dll msacm32.dll msimg32.dll msvcp_win.dll msvcrt.dll ncrypt.dll
netapi32.dll normaliz.dll ntdll.dll ole32.dll oleacc.dll oleaut32.dll powrprof.dll
propsys.dll psapi.dll rpcrt4.dll secur32.dll setupapi.dll shell32.dll shlwapi.dll
ucrtbase.dll user32.dll userenv.dll usp10.dll uxtheme.dll version.dll
windowscodecs.dll winhttp.dll wininet.dll winmm.dll winspool.drv wintrust.dll
wldap32.dll ws2_32.dll wtsapi32.dll
""".split())


def pe_imports(path):
    data = path.read_bytes()
    def unpack(fmt, offset):
        size = struct.calcsize("<" + fmt)
        if offset < 0 or offset + size > len(data): raise ValueError("Truncated PE")
        return struct.unpack_from("<" + fmt, data, offset)
    if data[:2] != b"MZ": raise ValueError("Not a PE file")
    pe, = unpack("I", 0x3c)
    if data[pe:pe+4] != b"PE\0\0": raise ValueError("Invalid PE signature")
    machine, count, _, _, _, optional_size, _ = unpack("HHIIIHH", pe + 4)
    optional = pe + 24
    magic, = unpack("H", optional)
    if magic != 0x20b: raise ValueError("Only 64-bit PE is supported")
    image_base, = unpack("Q", optional + 24)
    directory_count, = unpack("I", optional + 108)
    sections = [unpack("8sIIIIIIHHI", optional + optional_size + i * 40) for i in range(count)]
    def offset_of(rva):
        for section in sections:
            _, virtual_size, virtual_address, raw_size, raw_address, *_ = section
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                delta = rva - virtual_address
                if delta >= raw_size: raise ValueError("PE directory points outside file-backed data")
                return raw_address + delta
        raise ValueError("PE RVA has no section")
    def cstring(rva):
        start = offset_of(rva)
        end = data.find(b"\0", start, min(len(data), start + 4096))
        if end < 0: raise ValueError("Unterminated PE DLL name")
        return data[start:end].decode("ascii").lower()
    def directory(index):
        if index >= directory_count: return 0, 0
        if 112 + (index + 1) * 8 > optional_size: raise ValueError("Invalid PE optional header")
        return unpack("II", optional + 112 + index * 8)
    result = {"machine": hex(machine), "normal": [], "delay": []}
    for index, field, size in [(1, "normal", 20), (13, "delay", 32)]:
        rva, length = directory(index)
        if not rva: continue
        start = offset_of(rva)
        terminated = False
        for i in range(length // size):
            values = unpack("I" * (size // 4), start + i * size)
            if not any(values): terminated = True; break
            name_rva = values[3] if index == 1 else values[1] if values[0] & 1 else values[1] - image_base
            result[field].append(cstring(name_rva))
        if not terminated: raise ValueError("Unterminated PE import directory")
    return result


def audit_pe(path):
    info = pe_imports(path)
    failures = []
    if info["machine"] != "0x8664": failures.append("Expected native Windows x64")
    for dll in info["normal"] + info["delay"]:
        if dll not in WINDOWS_OS_DLLS and not re.fullmatch(r"(?:api|ext)-ms-win-[a-z0-9-]+\.dll", dll):
            failures.append("Non-OS DLL: " + dll)
    return {"file": str(path), "format": "PE", **info, "failures": failures}


def audit_macho(path, expected_arch):
    info = subprocess.check_output(["otool", "-L", str(path)], text=True)
    commands = subprocess.check_output(["otool", "-l", str(path)], text=True)
    architecture = subprocess.check_output(["lipo", "-archs", str(path)], text=True).strip()
    dependencies = [line.strip().split(" (compatibility", 1)[0] for line in info.splitlines()[1:] if line.strip()]
    failures = ["Non-OS load dependency: " + dep for dep in dependencies
                if not dep.startswith(("/System/Library/", "/usr/lib/"))]
    if expected_arch and architecture != expected_arch: failures.append("Architecture mismatch: " + architecture)
    # Static application targets do not need developer-machine search paths.
    rpaths = re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", commands)
    for rpath in rpaths:
        if not rpath.startswith(("@loader_path", "@executable_path")):
            failures.append("External runtime search path: " + rpath)
    deployment = re.findall(r"(?:minos|version) (\d+\.\d+(?:\.\d+)?)", commands)
    return {"file": str(path), "format": "Mach-O", "architecture": architecture,
            "dependencies": dependencies, "rpaths": rpaths, "version_commands": deployment,
            "failures": failures}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--arch", choices=["arm64", "x86_64"])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.artifact.is_dir():
        candidates = [p for p in args.artifact.rglob("*") if p.is_file()]
    else: candidates = [args.artifact]
    reports = []
    for path in candidates:
        with path.open("rb") as stream: magic = stream.read(4)
        if magic[:2] == b"MZ": reports.append(audit_pe(path))
        elif magic in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
            reports.append(audit_macho(path, args.arch))
    if not reports: raise SystemExit("No executable found to inspect")
    output = json.dumps(reports, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end="")
    if any(r["failures"] for r in reports): raise SystemExit(1)


if __name__ == "__main__":
    main()
