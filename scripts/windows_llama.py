"""Prepare the Windows CPU variant without changing the official llama.cpp checkout."""
import os
from pathlib import Path
import tempfile

from common import ROOT, assert_clean_checkout, run


def prepare_windows_worktree(upstream, destination, revision, patch):
    with tempfile.TemporaryDirectory() as scratch:
        env = {**os.environ, "GIT_INDEX_FILE": str(Path(scratch) / "index")}
        run(["git", "read-tree", revision], cwd=upstream, env=env)
        run(["git", "apply", "--cached", ROOT / patch["path"]], cwd=upstream, env=env)
        tree = run(["git", "write-tree"], cwd=upstream, env=env, capture=True).strip()
    if tree != patch["tree"]:
        raise RuntimeError("Patched llama.cpp tree differs from the dependency lock: " + tree)

    # llama.cpp embeds its Git revision, so use stable commit metadata.
    env = os.environ.copy()
    for role in ("AUTHOR", "COMMITTER"):
        env.update({f"GIT_{role}_NAME": "Offline PDF Translator",
                    f"GIT_{role}_EMAIL": "build@localhost",
                    f"GIT_{role}_DATE": "2000-01-01T00:00:00Z"})
    commit = run(["git", "-c", "commit.gpgSign=false", "commit-tree", tree, "-p", revision,
                  "-m", "Apply Windows PLaMo CPU kernels"], cwd=upstream, env=env, capture=True).strip()
    if not (destination / ".git").exists():
        run(["git", "worktree", "add", "--detach", destination, commit], cwd=upstream)
        return

    # Never repurpose the upstream checkout (including a junction pointing to it).
    common_dir_args = ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"]
    upstream_git = Path(run(common_dir_args, cwd=upstream, capture=True).strip())
    windows_git = Path(run(common_dir_args, cwd=destination, capture=True).strip())
    if destination.samefile(upstream) or not windows_git.samefile(upstream_git):
        raise RuntimeError(f"Expected a separate llama.cpp worktree at {destination}")
    assert_clean_checkout(destination)
    if run(["git", "rev-parse", "HEAD"], cwd=destination, capture=True).strip() != commit:
        run(["git", "checkout", "--detach", commit], cwd=destination)
