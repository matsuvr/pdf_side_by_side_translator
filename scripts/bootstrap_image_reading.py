#!/usr/bin/env python3
"""Prepare fixed OCR sources and all PLaMo-language weights for the experimental native build."""
from concurrent.futures import ThreadPoolExecutor
from common import WORK, checkout, download, sha256
from image_reading_assets import catalog, strip_script_dictionary

SOURCES = {
    "Tesseract": ("https://github.com/tesseract-ocr/tesseract", "64eab6c457b2337dd690746a5fde5c222b40d5f8"),
    "Leptonica": ("https://github.com/DanBloomberg/leptonica", "63aef18d98432b8582a1565e241f7bd2ee9cc8d9"),
    "CLD2": ("https://github.com/CLD2Owners/cld2", "b56fa78a2fe44ac2851bae5bf4f4693a0644da7b"),
}
REVISION = "87416418657359cb625c412a48b6e1d6d41c29bd"

if __name__ == "__main__":
    for name, (url, revision) in SOURCES.items():
        checkout({"url": url, "revision": revision}, WORK / "src" / name)
    _, models = catalog()
    def fetch(row):
        lang, digest, size, output_digest = row
        name = lang + ".traineddata"
        target = WORK / "resources/tessdata" / name
        if target.is_file() and sha256(target) == output_digest:
            return
        source = WORK / "downloads/ocr-original" / name if lang.startswith("script/") else target
        download(f"https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/{REVISION}/{name}", source, digest)
        if source.stat().st_size != int(size):
            raise RuntimeError("Unexpected model size: " + lang)
        if source != target:
            target.parent.mkdir(parents=True, exist_ok=True)
            tmp = target.with_suffix(".partial")
            tmp.write_bytes(strip_script_dictionary(source.read_bytes()))
            if sha256(tmp) != output_digest:
                tmp.unlink()
                raise RuntimeError("Derived OCR SHA-256 mismatch: " + lang)
            tmp.replace(target)
    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(fetch, models))
