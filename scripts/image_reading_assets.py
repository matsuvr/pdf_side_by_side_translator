"""Validate the OCR roster and generate the native embedded model data."""
import argparse
from pathlib import Path
import re
import struct

from common import ROOT, sha256

AUTO_MODELS = {"osd", "script/Latin", "script/Cyrillic", "script/Arabic", "script/Devanagari"}


def rows(path):
    return [line.split("\t") for line in path.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")]


def catalog():
    langs = rows(ROOT / "deps/image_reading_languages.tsv")
    models = rows(ROOT / "deps/image_reading_models.tsv")
    if len(langs) != 53 or len({row[0] for row in langs}) != 53:
        raise ValueError("Expected 53 distinct PLaMo language tags")
    for tag, key, label, scripts in langs:
        if not re.fullmatch(r"[a-z]+(?:-[a-z]+)?", tag) or not label or any(c in label for c in '"\\'):
            raise ValueError("Invalid language entry: " + tag)
        if not re.fullmatch(r"[A-Z][a-z]+(?:\+[A-Z][a-z]+)?", scripts):
            raise ValueError("Invalid script: " + scripts)
    if len({row[0] for row in models}) != len(models) or set(row[0] for row in models) != (
        AUTO_MODELS | {key for row in langs for key in row[1].split("+")}
    ):
        raise ValueError("OCR model lock does not match the language roster")
    for key, digest, size, output_digest in models:
        if not re.fullmatch(r"(?:script/)?[a-zA-Z_]+", key) or not re.fullmatch(r"[0-9a-f]{64}", digest) or not size.isdecimal() or not re.fullmatch(r"[0-9a-f]{64}", output_digest):
            raise ValueError("Invalid model entry: " + key)
    return langs, models


def strip_script_dictionary(data):
    """Remove only LSTM word-dawg (entry 19); keep the recognizer/charset."""
    n = struct.unpack_from("<I", data)[0]
    if n != 24:
        raise ValueError("Unexpected Tesseract component count")
    offsets = struct.unpack_from(f"<{n}q", data, 4)
    present = [v for v in offsets if v != -1]
    if present != sorted(set(present)) or present[0] != 4 + 8*n or present[-1] >= len(data):
        raise ValueError("Unexpected Tesseract component offsets")
    if offsets[19] == -1 or any(offsets[i] == -1 for i in (17, 21, 22)):
        raise ValueError("Missing expected Tesseract component")
    begin = offsets[19]
    end = min((v for v in present if v > begin), default=len(data))
    table = [-1 if i == 19 else v - (end - begin) if v >= end else v for i, v in enumerate(offsets)]
    return struct.pack(f"<I{n}q", n, *table) + data[4 + 8*n:begin] + data[end:]


def write_changed(path, text):
    data = text.encode("utf-8")
    if not path.is_file() or path.read_bytes() != data:
        path.write_bytes(data)


def generate(out, model_dir, platform):
    langs, models = catalog()
    out.mkdir(parents=True, exist_ok=True)
    lang_data = ",\n".join('    {"%s","%s","%s","%s"}' % tuple(row) for row in langs)
    model_data, native_data = [], []
    offset = 0
    for index, (key, _, size, output_digest) in enumerate(models, 300):
        path = model_dir / (key + ".traineddata")
        if not path.is_file() or (not key.startswith("script/") and path.stat().st_size != int(size)) or sha256(path) != output_digest:
            raise RuntimeError(f"Missing or unverified {path.name}; run scripts/bootstrap_image_reading.py")
        filename = path.resolve().as_posix()
        if any(c in filename for c in '"\r\n'):
            raise ValueError("Invalid embedded model path")
        if platform == "windows":
            model_data.append(f'    {{"{key}",{index}}}')
            native_data.append(f'{index} RCDATA "{filename}"')
        else:
            length = path.stat().st_size
            model_data.append(f'    {{"{key}",{offset},{length}}}')
            offset += length
            escaped = filename.replace("\\", "\\\\")
            native_data.append(f'.incbin "{escaped}"')
    fields = 'int resource;' if platform == "windows" else 'std::size_t offset,size;'
    write_changed(out / "image_reading_assets.hpp", (
        '#pragma once\n#include <cstddef>\n#include <string_view>\nnamespace opt {\n'
        'struct ImageLanguage {std::string_view tag,model,label,scripts;};\n'
        'inline constexpr ImageLanguage image_languages[]{\n' + lang_data + '\n};\n'
        'struct ImageModel {std::string_view key;' + fields + '};\n'
        'inline constexpr ImageModel image_models[]{\n' + ',\n'.join(model_data) + '\n};\n}\n'
    ))
    if platform == "windows":
        write_changed(out / "windows-image-reading.rc", "#pragma code_page(65001)\n" + "\n".join(native_data) + "\n")
    else:
        write_changed(out / "macos-image-reading.s",
                      ".section __TEXT,__ocr_models,regular\n"
                      ".private_extern _opt_ocr_models\n_opt_ocr_models:\n" + "\n".join(native_data) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=("windows", "macos"), required=True)
    args = parser.parse_args()
    generate(args.out, args.model_dir, args.platform)
