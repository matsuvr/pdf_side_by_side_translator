# Implementation and validation status

The current product specification is [DESIGN.md](../DESIGN.md).

## Session-only safety changes

The safety changes tracked by [#11](https://github.com/matsuvr/pdf_side_by_side_translator/issues/11)
replace automatic persistence with memory-only state, bound translation/layout
caches, retain manual source edits independently of disposable caches, and
protect explicit exports and their input files. Current behavior and legacy
file handling are specified in [session-data.md](session-data.md).

Validation on 2026-11-12 used a Linux development host (GCC 14.2, SQLite 3.50.4,
pinned llama.cpp). macOS digest/UUID/stat interfaces were supplied by test-only
compatibility adapters outside the repository; Linux is not a new product target.
The headless CMake targets were compiled with ASan and UBSan.
LeakSanitizer could not run under this environment's ptrace constraints; the
SQLite constructor regression instead injects each allocation failure and checks
that SQLite's live allocation count returns to baseline.

### Results

- **14 C++ headless test executables passed** with ASan/UBSan: UTF-8, digest,
  file identity, safe PDF reads, planner, attempts, shortcut policy, PDF delivery,
  layout, session source ownership, Store, cache identity, I/O and diagnostics.
- **Two Python CTest entries passed** (dependency-audit and publication-audit
  regression suites). The full headless CTest invocation had **16 passes out of
  17 entries**; `pinned_third_party_notices` could not run because the complete
  bootstrap dependency/license input tree was not installed on this host.
  The committed dependency lock and notices still match the hashes in
  `deps/licenses.lock.json`; neither dependencies nor their notices were changed.
- The actual pinned PLaMo Q4_K_M fixture, verified against the lock's SHA-256,
  passed `inference_probe --prepare-only` with ASan/UBSan: all **14 official
  tokenizer reference cases**, reserved marker handling, **six U+EE00 edge
  cases**, an 8 MiB context-overflow input, and preparation cancellation.
  Two ordinary generation smoke cases completed; the full legacy generation
  probe was not completed and is not claimed as a pass.
- PDF service code was compiled against the **pinned PDFium public headers**.
  Supplemental Linux runtime checks used **PDFium 159.0.8152.0**, from the
  [chromium/8152 test binary](https://github.com/bblanchon/pdfium-binaries/releases/tag/chromium/8152).
  Its archive SHA-256 matched the published asset digest
  `384ab5c51882408a143a21f9c14dcd2caf1f75280ae41c554d0489b46c3dba9f`.
  This test binary is not the project's pinned native build and is not a
  dependency upgrade or distribution artifact.
- That PDF runtime passed async rendering/search/replacement, ResourceLimited
  extraction followed by working raster rendering, the 300-page/large/complex/
  password/Unicode-path stress suite, **nine layout fixtures**, and external
  truncate followed by reopening another document. A **74,036-character page**
  still extracted fully, including interruption and resumption.
- The new real-parser input-mutation regression recovered at all **three block
  reads inside `FPDF_LoadPage()`**. The pinned source already returns failure
  from `CPDF_SyntaxParser::ReadStream` on a failed block read. An older available
  PDFium 156.0.8076.0 instead terminates at a CHECK, so that old runtime must not
  be used as a substitute for the fixed pinned dependency in this regression.
- `inference_service_test` passed with the pinned llama libraries and the
  supplemental PDF runtime, covering ownership, cancellation, superseding and
  stale responses. GUI/controller/canvas code also passed a Linux wxWidgets
  header syntax check; this is not native Windows/macOS GUI execution.

### Resource observations

The original standalone 10 MiB UTF-8 validation reproducer fell from
**405,076 KiB / 1,357 ms** to **11,648 KiB / 17 ms** (maximum process RSS / elapsed
validation time). An allocation-counting unit test also verifies zero heap
allocations for validation of an 8 MiB input. These figures describe that
reproducer, not the full model-loaded application.

A separate native PDFium probe rendered distinct 1024×1024 RGB images into one
reused 512×512 bitmap, without the GUI tile cache. Across four passes, 40 images
retained about 126.5 MiB RSS without limited caching and at most 108.4 MiB with it;
80 images retained about 246.9 MiB without and at most 108.4 MiB with it.
The pinned PDFium flag controls a **100 MiB per-page image cache**, and the app
retains up to eight ordinary page handles. It does not impose a process-wide
100 MiB cap or bound a single image's decoding peak. Model memory, parser/GUI
allocations and allocator retention remain distinct from cache budgets.

### Remaining platform verification

Run the normal pinned-dependency CTest presets and native GUI checks on both
Windows and macOS before building a release. In particular, exercise the
Windows 4 GiB mapping path, file identity/replace behavior, and the GUI close /
failed-export retry flow. Metal and final packaged binaries were not tested in
this Linux session. The historical release results below do not substitute for
verification of these new changes.

## v0.1.1

- Product name: Offline PDF Translator.
- Translation follows the currently displayed page. The all-page command,
  keyboard binding, background scheduler and batch-only attempt state are removed.
- Model settings select the GGUF. Device selection and saved CPU overrides are removed.
- Standard and easy Japanese use the pinned official PFML3 templates.
- macOS support is Apple Silicon, deployment target 14.0.
- Windows x64 remains a source target. The published v0.1.0 release records its
  Windows native build, 24 tests and model/GUI checks. A v0.1.1 Windows binary
  requires a Windows build host; this release is built on macOS arm64.

See [v0.1.1 release validation](releases/v0.1.1.md) for current checks.

## Historical records

The individual milestone documents record the revision and scope checked at
that time. References to all-page translation, editable source overrides,
manual CPU selection and macOS x86_64 belong to those historical checks.
Current user instructions are in [README.md](../README.md).

- [Translation](translation.md)
- [Layout](layout.md)
- [Persistence](persistence.md)
- [Native fonts](native-fonts.md), [input](native-input.md), [text](native-text.md)
- [Stress](stress.md), [resources and diagnostics](resources-and-diagnostics.md)
- [Distribution](distribution.md)
- [Historical Intel/Rosetta checks](macos-x64.md)
