# Contributing

Keep the four ownership domains separate: GUI, PDFium, llama and I/O/SQLite.
Work crossing them must carry its document/page/model/source/attempt identity.
Do not add runtime downloads, an inference server, telemetry or a mandatory
helper executable to the ordinary reading/translation path.

Run the checks relevant to a change and the existing CTest suite. Native text,
IME, DPI and actual-model claims need corresponding native/real-model evidence;
fake backends are useful for races but do not prove model translation quality.
Use original, redistributable fixtures rather than private or copied PDFs.

Pin dependency changes, review the actual static link closure and complete
license texts, regenerate `THIRD_PARTY_NOTICES.txt` and its input lock, and
repeat final runtime dependency inspection. Do not redistribute GGUF weights
under this project's MIT License. Model and generated-output terms remain
separate; see the included PLaMo contract and official model source.

Build instructions are in [docs/development.md](docs/development.md).
