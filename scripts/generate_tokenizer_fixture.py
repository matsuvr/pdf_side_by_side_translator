#!/usr/bin/env python3
"""Generate expected full PFML3 prompt IDs with the pinned official tokenizer.

Run with the development environment in deps/tokenizer-test-tools.txt.
No official model weights or tokenizer source are copied into the repository.
"""
import importlib.util
import json
from pathlib import Path

from common import ROOT, WORK, download, lock, sha256


def main():
    spec = lock()["model_fixture"]
    revision = spec["tokenizer_revision"]
    base = "https://huggingface.co/" + spec["tokenizer_repository"] + "/resolve/" + revision + "/"
    local = WORK / "reference-tokenizer"
    names = ["tokenization_plamo.py", "tokenizer.jsonl", "tokenizer_config.json",
             "special_tokens_map.json", "chat_template.jinja"]
    for name in names:
        download(base + name, local / name)
    module_spec = importlib.util.spec_from_file_location("official_plamo", local / names[0])
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    tokenizer = module.Plamo3Tokenizer.from_pretrained(str(local), local_files_only=True)
    cases = {
        "english": "The device has 3 sensors. It does not use the network.",
        "japanese": "日本語の文を確認する。数値は123.45である。",
        "newlines": "First line.\nSecond line.\n\nThird paragraph.\n",
        "spaces": "  two  spaces    and a trailing space ",
        "repetition": "helloooo !!!! aaaa zzzzzzzz",
        "unicode": "Emoji 😀, ligature ﬁ, combining e\u0301, and x² + y².",
        "digits": "0 123 3.1415926535 2026-11-09 -42 1e-6",
    }
    lines = ["# Official PLaMo tokenizer " + revision,
             "# name<TAB>form<TAB>source_utf8_hex<TAB>expected_full_prompt_ids",
             "# BOS is emitted by the template; encode(add_special_tokens=False)."]
    for name, source in cases.items():
        for form in ("plain", "polite"):
            prompt = tokenizer.apply_chat_template(
                [{"role": "user", "content": source}], tokenize=False,
                add_generation_prompt=True, source_lang="en", target_lang="ja", form=form)
            ids = tokenizer.encode(prompt, add_special_tokens=False)
            lines.append("\t".join([name + "-" + form, form, source.encode().hex(), " ".join(map(str, ids))]))
    destination = ROOT / "tests/fixtures/plamo3-pfml3-reference.tsv"
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8")
    evidence = {"repository": spec["tokenizer_repository"], "revision": revision,
                "files": {n: sha256(local / n) for n in names},
                "generated_cases": len(cases) * 2,
                "fixture_sha256": sha256(destination)}
    (destination.with_suffix(".json")).write_text(json.dumps(evidence, indent=2) + "\n")
    print("Generated", len(cases) * 2, "reference cases")


if __name__ == "__main__":
    main()
