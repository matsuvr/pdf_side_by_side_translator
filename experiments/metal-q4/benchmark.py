"""Paired developer benchmark. Each process warms up on the same PDF page."""
import argparse
import csv
import hashlib
from pathlib import Path
import re
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("probe")
p.add_argument("q4_model")
p.add_argument("gguf_model")
p.add_argument("--page", action="append", nargs=2, required=True, metavar=("PDF", "PAGE"))
p.add_argument("--rounds", type=int, default=4)
args = p.parse_args()
if args.rounds < 1:
    p.error("--rounds must be positive")
writer = csv.writer(sys.stdout)
writer.writerow(["pair", "pdf", "page", "backend", "load_s", "cold_page_s", "page_s", "first_scores_s",
                 "input_tokens", "output_tokens", "translation_sha256"])
models = {"q4": args.q4_model, "reference": args.gguf_model}
for pair in range(args.rounds):
    for page_index, (pdf, page) in enumerate(args.page):
        methods = list(models)
        if (pair + page_index) % 2:
            methods.reverse()
        for method in methods:
            result = subprocess.run([args.probe, method, models[method], pdf, page, "2"],
                                    capture_output=True, text=True, check=True)
            lines = result.stdout.splitlines()
            def fields(line):
                return dict(re.findall(r"(\w+)=(\S+)", line))
            first, warm = fields(lines[0]), fields(lines[-1])
            if first.get("run") != "0" or warm.get("run") != "1":
                raise ValueError(f"Unexpected probe result: {result.stdout}")
            text = "\n".join(lines[1:-1])
            digest = hashlib.sha256(text.encode()).hexdigest()
            writer.writerow([pair + 1, Path(pdf).name, page, method, fields(result.stderr)["model_open_s"],
                             first["total_s"], warm["total_s"], warm["first_scores_s"],
                             warm["input"], warm["output"], digest])
            sys.stdout.flush()
