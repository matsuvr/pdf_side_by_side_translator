#!/usr/bin/env python3
"""Generate expected full PFML3 prompt IDs with the pinned official tokenizer.

Run with the development environment in deps/tokenizer-test-tools.txt.
No official model weights or tokenizer source are copied into the repository.
"""
import importlib.util
import json
import sys
from pathlib import Path

from common import ROOT, WORK, download, lock, sha256


def load_official_tokenizer(local):
    module_spec = importlib.util.spec_from_file_location("official_plamo", local / "tokenization_plamo.py")
    module = importlib.util.module_from_spec(module_spec)
    sys.modules[module_spec.name] = module
    module_spec.loader.exec_module(module)
    return module.Plamo3Tokenizer.from_pretrained(str(local), local_files_only=True)


def translation_prompt(tokenizer, source, form, *, tokenize):
    options = {"target_lang": "ja", "form": "plain"} if form == "plain" else {"target_lang": "ja(easy)"}
    return tokenizer.apply_chat_template([{"role": "user", "content": source}],
                                         tokenize=tokenize, add_generation_prompt=True, **options)


def translation_vector(tokenizer, source, form):
    from jinja2.exceptions import TemplateError

    try:
        ids = translation_prompt(tokenizer, source, form, tokenize=True)
    except TemplateError:
        return "REJECT"
    return " ".join(map(str, ids))


def main():
    spec = lock()["macos"]
    revision = spec["tokenizer_revision"]
    base = "https://huggingface.co/" + spec["tokenizer_repository"] + "/resolve/" + revision + "/"
    local = WORK / "reference-tokenizer"
    names = ["tokenization_plamo.py", "tokenizer.jsonl", "tokenizer_config.json",
             "special_tokens_map.json", "chat_template.jinja"]
    for name in names:
        download(base + name, local / name)
    tokenizer = load_official_tokenizer(local)
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
        for form in ("plain", "easy"):
            options = {"source_lang": "en", "target_lang": "ja"}
            if form == "easy":
                options["target_lang"] = "ja(easy)"
            else:
                options["form"] = "plain"
            prompt = tokenizer.apply_chat_template(
                [{"role": "user", "content": source}], tokenize=False,
                add_generation_prompt=True, **options)
            if form == "easy":
                canonical = tokenizer.apply_chat_template(
                    [{"role": "user", "content": source}], tokenize=False,
                    add_generation_prompt=True, source_lang="en", target_lang="ja-easy")
                if prompt != canonical:
                    raise RuntimeError("Official ja(easy) alias differs from ja-easy")
            ids = tokenizer.encode(prompt, add_special_tokens=False)
            lines.append("\t".join([name + "-" + form, form, source.encode().hex(), " ".join(map(str, ids))]))
    auto_cases = {
        "literal-marker": "a<|plamo:tag|>b", "unfinished-marker": "a<|plamo:",
        "control-spelling": "a:plamo|>b", "embedded-nul": "A\0B🙂",
        "blank": " \t\x1c\u3000", "bom-only": "\ufeff",
        "unicode-page": "  निवास\u00a0कार्ड\r\n𠮷 e\u0301 👩\u200d💻\n",
    }
    for name, source in auto_cases.items():
        for form in ("plain", "easy"):
            lines.append("\t".join([name, form + "-auto", source.encode().hex(),
                                    translation_vector(tokenizer, source, form)]))
    count = 2 * (len(cases) + len(auto_cases))
    destination = ROOT / "tests/fixtures/plamo3-pfml3-reference.tsv"
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8")
    evidence = {"repository": spec["tokenizer_repository"], "revision": revision,
                "files": {n: sha256(local / n) for n in names},
                "generated_cases": count,
                "fixture_sha256": sha256(destination)}
    (destination.with_suffix(".json")).write_text(json.dumps(evidence, indent=2) + "\n")
    print("Generated", count, "reference cases")


if __name__ == "__main__":
    main()
