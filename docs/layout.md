# Reading order and source mapping

`LayoutEngine` runs on the PDF worker and consumes immutable glyph records. It
retains PDFium's original indices and geometry, including generated whitespace
and Unicode mapping failures. Reading text is separate from editable translation
input. Editing translation input never modifies the PDF selection or search
index.

The layout transform is created with user rotation zero in a stable normalized
page space. Crop-excluded glyphs remain in the raw records but are omitted from
reading text. Changing zoom, monitor scale, scroll, or user rotation does not
rebuild paragraph identities.

Processing groups text direction and baselines, separates distant line
fragments, and recursively splits horizontal bands and local column gutters.
Spanning rows divide the local column regions. The mixed fixture checks a
full-width heading, upper columns, a shared caption, and lower columns.

Thresholds live in `LayoutOptions` and are ratios of font height, rather than
screen pixels. The PDF adapter scales PDFium's untransformed `Tf` size by the
effective character matrix's vertical vector, including text, page and enclosing
Form transforms. Without that conversion, a large `Tf` scaled down for painting
merges separate lines and interleaves their characters before translation.
The original fixture exposed another distinction: ink
bounding boxes contain side bearings and must not be used as glyph advances.
Word spacing now prefers explicit/generated PDF space records between nearby
original indices. A 0.35-font-height gap is only a fallback when those records
cannot establish adjacency.

Known Latin ligatures expand with mappings back to the original glyph. Soft
hyphens are omitted from restored text; visible hyphens are retained. Unicode
is not globally normalized with NFKC. Combining characters and emoji remain
UTF-8. Line records provide an index for hit testing; selection is a logical
reading-text byte range mapped back to individual source glyphs.

The layout engine recognizes isolated short equations and aligned numeric tables.
Their text is included at its reading position in the full-page prompt.
The conservative table rule requires at least three
aligned columns and three rows with repeated numeric cells. Ambiguous text is
kept as independent prose rather than discarded. Captions remain translatable.
Complex RTL, vertical writing and arbitrary reading order are not guaranteed;
the editable source is the recovery path.

## Current verification

- Original P01/P02/P03: single column, two columns, mixed local columns.
- P04: nonzero CropBox; hidden text is not copied or searched.
- P05: all intrinsic rotations, including composition with user rotation.
- P07: equation, caption, numeric table and subsequent prose retain order.
- P08: image-only page remains viewable and has no text layer.
- P13: identical prose and columns under text/page/Form/nested scaling and all
  four intrinsic rotations produce exactly the same reading text and mappings.
- Synthetic Unicode records: ligature, soft hyphen, combining mark and emoji.
- Search: case toggle, backwards traversal, query generations and replacement.
- Geometry: independently authored fixture point/pixel values and inverse maps.

Native selection highlight alignment, IME behavior, editing persistence and
search navigation through the complete GUI still require the M4/M5 acceptance
pass. The source editor currently stores session overrides; durable state is
implemented in M4.
