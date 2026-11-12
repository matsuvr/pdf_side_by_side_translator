"""Guard distribution checks against overlooking delay-loaded runtimes."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("audit", ROOT / "scripts/audit_dependencies.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def synthetic_pe(normal, delay):
    b = bytearray(4096)
    b[:2] = b"MZ"
    struct.pack_into("<I", b, 0x3c, 128)
    b[128:132] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", b, 132, 0x8664, 1, 0, 0, 0, 240, 0)
    optional = 152
    struct.pack_into("<H", b, optional, 0x20b)
    struct.pack_into("<Q", b, optional + 24, 0x140000000)
    struct.pack_into("<I", b, optional + 108, 16)
    struct.pack_into("<8sIIIIIIHHI", b, optional + 240, b".rdata", 3584, 0x1000, 3584, 512, 0, 0, 0, 0, 0)
    struct.pack_into("<II", b, optional + 112 + 8, 0x1000, 40)
    struct.pack_into("<II", b, optional + 112 + 13*8, 0x1080, 64)
    struct.pack_into("<IIIII", b, 512, 0, 0, 0, 0x1200, 0)
    struct.pack_into("<IIIIIIII", b, 640, 1, 0x1280, 0, 0, 0, 0, 0, 0)
    b[1024:1024+len(normal)+1] = normal.encode()+b"\0"
    b[1152:1152+len(delay)+1] = delay.encode()+b"\0"
    return b


class DependencyAuditTest(unittest.TestCase):
    def test_deployment_is_not_a_dylib_compatibility_version(self):
        commands='''Load command 0
          cmd LC_LOAD_DYLIB
          current version 3000.1.0
          compatibility version 1000.0.0
        Load command 1
          cmd LC_BUILD_VERSION
          platform 1
          minos 14.0
          sdk 27.2
        '''
        self.assertEqual(audit.macho_deployment(commands),{'minimum_os':'14.0','sdk':'27.2'})

    def report(self, normal, delay, require_hardening=False):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.exe"
            path.write_bytes(synthetic_pe(normal, delay))
            return audit.audit_pe(path, require_hardening)

    def test_distribution_requires_hardening(self):
        self.assertEqual(len(self.report("kernel32.dll", "user32.dll", True)["failures"]), 2)

    def test_delay_loaded_runtime_is_not_missed(self):
        report = self.report("kernel32.dll", "MSVCP140.dll")
        self.assertEqual(report["delay"], ["msvcp140.dll"])
        self.assertEqual(report["failures"], ["Non-OS DLL: msvcp140.dll"])

    def test_os_msvcp_win_is_distinct_from_redistributable(self):
        self.assertFalse(self.report("msvcp_win.dll", "api-ms-win-core-file-l1-1-0.dll")["failures"])

    def test_normal_application_dependency_is_rejected(self):
        self.assertEqual(self.report("pdfium.dll", "user32.dll")["failures"], ["Non-OS DLL: pdfium.dll"])

    def test_network_imports_in_both_directories(self):
        self.assertEqual(self.report("WS2_32.dll", "winhttp.dll")["failures"],
                         ["Network DLL: ws2_32.dll", "Network DLL: winhttp.dll"])
        self.assertTrue(self.report("kernel32.dll", "api-ms-win-net-isolation-l1-1-0.dll")["failures"])

    def test_truncation_is_not_treated_as_no_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.exe"
            path.write_bytes(synthetic_pe("kernel32.dll", "user32.dll")[:100])
            with self.assertRaises(ValueError): audit.pe_imports(path)


if __name__ == "__main__":
    unittest.main()
