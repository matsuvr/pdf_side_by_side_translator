from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from bootstrap import prepare_llama


def git(directory, *args):
    return subprocess.check_output(["git", *args], cwd=directory)


class LlamaSourcesTest(unittest.TestCase):
    def test_alternating_targets_keeps_both_checkouts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            origin = root / "official"
            origin.mkdir()
            git(origin, "init", "--quiet")
            (origin / "kernel").write_bytes(b"upstream\n")
            git(origin, "add", "kernel")
            git(origin, "-c", "user.name=Fixture", "-c", "user.email=fixture@localhost",
                "-c", "commit.gpgSign=false", "commit", "-qm", "official")
            revision = git(origin, "rev-parse", "HEAD").decode().strip()
            (origin / "kernel").write_bytes(b"windows\n")
            git(origin, "add", "kernel")
            patch = root / "cpu.patch"
            patch.write_bytes(git(origin, "diff", "--cached", "--binary", "--no-ext-diff"))
            tree = git(origin, "write-tree").decode().strip()
            windows = "windows-x64-release"
            mac = "macos-arm64-release"
            spec = {"dependencies": {"llama.cpp": {"url": origin.as_uri(), "revision": revision}},
                    "patches": [{"dependency": "llama.cpp", "path": str(patch),
                                 "tree": tree, "targets": [windows]}]}
            src = root / "src"
            official = src / "llama.cpp"
            patched = src / "windows-x64/llama.cpp"

            prepare_llama(spec, src, mac)
            self.assertFalse(patched.exists())
            prepare_llama(spec, src, windows)
            windows_head = git(patched, "rev-parse", "HEAD")
            for target in (mac, windows):
                prepare_llama(spec, src, target)
                self.assertEqual(git(official, "rev-parse", "HEAD").decode().strip(), revision)
                self.assertEqual(git(patched, "rev-parse", "HEAD^").decode().strip(), revision)
                self.assertEqual(git(patched, "rev-parse", "HEAD"), windows_head)
                self.assertEqual((official / "kernel").read_text(), "upstream\n")
                self.assertEqual((patched / "kernel").read_text(), "windows\n")


if __name__ == "__main__":
    unittest.main()
