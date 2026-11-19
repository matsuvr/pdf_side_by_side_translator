#!/usr/bin/env python3
"""Prepare fixed OCR sources and English/Chinese weights for the experimental native build."""
from common import WORK, checkout, download

SOURCES = {
    "Tesseract": ("https://github.com/tesseract-ocr/tesseract", "64eab6c457b2337dd690746a5fde5c222b40d5f8"),
    "Leptonica": ("https://github.com/DanBloomberg/leptonica", "63aef18d98432b8582a1565e241f7bd2ee9cc8d9"),
}
MODELS = {
    "eng": "7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2",
    "chi_sim": "a5fcb6f0db1e1d6d8522f39db4e848f05984669172e584e8d76b6b3141e1f730",
    "chi_tra": "529c5b5797d64b126065cd55f2bb4c7fd7b15790798091b1ff259941a829330b",
}
REVISION = "87416418657359cb625c412a48b6e1d6d41c29bd"

if __name__ == "__main__":
    for name, (url, revision) in SOURCES.items():
        checkout({"url": url, "revision": revision}, WORK / "src" / name)
    for lang, digest in MODELS.items():
        name = lang + ".traineddata"
        download(f"https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/{REVISION}/{name}",
                 WORK / "resources/tessdata" / name, digest)
