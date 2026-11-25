#!/usr/bin/env python3
"""Check the configured feature policy and compiled SQLite configuration.

This is evidence against accidental build drift. It is not a proof of the
behavior of a binary or an attestation from a trusted build service.
"""
import json
from pathlib import Path
import re

from common import sha256

# Explicit settings owned by this application, not guesses about dependency defaults.
DISABLED = {
    'BUILD_SHARED_LIBS', 'GGML_BACKEND_DL', 'GGML_BLAS', 'GGML_CUDA',
    'GGML_HIP', 'GGML_VULKAN', 'GGML_RPC', 'GGML_SYCL', 'GGML_OPENCL',
    'GGML_OPENVINO', 'GGML_WEBGPU', 'GGML_ZDNN', 'GGML_VIRTGPU', 'GGML_MUSA',
    'GGML_HEXAGON', 'LLAMA_BUILD_SERVER', 'LLAMA_BUILD_APP', 'LLAMA_BUILD_COMMON',
    'LLAMA_BUILD_TOOLS', 'LLAMA_BUILD_TESTS', 'LLAMA_BUILD_EXAMPLES',
    'LLAMA_BUILD_UI', 'LLAMA_BUILD_MTMD', 'wxUSE_WEBVIEW', 'wxUSE_SOCKETS',
    'wxUSE_URL', 'wxUSE_PROTOCOL', 'wxUSE_PROTOCOL_HTTP', 'wxUSE_PROTOCOL_FTP',
    'wxUSE_PROTOCOL_FILE', 'wxUSE_FS_INET', 'wxUSE_WEBREQUEST', 'wxUSE_IPC',
}


def cmake_cache(path):
    values = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if line.startswith(('#', '//')) or '=' not in line:
            continue
        key, value = line.split('=', 1)
        name, sep, _ = key.partition(':')
        if sep:
            if name in values:
                raise RuntimeError('Duplicate CMake cache variable: ' + name)
            values[name] = value
    return values


def require_bool(cache, name, enabled):
    # CMake boolean values are case-insensitive. Missing is not evidence.
    value = cache.get(name, '').upper()
    allowed = {'1', 'ON', 'TRUE', 'YES', 'Y'} if enabled else {'0', 'OFF', 'FALSE', 'NO', 'N'}
    if value not in allowed:
        raise RuntimeError(f'Build policy mismatch: {name} must be {"enabled" if enabled else "disabled"}')
    return enabled


def verify_pdfium(pdf, target):
    expected = {
        'pdf_enable_v8': False, 'pdf_enable_xfa': False,
        'is_component_build': False, 'pdf_is_complete_lib': True,
        'is_debug': target.endswith('-debug'),
        'target_cpu': 'arm64' if 'arm64' in target else 'x64',
    }
    resolved = pdf.get('resolved_gn_arguments', '')
    for name, value in expected.items():
        if pdf.get('gn_args', {}).get(name) != value:
            raise RuntimeError('PDFium requested setting mismatch: ' + name)
        matches = re.findall(r'^\s*' + re.escape(name) + r'\s*=\s*(.*?)\s*$', resolved, re.MULTILINE)
        if matches != [json.dumps(value)]:
            raise RuntimeError('PDFium resolved setting mismatch: ' + name)
    # Archives were recorded by the actual PDFium build. Recheck before package.
    archives = pdf.get('archives', [])
    hashes = pdf.get('archive_sha256', {})
    if not archives or set(archives) != set(hashes):
        raise RuntimeError('Missing or inconsistent PDFium archive provenance; rebuild PDFium')
    for name in archives:
        path = Path(name)
        if not path.is_file() or sha256(path) != hashes[name]:
            raise RuntimeError('Changed PDFium build archive: ' + path.name)
    return expected


def verify_sqlite(entries):
    candidates = [entry for entry in entries if Path(entry['file'].replace('\\', '/')).name.lower() == 'sqlite3.c']
    if len(candidates) != 1:
        raise RuntimeError('Expected exactly one SQLite amalgamation compile record')
    entry = candidates[0]
    # Read the compiler's generated compile command, not CMake's textual intent.
    command = entry['command']
    expected = {'SQLITE_OMIT_LOAD_EXTENSION': '1', 'SQLITE_TEMP_STORE': '3'}
    recorded = {}
    for name, value in expected.items():
        # CMake may emit -D or /D and optionally quote the macro argument.
        defines = re.findall(r'(?:^|\s)[/-]D"?'+name+r'(?:=([^"\s]+))?"?(?=\s|$)', command)
        undefs = re.findall(r'(?:^|\s)[/-]U"?'+name+r'"?(?=\s|$)', command)
        if len(defines) != 1 or undefs or (defines[0] or '1') != value:
            raise RuntimeError('SQLite compile policy mismatch: ' + name)
        recorded[name] = value
    return recorded


def verify_build(build, target, pdf, image_reading):
    cache = cmake_cache(build / 'CMakeCache.txt')
    if cache.get('OPT_TARGET') != target:
        raise RuntimeError('CMake cache target does not match package target')
    if cache.get('CMAKE_BUILD_TYPE') != ('Debug' if target.endswith('-debug') else 'Release'):
        raise RuntimeError('CMake build type does not match package target')
    checks = {name: False for name in sorted(DISABLED)}
    checks.update({'OPT_BUILD_APP': True, 'OPT_PAGE_IMAGE_READING': image_reading})
    if target.startswith('windows-'):
        checks['GGML_METAL'] = False
    if image_reading:
        checks['DISABLE_CURL'] = True
    for name, enabled in checks.items():
        require_bool(cache, name, enabled)
    entries = json.loads((build / 'compile_commands.json').read_text(encoding='utf-8'))
    return {'schema': 1, 'target': target, 'cmake_features': checks,
            'sqlite_compile_definitions': verify_sqlite(entries),
            'pdfium_resolved_settings': verify_pdfium(pdf, target),
            'pdfium_archives_verified': len(pdf['archives'])}
