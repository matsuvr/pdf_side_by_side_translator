#!/usr/bin/env python3
"""Original large, complex, password and native-path PDF fixtures (stdlib only).

The legacy 40-bit encryption below is solely a tiny interoperability test for
password entry. It is not product encryption and must not protect real data.
"""
import hashlib
import json
import math
import struct
import zlib
from common import ROOT
from generate_fixtures import write_pdf
from pdf_fixture_writer import objects_pdf, stream


def fixture_md5(value):
    return hashlib.md5(value, usedforsecurity=False).digest()


def rc4(key, data):
    state = list(range(256)); j = 0
    for i in range(256):
        j = (j + state[i] + key[i % len(key)]) % 256
        state[i], state[j] = state[j], state[i]
    i = j = 0; output = bytearray()
    for byte in data:
        i = (i + 1) % 256; j = (j + state[i]) % 256
        state[i], state[j] = state[j], state[i]
        output.append(byte ^ state[(state[i] + state[j]) % 256])
    return bytes(output)


def password_pdf(path):
    assert rc4(b"Key", b"Plaintext").hex() == "bbf316e8d940af0ad3"
    padding = bytes.fromhex("28bf4e5e4e758a4164004e56fffa01082e2e00b6d0683e802f0ca9fe6453697a")
    def padded(text):
        return (text + padding)[:32]
    identifier = fixture_md5(b"OriginalOfflinePasswordFixtureV1")
    owner_key = fixture_md5(padded(b"owner-fixture"))[:5]
    owner = rc4(owner_key, padded(b"reader-fixture"))
    key = fixture_md5(padded(b"reader-fixture") + owner + struct.pack("<i", -4) + identifier)[:5]
    user = rc4(key, padding)
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Count 1 /Kids [4 0 R] >>",
               b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
               b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents 5 0 R >>",
               stream(b"BT /F1 18 Tf 48 700 Td (Password fixture: there are 11 samples.) Tj ET"),
               b"<< /Filter /Standard /V 1 /R 2 /Length 40 /P -4 /O <" + owner.hex().encode() + b"> /U <" + user.hex().encode() + b"> >>"]
    def encrypt(index, body):
        if index != 5:
            return body
        start = body.index(b"stream\n") + 7; end = body.rindex(b"\nendstream")
        object_key = fixture_md5(key + index.to_bytes(3, "little") + b"\0\0")[:10]
        return body[:start] + rc4(object_key, body[start:end]) + body[end:]
    trailer = b"/Encrypt 6 0 R /ID [<" + identifier.hex().encode() + b"> <" + identifier.hex().encode() + b">] "
    objects_pdf(path, objects, trailer, encrypt)


def complex_pdf(path):
    side = 1024
    pixels = bytes(180 + ((x * 7 + y * 13 + channel * 23) % 75)
                   for y in range(side) for x in range(side) for channel in range(3))
    commands = ["q 612 0 0 792 0 0 cm /Im0 Do Q", "0.16 0.34 0.55 rg"]
    for i in range(24000):
        x = 10 + (i * 37 % 590); y = 10 + (i * 53 % 710)
        dx = 3 + math.sin(i * 0.13) * 2; dy = 3 + math.cos(i * 0.17) * 2
        commands.append(f"{x} {y} m {x+dx:.3f} {y+dy:.3f} {x+dx*2:.3f} {y-dy:.3f} {x+dx*3:.3f} {y} c {x} {y+5} l f")
    commands += ["1 g 36 734 540 42 re f", "0 g BT /F1 18 Tf 48 750 Td (Complex paths and an original procedural image.) Tj ET"]
    contents = zlib.compress("\n".join(commands).encode(), 9)
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
               b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> /XObject << /Im0 6 0 R >> >> /Contents 4 0 R >>",
               stream(contents, b"/Filter /FlateDecode "), b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
               stream(zlib.compress(pixels, 9), f"/Type /XObject /Subtype /Image /Width {side} /Height {side} /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode ".encode())]
    objects_pdf(path, objects)


def main():
    directory = ROOT / "tests/fixtures"
    large = [[(48, 29900, 22, f"Large page {i+1:04d}"),
              (29700, 100, 18, "Lower-right corner.")] for i in range(300)]
    large[0][1:1] = [(48, 29800-i*20, 8, f"Incremental row {i+1:04d} contains original source for extraction and search.") for i in range(1000)]
    write_pdf(directory / "P09-large.pdf", large, page_size=(30000, 30000))
    complex_pdf(directory / "P10-complex.pdf")
    password_pdf(directory / "P11-password.pdf")
    relative = "P12 日本語 と 空白/長めのフォルダー 読書と翻訳の確認用/原文 PDF と 訳文.pdf"
    write_pdf(directory / relative, [[(48, 700, 18, "Native path fixture: 12 samples.")]])
    expected = {"origin": "Original generated stress and interoperability documents",
                "P09-large.pdf": {"page_count": 300, "media_box": [0, 0, 30000, 30000], "first_marker": "Large page 0001", "last_marker": "Large page 0300", "incremental_rows_on_first_page": 1000},
                "P10-complex.pdf": {"contains": "Complex paths and an original procedural image.", "path_count": 24000, "image_size": [1024, 1024]},
                "P11-password.pdf": {"password": "reader-fixture", "contains": "Password fixture: there are 11 samples.", "security": "Legacy fixture encryption only, no real data"},
                "P12": {"path": relative, "contains": "Native path fixture: 12 samples."}}
    (directory / "stress-ground-truth.json").write_text(json.dumps(expected, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
