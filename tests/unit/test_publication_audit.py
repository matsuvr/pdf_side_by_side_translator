import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('publication_audit',Path(__file__).resolve().parents[2]/'scripts/audit_publication.py')
audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)
class PublicationAuditTest(unittest.TestCase):
    def test_removed_historical_secret_is_still_reported_without_value(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            def git(*args):return subprocess.run(['git',*args],cwd=root,check=True,capture_output=True)
            git('init','-b','main');git('config','user.name','Fixture');git('config','user.email','fixture@users.noreply.github.com')
            # Synthetic credential-shaped text, never an actual credential.
            token='gh'+'p_'+'A'*36;(root/'removed.txt').write_text(token)
            git('add','.');git('commit','-m','synthetic fixture');git('rm','removed.txt');git('commit','-m','remove fixture')
            report=audit.audit_history(root,worktree=False)
            self.assertEqual(report['commits'],2)
            self.assertEqual(report['findings'][0]['pattern'],'github-token')
            self.assertNotIn(token,str(report))
    def test_runtime_artifacts_and_original_fixtures_are_distinct(self):
        self.assertTrue(audit.forbidden_path('private.GGUF'))
        self.assertTrue(audit.forbidden_path('backup/state.sqlite3-wal'))
        self.assertTrue(audit.forbidden_path('personal.pdf'))
        self.assertFalse(audit.forbidden_path('tests/fixtures/P01-simple.pdf'))
if __name__=='__main__':unittest.main()
