#!/usr/bin/env python3
"""Independently verify a Windows release ZIP and accompanying build evidence.

The manifest and its hashes provide integrity, not authenticity. Obtain a
trusted manifest digest and publisher certificate thumbprint independently.
Native Windows signature verification is performed on the extracted executable;
no package code is executed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import zipfile

from audit_dependencies import audit_pe
from common import run, sha256

EVIDENCE_NAMES = {'BUILD-INFO.json', 'DEPENDENCIES.json', 'SBOM.cdx.json', 'BUILD-VERIFICATION.json'}


def file_record(path):
    return {'sha256': sha256(path), 'size': path.stat().st_size}


def require_record(actual, wanted, description):
    if actual != wanted:
        raise RuntimeError('Integrity mismatch: ' + description)


def verify_evidence(directory, manifest):
    expected = manifest.get('evidence', {})
    if set(expected) != EVIDENCE_NAMES:
        raise RuntimeError('Missing build evidence inventory')
    for name, record in expected.items():
        require_record(file_record(directory / name), record, name)
    # BUILD-INFO and the manifest must identify the same source build.
    info = json.loads((directory / 'BUILD-INFO.json').read_text(encoding='utf-8'))
    for key, value in info.items():
        if key == 'signature':
            continue
        if manifest.get(key) != value:
            raise RuntimeError('Build identity mismatch: ' + key)
    verification = json.loads((directory / 'BUILD-VERIFICATION.json').read_text(encoding='utf-8'))
    if verification.get('target') != manifest.get('target'):
        raise RuntimeError('Build verification target mismatch')


def verify_windows_binary_signature(path, thumbprint, expected_thumbprint=None):
    if not re.fullmatch(r'[0-9a-fA-F]{40}', thumbprint or ''):
        raise RuntimeError('Missing or invalid publisher certificate thumbprint')
    if expected_thumbprint and thumbprint.upper() != expected_thumbprint.upper():
        raise RuntimeError('Publisher certificate differs from independently expected identity')
    run(['signtool', 'verify', '/pa', str(path)])
    # Compare the actual embedded signer certificate against the expected identity,
    # in addition to the platform trust verification above. The file is not run.
    env = {**os.environ, 'OPT_SIGNATURE_FILE': str(path.resolve())}
    actual = subprocess.check_output([
        'powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
        '$s=Get-AuthenticodeSignature -LiteralPath $env:OPT_SIGNATURE_FILE;'
        'if ($s.Status -ne "Valid" -or $s.SignatureType -ne "Authenticode" -or !$s.SignerCertificate) { exit 1 };'
        '$s.SignerCertificate.Thumbprint'
    ], text=True, env=env).strip()
    if actual.upper() != thumbprint.upper():
        raise RuntimeError('Embedded signer certificate does not match recorded identity')


def verify_windows_zip(archive, manifest, expected_thumbprint=None, require_signed=False):
    files = manifest.get('files', {})
    executable = manifest.get('executable', {})
    name = executable.get('name', '')
    if not isinstance(name, str) or not name.lower().endswith('.exe') or not all(
            isinstance(n, str) and n not in {'', '.', '..'} and
            '/' not in n and '\\' not in n and ':' not in n and n == n.rstrip(' .')
            and not re.fullmatch(r'(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])', n.split('.')[0], re.IGNORECASE)
            for n in [*files, name]):
        raise RuntimeError('Invalid Windows payload file names')
    if name in files:
        raise RuntimeError('Duplicate executable in package inventory')
    expected = {**files, name: {'sha256': executable.get('sha256'), 'size': executable.get('size')}}
    if manifest.get('signature') not in {'Authenticode', 'unsigned'}:
        raise RuntimeError('Unknown Windows signing state')
    signed = manifest['signature'] == 'Authenticode'
    if (require_signed or expected_thumbprint) and not signed:
        raise RuntimeError('A signed package is required')
    with tempfile.TemporaryDirectory(prefix='opt-verify-') as temporary, zipfile.ZipFile(archive) as source:
        entries = source.infolist()
        # Exact names, no duplicates even on a case-insensitive filesystem.
        if set(entry.filename for entry in entries) != set(expected) or len(entries) != len(expected):
            raise RuntimeError('ZIP payload does not match manifest inventory')
        if len({entry.filename.casefold() for entry in entries}) != len(entries):
            raise RuntimeError('ZIP case-colliding file names')
        for entry in entries:
            record = expected[entry.filename]
            if entry.is_dir() or entry.file_size != record['size']:
                raise RuntimeError('ZIP member size mismatch: ' + entry.filename)
            digest = hashlib.sha256()
            with source.open(entry) as stream:
                output = (Path(temporary) / name).open('wb') if entry.filename == name else None
                try:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                        digest.update(chunk)
                        if output:
                            output.write(chunk)
                finally:
                    if output:
                        output.close()
            if digest.hexdigest() != record['sha256']:
                raise RuntimeError('ZIP member hash mismatch: ' + entry.filename)
        binary = Path(temporary) / name
        if audit_pe(binary)['failures']:
            raise RuntimeError('Archived executable dependency/mitigation verification failed')
        if signed:
            verify_windows_binary_signature(binary, manifest.get('signing_identity'), expected_thumbprint)


def verify_package(directory, expected_sha256=None, expected_thumbprint=None, require_signed=False, expected_manifest_sha256=None):
    manifest_path = directory / 'package-manifest.json'
    if expected_manifest_sha256 and sha256(manifest_path) != expected_manifest_sha256.lower():
        raise RuntimeError('Manifest differs from independently expected SHA-256')
    manifest = json.loads((directory / 'package-manifest.json').read_text(encoding='utf-8'))
    name = manifest['artifact']
    if not isinstance(name, str) or any(ch in name for ch in '/\\:') or not name.endswith('.zip'):
        raise RuntimeError('Invalid archive name')
    archive = directory / name
    record = file_record(archive)
    require_record(record, {'sha256': manifest['sha256'], 'size': manifest['size']}, name)
    if expected_sha256 and record['sha256'] != expected_sha256.lower():
        raise RuntimeError('ZIP differs from independently expected SHA-256')
    verify_evidence(directory, manifest)
    if not manifest.get('target', '').startswith('windows-'):
        raise RuntimeError('Independent native verification currently supports Windows packages')
    verify_windows_zip(archive, manifest, expected_thumbprint, require_signed)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, help='Directory containing the ZIP and publisher evidence')
    parser.add_argument('--expected-sha256', help='Optional separately trusted archive digest')
    parser.add_argument('--expected-manifest-sha256', required=True, help='Manifest digest obtained from a separately trusted source')
    parser.add_argument('--expected-thumbprint', required=True, help='Publisher certificate thumbprint obtained independently')
    args = parser.parse_args()
    if platform.system() != 'Windows':
        parser.error('Run on Windows with the Windows SDK SignTool available')
    if args.expected_sha256 and not re.fullmatch(r'[0-9a-fA-F]{64}', args.expected_sha256):
        parser.error('Expected SHA-256 must be 64 hexadecimal digits')
    if not re.fullmatch(r'[0-9a-fA-F]{64}', args.expected_manifest_sha256):
        parser.error('Expected manifest SHA-256 must be 64 hexadecimal digits')
    if not re.fullmatch(r'[0-9a-fA-F]{40}', args.expected_thumbprint):
        parser.error('Expected certificate thumbprint must be 40 hexadecimal digits')
    verify_package(args.directory, args.expected_sha256, args.expected_thumbprint, require_signed=True, expected_manifest_sha256=args.expected_manifest_sha256)
    print('Verified archive, evidence and native Authenticode signature')


if __name__ == '__main__':
    main()
