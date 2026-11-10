# PDF stress and delivery evidence

The original fixtures now cover P01–P12 from DESIGN.md. `generate_fixtures.py`
creates the basic documents; `generate_unicode_fixture.py` creates the original
embedded geometric font; `generate_stress_fixtures.py` creates large/complex,
password and native-path cases. Expected text is authored in the generator
manifests, independently of the layout engine. Fixture generation is development
work only. The password PDF deliberately uses legacy 40-bit test encryption and
contains no real private data; the application does not implement that encryption.

The PDF worker now extracts at most 2,048 character records or roughly 10 ms per
step, retaining its cursor between steps. Pending views and visible-page source
requests can take priority. Searches use the latest query generation. There is
only one partial glyph buffer, and page/text handles remain under the eight-page
LRU. PDF parsing and individual image operations are cooperative upstream work;
there is no promise that every API call finishes within 10 ms.

Raw GUI delivery has independent limits: 24 MiB for pixels and 32 MiB for pending
layouts, plus at most 64 messages and one pending wake. A dropped payload is
acknowledged so the current view/extractor can retry it. The native bitmap cache
uses 96 MiB, leaving room for the active 1 MiB render tile and conversion buffers
within the approximate 128 MiB raster budget. Layout size accounting includes
vector capacities and nested source mappings. A single unusually large working
page can exceed the layout-cache target; multiple such pending pages are not
retained together.

On macOS arm64, the real stress probe verified:

- 300 pages with 30,000 × 30,000 point MediaBoxes, rendered into 60,000 × 60,000
  virtual views using bounded 512-pixel tiles rather than whole-page bitmaps.
- A 1,000-line dense source page, cooperative extraction, visible-page priority,
  and subsequent completion without losing the final source row.
- LRU eviction across more than eight pages: the observed peak was eight handles.
- A 24,000-path page with an original 1,024 × 1,024 procedural image, followed by
  a change of zoom/rotation while rendering.
- Missing and wrong passwords, cancellation, and a subsequent correct password.
- Opening and extracting a PDF through a long nested Japanese/space path well
  beyond 260 UTF-8 bytes. Windows extended-path handling is implemented but has
  not yet been executed on Windows.

One recorded run reported a first large-page tile in 28.9 ms, maximum tile payload
1,048,576 bytes, 21 render slices with three cooperative continuations, maximum
render-call slice 10.18 ms, and 42 text slices with 38 continuations. These numbers
are a local fixture measurement, not general performance guarantees.

Twenty-one CTest cases pass. The Unicode integration case also checks that
PDFium's co-located `fi` pieces form one selectable painted glyph, and that
supplementary Unicode retains both original character indices. Non-finite glyph
geometry is excluded before sorting/hit testing. Long native translation viewport
tests and Windows/Intel execution remain separate acceptance work.
