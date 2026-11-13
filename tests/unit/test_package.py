"""Exercise real staging/ZIP paths with native signing and binary audits mocked.

These fixtures are not executables and do not prove native runtime behavior.
"""
import contextlib
import json
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import package

EVIDENCE = {'BUILD-INFO.json', 'DEPENDENCIES.json', 'SBOM.cdx.json'}


class PackageTest(unittest.TestCase):
    def package_fixture(self, target):
        mac = target.startswith('macos-')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            info = {'identity_sha256': 'a' * 64, 'dirty': False, 'target': target,
                    'product': 'Reader', 'version': '1.0', 'commit': 'b' * 40}
            values = {'OPT_VERSION': '1.0', 'OPT_PRODUCT_NAME': 'Reader',
                      'OPT_EXECUTABLE_NAME': 'reader', 'OPT_BUNDLE_ID': 'test.reader'}
            build = root / 'build' / target
            resources = build / 'Reader.app/Contents/Resources'
            resources.mkdir(parents=True)
            font = resources / 'NotoSansCJKjp-Regular.otf'
            font.write_bytes(b'font fixture')
            (resources / 'app-icon.icns').write_bytes(b'icon fixture')
            with (resources.parent / 'Info.plist').open('wb') as output:
                plistlib.dump({'CFBundleIdentifier': 'test.reader',
                              'CFBundleShortVersionString': '1.0'}, output)
            binary = build / ('Reader.app/Contents/MacOS/Reader' if mac else 'reader.exe')
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_bytes(('OPT_BUILD_INFO:' + info['identity_sha256']).encode())
            package.write_json(build / 'BUILD-INFO.json', info)
            manifest = root / '.work/pdfium' / target / 'pdfium-build-manifest.json'
            manifest.parent.mkdir(parents=True)
            pdf = {'revision': 'pdf-revision', 'target': target, 'resolved_dependencies': ''}
            package.write_json(manifest, pdf)
            (root / 'resources').mkdir()
            (root / 'resources/help-ja.txt').write_text('help')
            companions = {}
            for name in ['README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.txt',
                         'PLaMo-Community-ja.txt']:
                companions[name] = root / name
                companions[name].write_text('fixture ' + name)
            if target.endswith('-release'):
                companions['model.gguf'] = root / 'model.gguf'
                companions['model.gguf'].write_bytes(b'model fixture')
            sbom = {'bomFormat': 'CycloneDX', 'components': []}
            spec = {'dependencies': {'PDFium': {'revision': 'pdf-revision'},
                    'NotoSansCJKjp-Regular': {'sha256': package.sha256(font)}}}
            signed = []

            def fake_run(command, **kwargs):
                command = [str(x) for x in command]
                if command[0] == 'ditto':
                    source, destination = map(Path, command[-2:])
                    if '-x' in command:
                        with zipfile.ZipFile(source) as archive:
                            archive.extractall(destination)
                    else:
                        with zipfile.ZipFile(destination, 'w') as archive:
                            for path in source.rglob('*'):
                                if path.is_file():
                                    archive.write(path, path.relative_to(source))
                elif command[0] == 'codesign':
                    # Evidence never enters the bundle, including before signing.
                    bundle = Path(command[-1])
                    self.assertFalse(any(p.name in EVIDENCE for p in bundle.rglob('*')))
                    signed.append(command)
                return ''

            with contextlib.ExitStack() as stack:
                for name, value in [('ROOT', root), ('build_info', lambda *a: info),
                                    ('product', lambda: values), ('lock', lambda: spec),
                                    ('companion_files', lambda *a: companions),
                                    ('source_sbom', lambda *a: sbom), ('run', fake_run),
                                    ('validate_static_windows', lambda *a: None),
                                    ('audit_pe', lambda *a: {'failures': []}),
                                    ('audit_bundle', lambda *a: [{'failures': []}])]:
                    stack.enter_context(patch.object(package, name, value))
                stack.enter_context(patch.object(package.platform, 'system', return_value='Darwin' if mac else 'Windows'))
                stack.enter_context(patch.object(sys, 'argv', ['package.py', '--target', target]))
                package.main()
                output = root / 'dist' / target / info['identity_sha256'][:12]
                archive = next(output.glob('*.zip'))
                with zipfile.ZipFile(archive) as zipped:
                    names = zipped.namelist()
                    self.assertFalse(any(Path(n).name in EVIDENCE for n in names))
                    for name, source in companions.items():
                        self.assertEqual(zipped.read(name), source.read_bytes())
                    executable = 'Reader.app/Contents/MacOS/Reader' if mac else 'reader.exe'
                    self.assertEqual(zipped.read(executable), binary.read_bytes())
                    if mac:
                        self.assertEqual(zipped.read('Reader.app/Contents/Resources/NotoSansCJKjp-Regular.otf'), font.read_bytes())
                for name in EVIDENCE | {'package-manifest.json'}:
                    self.assertIsInstance(json.loads((output / name).read_text()), dict)
                self.assertEqual(json.loads((output / 'SBOM.cdx.json').read_text()), sbom)
                self.assertEqual(json.loads((output / 'DEPENDENCIES.json').read_text()), pdf)
                recorded = json.loads((output / 'package-manifest.json').read_text())
                self.assertEqual(recorded['sha256'], package.sha256(archive))
                self.assertEqual(recorded['model_included'], target.endswith('-release'))
                # Reusing a verified package still works with evidence outside ZIP.
                package.main()
                if mac:
                    self.assertTrue(signed)

    def test_windows_release(self):
        self.package_fixture('windows-x64-release')

    def test_macos_release(self):
        self.package_fixture('macos-arm64-release')

    def test_windows_debug(self):
        self.package_fixture('windows-x64-debug')

    def test_macos_debug(self):
        self.package_fixture('macos-arm64-debug')


if __name__ == '__main__':
    unittest.main()
