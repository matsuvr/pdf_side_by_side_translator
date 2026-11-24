"""Exercise the Windows packaging CLI without a certificate or Windows SDK.

Run: python -m unittest discover -s tests/unit -p package_windows_test.py
"""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import package


class WindowsPackageTest(unittest.TestCase):
    target = 'windows-x64-release'
    thumbprint = 'ab' * 20
    timestamp_url = 'https://timestamp.example.test'
    binary_name = 'offline-pdf-translator.exe'
    signed_marker = b'RFC3161 test substitute'

    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.info = {'identity_sha256': 'c' * 64, 'dirty': False}
        build = self.root / 'build' / self.target
        build.mkdir(parents=True)
        (build / 'BUILD-INFO.json').write_text(json.dumps(self.info))
        self.binary = build / self.binary_name
        self.source = b'OPT_BUILD_INFO:' + self.info['identity_sha256'].encode()
        self.binary.write_bytes(self.source)
        pdf = self.root / '.work/pdfium' / self.target
        pdf.mkdir(parents=True)
        (pdf / 'pdfium-build-manifest.json').write_text(
            json.dumps({'revision': 'fixture', 'target': self.target}))
        license_file = self.root / 'LICENSE'
        license_file.write_bytes(b'fixture')
        self.enterContext(patch.object(package, 'ROOT', self.root))
        self.enterContext(patch.object(package.platform, 'system', return_value='Windows'))
        self.enterContext(patch.object(package.shutil, 'which', return_value='signtool.exe'))
        self.enterContext(patch.object(package, 'product', return_value={
            'OPT_VERSION': '1.2.3', 'OPT_PRODUCT_NAME': 'Offline PDF Translator',
            'OPT_EXECUTABLE_NAME': 'offline-pdf-translator'}))
        self.enterContext(patch.object(package, 'build_info', return_value=self.info))
        self.enterContext(patch.object(package, 'lock', return_value={
            'dependencies': {'PDFium': {'revision': 'fixture'}}}))
        self.enterContext(patch.object(package, 'companion_files', return_value={'LICENSE': license_file}))
        self.enterContext(patch.object(package, 'source_sbom', return_value={}))
        self.enterContext(patch.object(package, 'validate_static_windows'))
        self.enterContext(patch.object(package, 'audit_pe', return_value={'failures': []}))
        # Replace only the external subprocess boundary; real run() propagates
        # subprocess failure, and staging, ZIP, hashes, and manifest are real.
        self.calls = []
        self.fail_stage = None
        self.enterContext(patch.object(package.subprocess, 'run', side_effect=self.native))

    def native(self, command, **kwargs):
        if command[0] == 'signtool.exe':
            self.calls.append(command)
            path = Path(command[-1])
            if command[1] == 'sign':
                if self.fail_stage == 'sign':
                    raise subprocess.CalledProcessError(2, command)
                path.write_bytes(path.read_bytes() + self.signed_marker)
            else:
                if path.parent.name == self.fail_stage or self.signed_marker not in path.read_bytes():
                    self.assertTrue(kwargs['check'])
                    raise subprocess.CalledProcessError(2, command)
        return subprocess.CompletedProcess(command, 0)

    def invoke(self, *options):
        with patch.object(sys, 'argv', ['package.py', '--target', self.target, *options]), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            package.main()

    def signed(self):
        self.invoke('--windows-cert', self.thumbprint, '--windows-timestamp-url', self.timestamp_url)

    def zips(self):
        return list((self.root / 'dist').rglob('*.zip'))

    def test_timestamped_output_and_cache(self):
        self.signed()
        self.assertEqual(self.calls[0][:-1], [
            'signtool.exe', 'sign', '/s', 'My', '/sha1', self.thumbprint.upper(),
            '/fd', 'SHA256', '/tr', self.timestamp_url, '/td', 'SHA256'])
        self.assertEqual([Path(call[-1]).parent.name for call in self.calls],
                         ['payload', 'payload', 'extracted'])
        self.assertTrue(all(call[1:-1] == ['verify', '/pa', '/tw'] for call in self.calls[1:]))
        self.assertEqual(self.binary.read_bytes(), self.source)
        archive, = self.zips()
        self.assertTrue(archive.parent.name.endswith('-signed-rfc3161'))
        self.assertEqual(json.loads((archive.parent / 'package-manifest.json').read_text())[
            'windows_timestamp_url'], self.timestamp_url)
        with zipfile.ZipFile(archive) as source:
            self.assertCountEqual(source.namelist(), [self.binary_name, 'LICENSE'])
            self.assertEqual(source.read(self.binary_name), self.source + self.signed_marker)
        self.calls.clear()
        self.signed()
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(self.calls[0][1:-1], ['verify', '/pa', '/tw'])
        self.assertTrue(Path(self.calls[0][-1]).parent.name.startswith('.verify-'))

    def test_missing_timestamp_fails_closed(self):
        for stage in ('sign', 'payload', 'extracted'):
            with self.subTest(stage=stage):
                self.fail_stage = stage
                with self.assertRaises(subprocess.CalledProcessError):
                    self.signed()
                self.assertEqual(self.zips(), [])
                self.assertEqual(self.binary.read_bytes(), self.source)

    def test_unsigned_release_requires_explicit_opt_in(self):
        for options in ((), ('--windows-cert', self.thumbprint),
                        ('--windows-timestamp-url', self.timestamp_url),
                        ('--windows-cert', self.thumbprint, '--windows-timestamp-url', 'file:///path'),
                        ('--windows-cert', self.thumbprint, '--windows-timestamp-url',
                         'https://user@timestamp.example.test')):
            with self.subTest(options=options), self.assertRaises(SystemExit):
                self.invoke(*options)
        self.assertEqual(self.zips(), [])
        self.invoke('--allow-unsigned-windows-release')
        self.assertEqual(self.calls, [])
        archive, = self.zips()
        self.assertTrue(archive.name.endswith('-unsigned.zip'))
        self.assertEqual(json.loads((archive.parent / 'package-manifest.json').read_text())[
            'signature'], 'unsigned')
        with zipfile.ZipFile(archive) as source:
            self.assertEqual(source.read(self.binary_name), self.source)

    def test_cached_zip_cannot_substitute_different_executable(self):
        self.signed()
        archive, = self.zips()
        with zipfile.ZipFile(archive, 'w') as output:
            output.writestr(self.binary_name, self.source)
            output.writestr('LICENSE', b'fixture')
        manifest_file = archive.parent / 'package-manifest.json'
        manifest = json.loads(manifest_file.read_text())
        manifest_file.write_text(json.dumps({**manifest, 'sha256': package.sha256(archive)}))
        with self.assertRaisesRegex(RuntimeError, 'Missing or changed package file'):
            self.signed()


if __name__ == '__main__':
    unittest.main()
