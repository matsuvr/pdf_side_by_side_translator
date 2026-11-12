# Implementation and validation status

The current product specification is [DESIGN.md](../DESIGN.md).

## Privacy and responsiveness follow-up

The follow-up audit started from `a5c22177bfa8e037f1f794695d0b400d6399e8cc`
after the session-only and bundled-model changes were merged. It retains those
policies and the Release-test assertion fix. Four additional problems were
reproduced and addressed:

| Issue | Evidence and resulting behavior |
| --- | --- |
| [#14 — export access controls](https://github.com/matsuvr/pdf_side_by_side_translator/issues/14) | The old create-and-rename sequence changed a `0600` destination to `0644` under umask `022` in the Linux reproducer. Text and SQLite exports now share exclusive, private staging; replacement preserves the destination's access controls and Windows EFS encryption. Failed writes/publication keep the old destination. Volumes explicitly reporting no persistent ACL support still allow ordinary exports. |
| [#15 — mutable GGUF mappings](https://github.com/matsuvr/pdf_side_by_side_translator/issues/15) | Truncating a test-owned model copy during generation produced an ASan BUS error in the old CPU tensor access. Resident model buffers remove that mapping dependency; the same regression now returns `ModelUnavailable` with partial text and rejects reuse. No model disk snapshot is introduced. |
| [#16 — nonregular inputs](https://github.com/matsuvr/pdf_side_by_side_translator/issues/16) | A FIFO with no writer blocked the old POSIX open before its file-type check. Nonblocking open followed by verification of the open object now returns an ordinary error. Regular files and their symlinks remain supported. |
| [#17 — search work and cancellation](https://github.com/matsuvr/pdf_side_by_side_translator/issues/17) | Repeated-prefix searches could take seconds and closing the search UI did not cancel worker work. Forward/reverse linear search avoids whole-page copies, checks cancellation during preprocessing and scanning, and receives cancellation from the controller. |

### Validation of this follow-up

Validation on 2026-11-12 used the Linux auxiliary environment, test-only
CommonCrypto/UUID/stat adapters, pinned SQLite 3.50.4 and pinned llama.cpp
`a4d4f2fd4ec85ec6a0a9ece4870416c1b6cc24f5`. It does not add a Linux product target.

- **All 17 C++ headless tests passed in both Release and ASan/UBSan builds.**
  The new export test covers private staging, retained permissions, Unicode
  paths, failed publication, bad streams, a real OS write failure, and cleanup.
  Store/I/O tests also cover replacement permissions. Search tests compare
  4,000 inputs in both directions and both case modes with the old semantics,
  including overlaps, UTF-8 byte positions and embedded NULs.
- **The complete headless CTest run passed 19 of 20 entries in each build.**
  The two Python regression entries passed. `pinned_third_party_notices` stops
  at the absent bootstrap input `.work/pdfium-workspace/pdfium/LICENSE`.
  Dependency locks and notices are unchanged; this is not recorded as an
  all-green CTest run.
- The actual official PLaMo Q4_K_M fixture, SHA-256
  `70891ec70dc7ab4205745a38307867c2881b99239f20327927633abfe4bc9ec6`, passed
  `inference_probe --prepare-only` with ASan/UBSan, including all 14 reference
  tokenizer cases, reserved marker/U+EE00 handling, bounded preparation and
  cancellation. The new `model_input_change_test` uses its own model copy;
  it reproduced the old BUS error and passed with the new engine.
- Updated `pdf_async_test` and `pdf_stress_test` passed with ASan/UBSan and
  `NDEBUG`. The service was compiled against pinned PDFium public headers
  `96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4`; execution used the supplemental
  `chromium/8152` runtime described below, whose archive digest was rechecked.
  Coverage includes cancellation followed by a new search, normal rendering,
  document replacement, rotations, constrained extraction followed by raster
  rendering, 300 pages, large pages, password/Unicode paths, and input
  truncation followed by recovery. The stress run reported 8 cached pages
  and 1,048,576 tile bytes at peak.
- Windows branches were compiled and linked with actual MinGW-w64 12 headers,
  GCC 14.2, `advapi32` and `bcrypt`: both the atomic export test and the I/O
  integration test with pinned SQLite produced Windows x64 executables.
  This is cross-compilation, not execution on Windows. The macOS file-security
  API signatures were checked against Apple's published headers; macOS SDK
  compilation and execution were not available here.
- LeakSanitizer was retried and cannot run under the host's ptrace constraints.
  ASan/UBSan results therefore use `detect_leaks=0`; they are not a leak-free
  certification. The SQLite allocation-failure regression still passes.

### Responsiveness measurements

The same optimized in-memory search probe, using a repeated-prefix mismatch
over **1,000,000 bytes**, measured forward search at **5,830.68 ms before /
14.1074 ms after**, and reverse search at **5,796.97 ms before / 3.66176 ms
after**. These are synthetic matcher measurements, not whole-app timings or
a claimed speedup for every PDF.

A separate successful load-only ABBA comparison with the actual model and
ASan/UBSan CPU builds measured model load times of 6.50208/6.89344 seconds
before and 7.81659/7.47323 seconds after: approximately **0.95 seconds longer
on average**. Peak process RSS differed by less than 1 MiB in those runs.
This does not predict native Release/Metal performance; resident weights may
change physical-memory use and startup behavior on those platforms.

Native Windows/macOS CTest, ACL/EFS behavior, GUI close/cancel/export flows,
Metal inference and packaged application checks remain to be run on those
hosts. The earlier validation records below describe their own revisions.

## Bundled model startup and old-data disposal

Startup now reads `plamo-3-translate-0.8b-Q4_K_M.gguf` beside the Windows
executable or beside the macOS `.app` bundle. Only a missing companion opens
the file picker. The initial PDF opens after that picker returns, avoiding a
second password modal during model selection. The model path is not persisted.

Old automatic state/cache files and logs in the default app-data directory are
discarded by the I/O owner before it publishes a fresh in-memory session.
No old database is opened, migrated or backed up. Known files and then empty
directories are removed; unrelated files and directory-link targets are left
alone. See [session-data.md](session-data.md) for the exact scope.

Follow-up validation used the Linux auxiliary environment described below:

- **All 16 C++ headless tests passed with ASan/UBSan**, including new companion
  path tests, legacy-file disposal tests, and the updated I/O startup test.
  Coverage includes Unicode/spaces in paths, decoy models in the working
  directory or inside `.app`, missing versus present-but-invalid model files,
  corrupt old databases, repeated cleanup, retained exports, symlink targets,
  and an actual permission-denied removal followed by retry.
- The complete headless CTest run had **18 passes out of 19 entries**. The two
  Python regression entries passed. `pinned_third_party_notices` still stops at
  the missing bootstrap PDFium `LICENSE` input; dependency locks and notices
  are unchanged by this follow-up.
- GUI controller/app entry code passed the wxWidgets-header syntax check,
  and `git diff --check` passed. Windows/macOS packaged GUI startup and the
  Windows junction branch still need native execution; these Linux checks do
  not claim that coverage.

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
