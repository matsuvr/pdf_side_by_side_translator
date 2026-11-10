# Implementation and validation status

The current product specification is [DESIGN.md](../DESIGN.md).

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
