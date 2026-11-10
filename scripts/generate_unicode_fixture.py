#!/usr/bin/env python3
"""Original Unicode PDF fixtures; requires deps/fixture-test-tools.txt.

The embedded font's geometric glyphs are drawn here from scratch. No glyph
outline, font byte, document or test expectation comes from another app.
"""
import io
import json
import struct
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from common import ROOT
from pdf_fixture_writer import objects_pdf, stream


def unicode_map(pairs):
    lines = ["/CIDInit /ProcSet findresource begin", "12 dict begin begincmap",
             "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def",
             "/CMapName /OriginalFixtureUnicode def /CMapType 2 def",
             "1 begincodespacerange <0000> <FFFF> endcodespacerange",
             f"{len(pairs)} beginbfchar"]
    lines += [f"<{cid:04X}> <{text.encode('utf-16-be').hex()}>" for cid, text in pairs]
    lines += ["endbfchar endcmap CMapName currentdict /CMap defineresource pop end end"]
    return "\n".join(lines).encode()


def original_font(characters):
    names = [".notdef"] + [f"fixture{n:03d}" for n in range(len(characters))]
    font = FontBuilder(1000, isTTF=True)
    font.setupGlyphOrder(names)
    font.setupCharacterMap({ord(c): names[i + 1] for i, c in enumerate(characters)})
    glyphs, metrics = {}, {}
    for index, name in enumerate(names):
        char = characters[index - 1] if index else "?"
        pen = TTGlyphPen(None)
        if char not in (" ", "\u200d", "\u00ad"):
            # Small original geometric marks distinguish each glyph by bars.
            # They test PDF mappings, not the appearance of a reading font.
            if char == "\u0301":
                pen.moveTo((-350, 680)); pen.lineTo((-100, 850)); pen.lineTo((-30, 760)); pen.lineTo((-280, 620)); pen.closePath()
            else:
                for bar in range(5):
                    height = 250 + ((ord(char) >> bar) & 1) * 400
                    x = 40 + bar * 95
                    pen.moveTo((x, 0)); pen.lineTo((x + 45, 0)); pen.lineTo((x + 45, height)); pen.lineTo((x, height)); pen.closePath()
        glyphs[name] = pen.glyph()
        metrics[name] = (0 if char in ("\u0301", "\u200d", "\u00ad") else 600, -350 if char == "\u0301" else 0)
    font.setupGlyf(glyphs)
    font.setupHorizontalMetrics(metrics)
    font.setupHorizontalHeader(ascent=900, descent=-200)
    font.setupNameTable({"familyName": "Offline Mapping Fixture", "styleName": "Regular",
                         "uniqueFontIdentifier": "OriginalOfflineMappingFixtureV1",
                         "fullName": "Offline Mapping Fixture Regular", "psName": "OfflineMappingFixture-Regular",
                         "version": "Version 1.0"})
    font.setupOS2(sTypoAscender=900, sTypoDescender=-200, usWinAscent=900, usWinDescent=200)
    font.setupPost(); font.setupMaxp()
    font.font["head"].created = font.font["head"].modified = 2082844800  # fixed 1970-01-01
    font.font.recalcTimestamp = False
    output = io.BytesIO(); font.font.save(output)
    return output.getvalue(), metrics, names


def main():
    directory = ROOT / "tests/fixtures"
    text = "of\ufb01ce café e\u0301 👩\u200d💻 hy\u00adphen re-enter"
    chars = sorted(set(text), key=ord)
    binary, metrics, names = original_font(chars)
    ids = {c: i + 1 for i, c in enumerate(chars)}
    encoded = b"".join(struct.pack(">H", ids[c]) for c in text).hex().encode()
    widths = " ".join(str(metrics[name][0]) for name in names[1:])
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>",
               b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
               b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
               stream(b"BT /F1 22 Tf 48 700 Td <" + encoded + b"> Tj ET"),
               b"<< /Type /Font /Subtype /Type0 /BaseFont /OfflineMappingFixture-Regular /Encoding /Identity-H /DescendantFonts [6 0 R] /ToUnicode 8 0 R >>",
               f"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /OfflineMappingFixture-Regular /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor 7 0 R /CIDToGIDMap /Identity /DW 600 /W [1 [{widths}]] >>".encode(),
               b"<< /Type /FontDescriptor /FontName /OfflineMappingFixture-Regular /Flags 4 /FontBBox [-400 -200 600 900] /ItalicAngle 0 /Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 /FontFile2 9 0 R >>",
               stream(unicode_map([(ids[c], c) for c in chars])), stream(binary)]
    objects_pdf(directory / "P06-unicode.pdf", objects)
    japanese = "日本語の読書と翻訳"
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>",
               b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
               b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
               stream(b"BT /F1 30 Tf 48 700 Td <" + japanese.encode("utf-16-be").hex().encode() + b"> Tj ET"),
               b"<< /Type /Font /Subtype /Type0 /BaseFont /HeiseiKakuGo-W5 /Encoding /UniJIS-UTF16-H /DescendantFonts [6 0 R] /ToUnicode 8 0 R >>",
               b"<< /Type /Font /Subtype /CIDFontType0 /BaseFont /HeiseiKakuGo-W5 /CIDSystemInfo << /Registry (Adobe) /Ordering (Japan1) /Supplement 5 >> /FontDescriptor 7 0 R /DW 1000 >>",
               b"<< /Type /FontDescriptor /FontName /HeiseiKakuGo-W5 /Flags 4 /FontBBox [-100 -200 1100 1000] /ItalicAngle 0 /Ascent 880 /Descent -120 /CapHeight 700 /StemV 80 >>",
               stream(unicode_map([(ord(c), c) for c in sorted(set(japanese))]))]
    objects_pdf(directory / "P06-japanese-fallback.pdf", objects)
    expected = {"origin": "Original geometric font and authored Unicode expectations",
                "P06-unicode.pdf": {"page_count": 1, "contains": ["office", "café", "e\u0301", "👩\u200d💻", "hyphen", "re-enter"], "excludes": ["\ufffd", "\ufb01", "\u00ad"], "surrogate_mapping": ["👩", "💻"], "paint_cluster_mapping": ["fi"]},
                "P06-japanese-fallback.pdf": {"page_count": 1, "contains": [japanese]}}
    (directory / "unicode-ground-truth.json").write_text(json.dumps(expected, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
