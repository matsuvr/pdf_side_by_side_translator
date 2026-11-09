# M0 evidence — macOS arm64

Validation date: 2026-11-10. This is a dependency probe, not the finished product.

## Reproducible configuration

- Sources are pinned in `deps/third_party.lock.json`.
- PDFium: `96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4`.
- wxWidgets 3.3.4: `6a42a4b999b127cf28a8b5fe686da6b8ca93a9b8`.
- llama.cpp: `a4d4f2fd4ec85ec6a0a9ece4870416c1b6cc24f5`.
- PDFium's DEPS provides LLVM `llvmorg-24-init-14453-ge063be79-1`.
  CMake uses the same compiler distribution and its `ld64.lld`.
- Xcode 27.2 (27B52), SDK 27.2, arm64 deployment target 14.0.
  Only the current host OS was executed; macOS 14 execution is unverified.
- C++20, no LTO, static application dependencies, embedded ICU data and Metal
  source, AGG renderer, V8/XFA disabled.
- PDFium's supported `use_allocator_shim=false` and
  `use_partition_alloc_as_malloc=false` prevent replacing the application's
  process-wide allocator. PDFium still uses its private Chromium C++ runtime.

The generated `.work/pdfium/macos-arm64-release/pdfium-build-manifest.json`
records GN's resolved archives, compiler, SDK, GN arguments, DEPS hash and
actual dependency revisions. It is machine-local, not a distributable file.
GN's libc++ archives are thin archives; Apple's default linker does not read
them. The pinned LLVM linker is a **build tool**, not a runtime dependency.

## Results

| Check | Observed result |
| --- | --- |
| GN-built PDF consumer | 3 pages, 222 first-page characters, 5,976 nonwhite pixels |
| CMake-built PDF consumer | Same extraction and raster counts; visually legible |
| Full PFML3 prompt token IDs | 14 official-reference cases matched |
| Real GGUF CPU decode | Completed; digits and negation preserved in the fixed sensor example |
| Real GGUF Metal decode | Completed; digits and negation preserved in the same example |
| Cancellation, output limit, context capacity | Distinct non-complete reasons |
| Reuse after interruption/limit | Independent input reproduced its baseline output |
| Reserved PFML3 marker in source | `ModelInputUnsupported`; not silently rewritten |
| UTF-8 unit checks | Split scalars, invalid continuations, overlong, surrogate, out-of-range checked |
| PE audit parser checks | Normal and delay imports, OS/redistributable distinction, truncation checked |
| Combined native `.app` | PDF on left and real Japanese translation on right visually verified |
| Relocation | Copied outside build directory and successfully launched |
| Native paths | PDF and GGUF opened through paths containing Japanese and spaces |
| Bundle font | Noto Sans CJK JP registered in process scope; Japanese text displayed |
| Mach-O dependency inspection | arm64; only OS frameworks/libraries; no external rpaths |
| Bundle signing | Development ad-hoc signature verified after relocation |

The GGUF SHA-256 is
`70891ec70dc7ab4205745a38307867c2881b99239f20327927633abfe4bc9ec6`.
The local model matched the pinned official Q4_K_M object. It is excluded from
Git and is not redistributed under the application's MIT license.

The probe translates “This application translates documents offline.” using
a Japanese verb meaning “converts”. Generation completion does not establish
perfect translation quality. The sensor example correctly retained “3” and
“does not use the network”.

The combined automated model probe (load, 14 tokenizer cases, multiple decodes
and interruption/reset checks) took 6.73 s on CPU and 1.02 s on Metal. These
are whole-probe durations, not tokens/s benchmarks. Reported maximum RSS was
about 1.24 GB and 0.83 GB respectively; OS memory accounting differs from
model/context allocation sizes.

## Remaining acceptance work

- Windows and Intel Mac native build/execution.
- Clean-machine, network-disabled first launch.
- Forced Metal initialization failure and CPU fallback injection.
- PDFium Japanese fallback-font callback and unembedded Japanese PDF fixtures.
- Product UI, four-thread service ownership and all M1–M6 behavior.
- Final resource/license inventory, icon, packaging, publication audit.
- Developer ID signing and notarization are not performed.

No R01–R10 release acceptance claim is made from this probe alone.
