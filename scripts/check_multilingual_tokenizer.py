"""Compare the viewer's model inputs with PFN's official translation template and tokenizer."""
import argparse
import html
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unicodedata
from generate_tokenizer_fixture import load_official_tokenizer, translation_prompt, translation_vector

ROOT = Path(__file__).resolve().parents[1]


def print_samples(browser, samples, directory):
    markup = '<meta charset="utf-8"><style>@page{size:A4;margin:12mm}body{margin:0}' \
             'section{break-after:page;font:12pt Arial,sans-serif}section:last-child{break-after:auto}</style>'
    markup += "".join(f'<section lang="{tag}" dir="auto">{html.escape(text)}</section>' for tag, text in samples)
    document = directory / "samples.html"
    document.write_text(markup, encoding="utf-8")
    pdf = directory / "samples.pdf"
    subprocess.run([browser, "--headless", "--disable-gpu", "--disable-background-networking",
                    "--disable-extensions", "--no-pdf-header-footer", f"--user-data-dir={directory / 'chrome'}",
                    f"--print-to-pdf={pdf}", document.as_uri()], capture_output=True, check=True, timeout=60)
    return pdf


def pdf_cases(pdf, samples, probes):
    pages = []
    for probe in probes:
        layout_probe = probe.with_name("layout_probe" + probe.suffix)
        layout = json.loads(subprocess.check_output([layout_probe, pdf]))
        pages.append([page["text"] for page in layout["pages"]])
    if len(pages[0]) != len(samples) or pages[0] != pages[1]:
        raise ValueError("Different PDF sources or incorrect number of pages")
    cases = []
    differences = []
    for (tag, original), text in zip(samples, pages[0]):
        if not text.strip() or "\ufffd" in text:
            raise ValueError(f"Missing or damaged native PDF text: {tag}")
        if original != text:
            differences.append(tag)
        cases.append((tag + "-pdf", text))
    print(f"{len(cases)} PDF pages matched between builds; "
          f"differences from fixture before PDF printing: {', '.join(differences) or 'none'}")
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("gguf-probe", "rust-probe", "gguf", "rust-model"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--official-tokenizer", type=Path, default=ROOT / ".work/reference-tokenizer",
                        help="Directory containing the five pinned PFN tokenizer/template files")
    parser.add_argument("--reference", type=Path, help="Also verify UTF-8 round trips with this GGUF probe")
    parser.add_argument("--upstream-probe", type=Path, help="Also compare the unpatched llama.cpp viewer build")
    parser.add_argument("--corpus", type=Path, help="Extra cases as name, tab, hexadecimal UTF-8 source")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--pdf", type=Path, help="Compare one PDF page per language, in fixture order")
    source.add_argument("--chrome", type=Path, help="Print the samples to a temporary A4 PDF with this browser")
    parser.add_argument("--codepoints", action="store_true",
                        help="Also check each observed Unicode scalar and the entire basic Devanagari block in isolation")
    args = parser.parse_args()
    manifest = json.loads((ROOT / "tests/fixtures/plamo3-pfml3-reference.json").read_text(encoding="utf-8"))
    for filename, expected_hash in manifest["files"].items():
        if hashlib.sha256((args.official_tokenizer / filename).read_bytes()).hexdigest() != expected_hash:
            raise ValueError(f"Official reference file differs from {manifest['revision']}: {filename}")
    tokenizer = load_official_tokenizer(args.official_tokenizer)
    lines = (ROOT / "tests/fixtures/plamo3-language-samples.tsv").read_text(encoding="utf-8").splitlines()
    roster = (ROOT / "deps/image_reading_languages.tsv").read_text(encoding="utf-8").splitlines()
    samples = [line.split("\t", 1) for line in lines if line and not line.startswith("#")]
    tags = [line.split("\t")[0] for line in roster if line and not line.startswith("#")]
    if sorted(tag for tag, _ in samples) != sorted(tags) or len(set(tags)) != 53:
        raise ValueError("Expected one native-language sample per supported language")
    cases = []
    for tag, text in samples:
        cases.append((tag, text))
        mixed = unicodedata.normalize("NFD", text) + "  १२३ e\u0301 👩\u200d💻 𠮷"
        cases.append((tag + "-mixed", mixed))
    with tempfile.TemporaryDirectory() as directory:
        pdf = print_samples(args.chrome, samples, Path(directory).resolve()) if args.chrome else args.pdf
        if pdf:
            cases.extend(pdf_cases(pdf, samples, (args.gguf_probe, args.rust_probe)))
        if args.codepoints:
            observed = {char for _, text in cases for char in text}
            devanagari = {chr(value) for value in range(0x0900, 0x0980)}
            characters = sorted(observed | devanagari)
            print(f"{len(observed)} distinct input code points; {len(characters)} including basic Devanagari")
            cases.extend((f"U+{ord(char):04X}", char) for char in characters)
        if args.reference:
            corpus = Path(directory) / "samples.tsv"
            with corpus.open("w", encoding="utf-8", newline="\n") as out:
                for tag, text in cases:
                    print(tag, text.encode("utf-8").hex(), sep="\t", file=out)
            subprocess.run([args.reference, args.gguf, corpus], stdout=subprocess.DEVNULL, check=True)
        cases.extend((tag + "-whitespace", "\ufeff  " + text + "\t\r\n" + text + "\u00a0")
                     for tag, text in samples)
        cases.extend([
            ("empty", ""), ("blank", " \t\x1c\u3000"), ("bom-only", "\ufeff"),
            ("embedded-nul", "A\0B🙂"), ("boundary", "ab\uee00cd"),
            ("combining", "①Ａé e\u0301"), ("repeated", "a    !!!!\n\n\n\nb"),
            ("partial-marker", "a<|plamo:"), ("unknown-marker", "a<|plamo:not-known|>b"),
            ("partial-control", "a:plamo|>b"), ("byte-spelling", "a<0x41>b"),
        ])
        for token_id, (spelling, _, kind) in enumerate(tokenizer.data):
            if kind in ("CONTROL", "UNKNOWN"):
                cases.append((f"reserved-spelling-{token_id}", "L" + spelling + "R"))
        if args.corpus:
            for line in args.corpus.read_text(encoding="ascii").splitlines():
                tag, source_hex = line.split("\t")
                cases.append((tag, bytes.fromhex(source_hex).decode("utf-8")))
        vectors = Path(directory) / "vectors.tsv"
        rejected = 0
        rendered = Path(directory) / "rendered.tsv"
        rendered_ids = []
        with vectors.open("w", encoding="utf-8", newline="\n") as out, rendered.open("w", encoding="utf-8") as full:
            for tag, source_text in cases:
                for form in ("plain", "easy"):
                    vector = translation_vector(tokenizer, source_text, form)
                    rejected += vector == "REJECT"
                    print(tag, form + "-auto", source_text.encode("utf-8").hex(), vector, sep="\t", file=out)
                    if args.reference and vector != "REJECT":
                        prompt = translation_prompt(tokenizer, source_text, form, tokenize=False)
                        name = tag + "-" + form
                        print(name, prompt.encode().hex(), sep="\t", file=full)
                        rendered_ids.append(name + "\t" + vector)
        if args.reference:
            raw = subprocess.check_output([args.reference, args.gguf, rendered, "--rendered"])
            if [line.rstrip() for line in raw.decode("utf-8").splitlines()] != rendered_ids:
                raise RuntimeError("GGUF tokenization of the rendered PFN prompts differs from PFN")
            print(f"{len(rendered_ids)} complete PFN prompts matched directly in llama.cpp")
        expected = 2 * len(cases)
        backends = [(args.gguf_probe, args.gguf), (args.rust_probe, args.rust_model)]
        if args.upstream_probe:
            backends.append((args.upstream_probe, args.gguf))
        for probe, model in backends:
            result = subprocess.run([probe, model, "cpu", vectors, "--prepare-only"],
                                    stdout=subprocess.PIPE, check=True)
            if f"reference_tokenizer_cases={expected} matched".encode() not in result.stdout:
                raise RuntimeError(f"{probe} did not verify every vector")
        print(f"{len(tags)} languages, {len(backends)} backends: {expected - rejected} official prompt vectors "
              f"and {rejected} official rejections matched on each backend")


if __name__ == "__main__":
    main()
