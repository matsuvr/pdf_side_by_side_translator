"""Regression checks for build-policy drift and offline package verification."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import verify_build as build
import verify_package as package


class BuildPolicyTest(unittest.TestCase):
    def fixture(self, root):
        target = 'windows-x64-release'
        cache = {name: 'OFF' for name in build.DISABLED}
        cache.update({'OPT_TARGET': target, 'CMAKE_BUILD_TYPE': 'Release',
                      'OPT_BUILD_APP': 'ON', 'OPT_PAGE_IMAGE_READING': 'ON',
                      'GGML_METAL': 'OFF', 'DISABLE_CURL': 'ON'})
        def write_cache():
            (root/'CMakeCache.txt').write_text(''.join(f'{key}:STRING={value}\n' for key, value in cache.items()))
        write_cache()
        entries = [{'file': 'C:/deps/SQLite/sqlite3.c', 'command': 'clang-cl -DSQLITE_OMIT_LOAD_EXTENSION /DSQLITE_TEMP_STORE=3 sqlite3.c'}]
        (root/'compile_commands.json').write_text(json.dumps(entries))
        archive = root/'pdfium.lib'
        archive.write_bytes(b'archive-fixture')
        settings = {'pdf_enable_v8': False, 'pdf_enable_xfa': False, 'is_component_build': False,
                    'pdf_is_complete_lib': True, 'is_debug': False, 'target_cpu': 'x64'}
        pdf = {'gn_args': settings.copy(), 'resolved_gn_arguments': '\n'.join(
            f'{name} = {json.dumps(value)}' for name, value in settings.items()),
            'archives': [str(archive)], 'archive_sha256': {str(archive): build.sha256(archive)}}
        return target, cache, write_cache, entries, pdf, archive

    def test_build_evidence_and_drift(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target, cache, write_cache, entries, pdf, archive = self.fixture(root)
            self.assertEqual(build.verify_build(root, target, pdf, True)['pdfium_archives_verified'], 1)
            for key in ('wxUSE_SOCKETS', 'LLAMA_BUILD_SERVER', 'GGML_RPC', 'DISABLE_CURL'):
                previous = cache[key]
                cache[key] = 'ON' if previous == 'OFF' else 'OFF'
                write_cache()
                with self.assertRaisesRegex(RuntimeError, key):
                    build.verify_build(root, target, pdf, True)
                cache[key] = previous
            del cache['wxUSE_WEBREQUEST']
            write_cache()
            with self.assertRaisesRegex(RuntimeError, 'wxUSE_WEBREQUEST'):
                build.verify_build(root, target, pdf, True)
            cache['wxUSE_WEBREQUEST'] = 'OFF'
            write_cache()
            pdf['gn_args']['pdf_enable_v8'] = True
            with self.assertRaisesRegex(RuntimeError, 'pdf_enable_v8'):
                build.verify_build(root, target, pdf, True)
            pdf['gn_args']['pdf_enable_v8'] = False
            pdf['resolved_gn_arguments'] = pdf['resolved_gn_arguments'].replace('pdf_enable_xfa = false', 'pdf_enable_xfa = true')
            with self.assertRaisesRegex(RuntimeError, 'pdf_enable_xfa'):
                build.verify_build(root, target, pdf, True)
            pdf['resolved_gn_arguments'] = pdf['resolved_gn_arguments'].replace('pdf_enable_xfa = true', 'pdf_enable_xfa = false')
            archive.write_bytes(b'changed archive')
            with self.assertRaisesRegex(RuntimeError, 'Changed PDFium'):
                build.verify_build(root, target, pdf, True)

    def test_sqlite_compile_policy(self):
        for command in ('clang -DSQLITE_TEMP_STORE=3',
                        'clang -DSQLITE_TEMP_STORE=2 -DSQLITE_OMIT_LOAD_EXTENSION',
                        'clang -DSQLITE_TEMP_STORE=3 -DSQLITE_OMIT_LOAD_EXTENSION -USQLITE_OMIT_LOAD_EXTENSION'):
            with self.assertRaisesRegex(RuntimeError, 'SQLite compile policy'):
                build.verify_sqlite([{'file': 'sqlite3.c', 'command': command}])


class PackageIntegrityTest(unittest.TestCase):
    def fixture(self, root):
        info = {'schema': 3, 'target': 'windows-x64-release', 'identity_sha256': 'a'*64}
        contents = {'BUILD-INFO.json': info, 'BUILD-VERIFICATION.json': {'target': info['target']},
                    'DEPENDENCIES.json': {'archives': []}, 'SBOM.cdx.json': {'components': []}}
        for name, value in contents.items():
            (root/name).write_text(json.dumps(value))
        binary = root/'reader.exe'
        binary.write_bytes(b'executable fixture')
        companion = root/'license.txt'
        companion.write_bytes(b'license fixture')
        archive = root/'reader.zip'
        with zipfile.ZipFile(archive, 'w') as output:
            output.write(binary, binary.name)
            output.write(companion, companion.name)
        manifest = {**info, 'artifact': archive.name, **package.file_record(archive),
                    'executable': {'name': binary.name, **package.file_record(binary)},
                    'files': {companion.name: package.file_record(companion)},
                    'evidence': {name: package.file_record(root/name) for name in contents},
                    'signature': 'Authenticode', 'signing_identity': 'b'*40}
        (root/'package-manifest.json').write_text(json.dumps(manifest))
        return manifest, archive

    def test_verify_archive_and_signature(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest, archive = self.fixture(root)
            with patch.object(package, 'audit_pe', return_value={'failures': []}), \
                 patch.object(package, 'verify_windows_binary_signature') as signature:
                self.assertEqual(package.verify_package(root, manifest['sha256'], 'b'*40, True), manifest)
                signature.assert_called_once()
                self.assertEqual(signature.call_args.args[1:], ('b'*40, 'b'*40))
                with self.assertRaisesRegex(RuntimeError, 'Manifest differs'):
                    package.verify_package(root, expected_manifest_sha256='f'*64)
                with self.assertRaisesRegex(RuntimeError, 'ZIP differs'):
                    package.verify_package(root, expected_sha256='f'*64)
                with zipfile.ZipFile(archive, 'a') as output:
                    output.writestr('unexpected.txt', b'bad')
                manifest.update(package.file_record(archive))
                (root/'package-manifest.json').write_text(json.dumps(manifest))
                with self.assertRaisesRegex(RuntimeError, 'ZIP payload'):
                    package.verify_package(root)

    def test_signature_identity_is_checked(self):
        with patch.object(package, 'run') as run, \
             patch.object(package.subprocess, 'check_output', return_value='a'*40+'\n'):
            with self.assertRaisesRegex(RuntimeError, 'Embedded signer'):
                package.verify_windows_binary_signature(Path('reader.exe'), 'b'*40)
            run.assert_called_once()
        with patch.object(package, 'run') as run:
            with self.assertRaisesRegex(RuntimeError, 'independently expected'):
                package.verify_windows_binary_signature(Path('reader.exe'), 'b'*40, 'a'*40)
            run.assert_not_called()

    def test_unsigned_requires_explicit_local_scope(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest, _ = self.fixture(root)
            manifest['signature'] = 'unsigned'
            (root/'package-manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(RuntimeError, 'signed package is required'):
                package.verify_package(root, require_signed=True)


if __name__ == '__main__':
    unittest.main()
