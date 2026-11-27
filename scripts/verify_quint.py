"""Run the development-only Quint suite and replay navigation traces via the C ABI."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from common import ROOT

FORMAL = ROOT / "formal"
CLI = FORMAL / "node_modules/@informalsystems/quint/dist/src/cli.js"


def verify(directory, models_only):
    def quint(command, spec, *args):
        print(f"\n{command} {spec.name}", flush=True)
        subprocess.run(["node", CLI, command, spec, *args], cwd=directory, check=True)

    tlc = ["--backend=tlc", f"--tlc-config={FORMAL / 'tlc.json'}", "--verbosity=3"]
    for name in ("caches", "decode", "navigation", "pool", "reader"):
        spec = FORMAL / f"{name}.qnt"
        quint("typecheck", spec)
        emit_traces = spec.stem == "navigation" and not models_only
        test_args = ["--out-itf=case-{test}-{seq}.json"] if emit_traces else []
        quint("test", spec, "--backend=typescript", *test_args)
        run_args = ["--out-itf=sample-{seq}.json", "--n-traces=4"] if emit_traces else []
        quint("run", spec, "--backend=typescript", "--seed=0x51de", "--verbosity=1",
              "--max-samples=100", "--max-steps=30", *run_args)
        # Verbosity 3 includes TLC's state counts and counterexample trace.
        temporal = ["--temporal=dispatchReturns"] if spec.stem == "pool" else []
        quint("verify", spec, *tlc, *temporal)
        if spec.stem == "reader":
            quint("verify", spec, *tlc, "--main=readerProgress",
                  "--temporal=finishesAfterLastRequest,publishesSuccessfulTranslation")
            quint("verify", spec, *tlc, "--main=readableDocument", "--temporal=showsTranslation")

    if not models_only:
        cases = sorted(directory.glob("case-*.json"))
        samples = sorted(directory.glob("sample-*.json"))
        if not cases or len(samples) != 4:
            raise RuntimeError("Quint did not produce the expected navigation traces")
        from replay_quint import replay
        failures = []
        try:
            replay(reader_library(), cases + samples, directory)
        except (AssertionError, RuntimeError, subprocess.CalledProcessError) as error:
            failures.append(f"C ABI replay: {error}")
        cargo = ["cargo", "test", "--locked", "--release"]
        if subprocess.run([*cargo, "--workspace", "--no-fail-fast"], cwd=ROOT).returncode:
            failures.append("Rust workspace tests failed")
        if os.environ.get("OPT_P4X86_TEST_MODEL"):
            if subprocess.run([*cargo, "-p", "plamo3-translate-rs", "--test", "page_limit",
                               "--", "--ignored"], cwd=ROOT).returncode:
                failures.append("S4 model token boundary test failed")
        if failures:
            raise RuntimeError("\n".join(failures))


def reader_library():
    result = subprocess.run(
        ["cargo", "build", "--locked", "--release", "-p", "pdf-reader",
         "--message-format=json-render-diagnostics"],
        cwd=ROOT, stdout=subprocess.PIPE, check=True, text=True, encoding="utf-8")
    for line in result.stdout.splitlines():
        artifact = json.loads(line)
        if artifact["reason"] == "compiler-artifact" and artifact["target"]["name"] == "pdf_reader":
            for filename in artifact["filenames"]:
                if Path(filename).suffix in (".dll", ".dylib"):
                    return filename
    raise RuntimeError("Cargo did not report a native pdf_reader library")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models-only", action="store_true",
                        help="run all Quint checks without building the native reader")
    args = parser.parse_args()
    if not args.models_only and not __debug__:
        parser.exit(1, "Run without Python -O so the ABI replay assertions are enabled.\n")
    if not CLI.is_file():
        parser.exit(1, "Install the locked development tools with: npm ci --prefix formal\n")
    work = ROOT / ".work/quint"
    work.mkdir(parents=True, exist_ok=True)
    # Each invocation consumes only its own traces. After a failure keep the
    # trace and checker diagnostics so the failing sequence can be inspected.
    directory = Path(tempfile.mkdtemp(prefix="check-", dir=work))
    try:
        verify(directory, args.models_only)
    except (OSError, RuntimeError, AssertionError, subprocess.CalledProcessError) as error:
        sys.exit(f"{error}\nVerification outputs: {directory}")
    print(f"\nVerification passed. Outputs: {directory}")


if __name__ == "__main__":
    main()
