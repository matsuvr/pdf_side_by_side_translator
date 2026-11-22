"""Compare Release translation probes on identical, PDF-extracted pages."""
import argparse
import csv
import hashlib
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile
import textwrap
import time

from common import ROOT, sha256
from generate_fixtures import write_pdf


ABSTRACT = (
    "We study how the choice of sampling interval affects estimates of transport in porous media. "
    "A camera records the positions of tracer particles as water flows through a bed of glass beads. "
    "For each trajectory we compare the measured displacement with the prediction of a diffusion model. "
    "The mean displacement increases with elapsed time, but the distribution of individual displacements "
    "remains asymmetric. At short intervals the uncertainty is dominated by the resolution of the camera. "
    "At longer intervals a small number of fast particles account for most of the variance. We report "
    "confidence intervals from independent experiments and discuss the limits of extrapolating these "
    "observations to larger systems."
)


def fixtures(directory):
    abstract = directory / "abstract.pdf"
    lines = textwrap.wrap(ABSTRACT, width=88)
    write_pdf(abstract, [[(40, 730 - 14 * i, 10, line) for i, line in enumerate(lines)]])

    long_page = directory / "licenses.pdf"
    lines = []
    for source in (ROOT / "LICENSE", ROOT / "licenses/Chromium-BSD.txt"):
        for paragraph in source.read_text(encoding="utf-8").split("\n\n"):
            if paragraph.strip():
                text = " ".join(paragraph.split()).encode("ascii", "replace").decode()
                lines.extend(textwrap.wrap(text, width=96))
                lines.append("")
    height = 40 + 14 * len(lines)
    write_pdf(long_page, [[(32, height - 26 - 14 * i, 9, line) for i, line in enumerate(lines) if line]],
              page_size=(650, height))
    return [ROOT / "tests/fixtures/P01-simple.pdf", abstract, long_page]


def trial(probe, model, pdf, env):
    started = time.perf_counter()
    p = subprocess.run([probe, model, "cpu", pdf], capture_output=True, timeout=180, env=env)
    wall = time.perf_counter() - started
    output = p.stdout.decode("utf-8").replace("\r\n", "\n")
    _, complete, translation = output.partition("\npage_complete:\n")
    chunks = re.findall(r"^chunk reason=(\d+) seconds=(\S+) input_tokens=(\d+) output_tokens=(\d+)$",
                        output, flags=re.MULTILINE)
    if p.returncode or not complete or not translation.strip() or not chunks or any(c[0] != "0" for c in chunks):
        raise RuntimeError(f"{probe} failed on {pdf.name}\n{output[-500:]}\n{p.stderr.decode('utf-8', 'replace')[-1500:]}")
    return (sum(float(c[1]) for c in chunks), wall, sum(int(c[2]) for c in chunks),
            sum(int(c[3]) for c in chunks), hashlib.sha256(translation.strip().encode()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("probes", type=Path, nargs="+", help="Static probe binaries with distinct file names")
    args = parser.parse_args()
    if args.runs < 1 or args.threads < 1:
        parser.error("runs and threads must be positive")
    probes = [p.resolve() for p in args.probes]
    if len({p.name for p in probes}) != len(probes):
        parser.error("Give each probe a distinct file name")
    model = args.model.resolve()
    print(f"# model_sha256={sha256(model)} threads={args.threads}")
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(["run", "pdf", "probe", "translation_s", "process_s", "input_tokens", "output_tokens", "text_sha256"])
    env = {**os.environ, "OPT_INFERENCE_THREADS": str(args.threads)}
    samples = {}
    with tempfile.TemporaryDirectory() as scratch:
        pages = fixtures(Path(scratch))
        for i in range(args.runs):
            offset = i % len(probes)
            order = probes[offset:] + probes[:offset]
            for pdf in pages:
                for probe in order:
                    row = trial(probe, model, pdf, env)
                    writer.writerow([i + 1, pdf.name, probe.name, *row])
                    sys.stdout.flush()
                    samples.setdefault((pdf.name, probe.name), []).append(row)
    for (pdf, probe), rows in samples.items():
        median = statistics.median(r[0] for r in rows)
        wall = statistics.median(r[1] for r in rows)
        distinct = len({r[4] for r in rows})
        print(f"# {pdf} {probe} translation_s={median:.4f} process_s={wall:.4f} distinct_outputs={distinct}")


if __name__ == "__main__":
    main()
