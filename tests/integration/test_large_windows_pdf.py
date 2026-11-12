"""Read the fixture's cross-reference table from a sparse offset above 4 GiB."""
from pathlib import Path
import json
import re
import subprocess
import sys
import tempfile


def extract(probe, path):
    return json.loads(subprocess.check_output([probe, path], timeout=30))


def main():
    probe, reader, source = sys.argv[1:]
    data = Path(source).read_bytes()
    trailer = re.search(rb"startxref\s+(\d+)\s+%%EOF\s*$", data)
    xref = int(trailer[1])
    assert data[xref:].startswith(b"xref")
    offset = (1 << 32) + 4096
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "large.pdf"
        path.touch()
        # Mark sparse before extending, even on filesystems that reject it.
        subprocess.run(["fsutil", "sparse", "setflag", path], check=True)
        with path.open("wb") as output:
            output.write(data[:xref])
            output.seek(offset)
            output.write(data[xref:trailer.start(1)])
            output.write(str(offset).encode("ascii") + b"\n%%EOF\n")
        subprocess.run([reader, path], check=True, timeout=30)
        expected = extract(probe, source)
        if not expected["pages"][0]["text"] or extract(probe, path) != expected:
            raise RuntimeError("Large mapped PDF did not reproduce the original page text")


if __name__ == "__main__":
    main()
