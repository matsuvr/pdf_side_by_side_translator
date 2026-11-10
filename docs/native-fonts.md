# Native font and Unicode evidence

The PDF worker installs a small adapter through the pinned public
`FPDF_SYSFONTINFO` API. Japanese Shift-JIS charset requests use the bundled Noto
Sans CJK JP font bytes. Other requests delegate to PDFium's platform system-font
provider. The adapter reads OpenType tables with checked offsets, supplies the
whole file when requested, and releases the provider and bytes before the PDFium
library shuts down. No private PDFium API is used. Native text controls separately
register the same font for the process; neither path installs a system font.

`P06-japanese-fallback.pdf` is an original non-embedded Japanese CID-font document.
The render test confirms actual bundled-font mapping and byte-read callbacks,
then renders “日本語の読書と翻訳”. Its PNG was visually inspected: the Japanese
glyphs are present and legible. `P06-unicode.pdf` embeds an original tiny TrueType
font whose geometric outlines are generated in the fixture script. Its test
confirms that an embedded font is not replaced by the fallback. The fixture has
no copied third-party glyph outlines.

The real Unicode fixture exposed a defect that synthetic scalar tests did not:
the pinned PDFium reports supplementary characters from ToUnicode as adjacent
UTF-16 surrogate records. The layout adapter now joins a matching pair into one
UTF-8 scalar while preserving both PDF character indices in its source range.
Continuation records remain in the indexed glyph array but do not produce a
second character or highlight. Layout version 2 identifies this behavior.

The integration test verifies the actual “👩‍💻” sequence, combining accent,
precomposed accent, ligature expansion, soft-hyphen removal and visible-hyphen
preservation. It checks every UTF-8 mapping boundary and source-index range;
each supplementary scalar maps to two original PDF character records.

Eighteen CTest cases pass on macOS arm64. This includes both font paths and the
Unicode fixture mapping. The font callback has not yet been executed on Windows
or Intel Mac. Fixture generation requires development-only fontTools 4.61.1,
installed separately; the product and normal fixture consumers require no Python
or fontTools runtime.
