import json
from pathlib import Path
import subprocess
import sys

probe, directory = sys.argv[1], Path(sys.argv[2])
manifest = json.loads((directory / "pdf-ground-truth.json").read_text())
for name, expected in manifest.items():
    if not name.endswith(".pdf"):
        continue
    actual = json.loads(subprocess.check_output([probe, str(directory / name)], text=True))
    pages = actual["pages"]
    if "page_count" in expected:
        assert len(pages) == expected["page_count"], name
    for index, page in enumerate(pages):
        text = page["text"]
        ordered = expected.get("ordered_markers", expected.get("reading_order", [[]] * len(pages))[index])
        previous = -1
        for marker in ordered:
            position = text.find(marker, previous + 1)
            assert position > previous, (name, index, marker, text)
            previous = position
        for marker in expected.get("contains", []):
            assert marker in text, (name, marker, text)
        for marker in expected.get("excludes", []):
            assert marker not in text, (name, marker, text)
        for marker, kind in expected.get("block_kinds", {}).items():
            matching = [b for b in page["blocks"] if marker in b["text"]]
            assert len(matching) == 1 and matching[0]["kind"] == kind, (name, marker, matching)
        if index in expected.get("no_text_pages", []):
            assert not text and page["availability"] == 1, (name, index, page)
    print(name, "reading order/crop/rotation/text availability passed")
