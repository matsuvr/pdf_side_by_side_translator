> Historical validation through v0.1.0. Current behavior and release checks: [implementation status](implementation-status.md).

# macOS x86_64 cross-build and Rosetta evidence

The fixed PDFium, wxWidgets, llama/ggml, SQLite and application were compiled as
x86_64 on the Apple Silicon development Mac with LLVM
`llvmorg-24-init-14453-ge063be79-1`, Xcode/SDK 27.2 and deployment target 14.0.
The product and test programs linked successfully. Metal is disabled in this
target; CPU inference is the intended path.

The host had `LIBRARY_PATH=/usr/local/lib` and `CPATH=/usr/local/include`.
The nonexistent library directory caused a fatal warning in the GN-built host
NASM linker step. Clearing environment-only search overrides for child builds
fixed it without changing upstream sources or suppressing diagnostics. The
same isolation is part of the CMake presets. Visual Studio's required Windows
`LIB`/`INCLUDE` environment is retained.

All 23 CTest checks passed while executing the generated x86_64 programs under
Rosetta (12.10 s for the suite on this run). The actual model probe matched all
14 official-reference full prompt token vectors, completed the sensor/digits/
negation example, preserved cancellation/output/context-limit reasons and
reproduced its independent baseline after context reset. Whole-probe elapsed
time was 7.10 s, maximum RSS reported by `/usr/bin/time -l` was 760,664,064 bytes.
This is not a native Intel CPU performance measurement.

The x86_64 PDF-to-translation service probe completed all three text blocks of
P01, retained the digits/negation and paragraph order, and emitted Japanese.
The native text acceptance program also passed: 800 Japanese/emoji lines,
80 appends, selection/focus/pixel viewport retention, font/width logical-anchor
restoration and suffix replacement, using the actual Cocoa widget under
Rosetta.

GGML native tuning, AVX/AVX2/AVX-512, FMA/F16C, BMI2, AMX and SSE4.2 options are
explicitly disabled. PDFium's bundled codecs have their own guarded SIMD code;
these settings do not establish a whole-product SSE2-only hardware baseline.
Physical Intel hardware, older macOS, monitor-scale transitions and clean
machine offline first launch remain unverified. Rosetta success is not reported
as a native Intel hardware test.
