# Translation implementation evidence

Validated on macOS arm64 on 2026-11-10. Windows and Intel Mac remain untested.

The real Engine is constructed, loaded, used and destroyed on the inference
worker. The GUI never calls llama APIs. Model loading does not block PDF work.
The worker holds one latest plan/execution and rejects duplicate or superseded
executions. A failed automatic Metal load rebuilds once with CPU; backend labels
come from the device actually selected (`MTL` in the pinned upstream).

All 14 full official PFML3 token-vector reference cases match. The real Metal
probe also verified cancellation, output-limit classification, context bounds,
state reset and rejection of a literal reserved marker. The source tokenizer's
`parse_special=false` does not make every reserved marker ordinary text, so such
input gets an explicit unsupported-input reason.

`translation_probe` sends the original P01 PDF through PdfService, layout,
PageAttempt, ChunkPlanner and InferenceService. CPU and Metal both completed all
three blocks, preserving the number 3, the network-use negation and paragraph
order. The Japanese output was inspected. The model translated “re-enter” as
“再入力”; this tests application source preservation rather than a guarantee of
literal terminology in the generated translation. Per-chunk generation on this
short fixture was about 0.08–0.19 seconds, excluding model load; these are not
general performance benchmarks.

The native app was opened through its model-settings dialog, loaded the local
GGUF, opened P01, and automatically displayed the same completed Japanese page
beside the PDF. The initial equal sash is visually confirmed. The native text
adapter changes a suffix while retaining native selection and pixel viewport;
long streaming, selection and scroll acceptance still needs M5 coverage.

Eleven CTest cases pass. New cases cover OS SHA-256 and typed canonical keys,
same-open-file hashing across path replacement, UTF-8 and context-budget chunk
coverage, forced-attempt resume, one-time split replacement, stale event rejection,
worker ownership, latest request handling, asynchronous blocked model loading,
and a failed fake Metal load followed by successful CPU reconstruction. Fake
backends are test injection only; product translation always uses the real engine.

Page attempts record every source interval, leaf status and replaced parent.
Forced attempts retain cache bypass on resume; ungenerated leaves cannot count
as complete. See [persistence evidence](persistence.md) for the subsequent I/O,
semantic cache, batch and export implementation.

## Real-context CPU fallback check

The opt-in `translation_probe ... auto P01-simple.pdf fail-after-metal` adapter
created an actual Metal model/context, injected a failure immediately after
that construction, then exercised the production service's unload and CPU
rebuild path. The resulting backend was CPU, and all three P01 text blocks
completed with Japanese output (about 0.07–0.15 s per chunk in this run).
The injected failure is available only in the test executable. This verifies
cleanup/rebuild with actual resources; it does not reproduce a physical GPU
or driver failure. The normal Metal and forced CPU paths were also exercised.
