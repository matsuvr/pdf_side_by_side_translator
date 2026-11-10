# Implementation and validation status

This file records evidence as work progresses. A configured target is not a
validated release. DESIGN.md is the acceptance specification.

## Environment

- Development host: macOS arm64, Xcode 27.2, SDK 27.2.
- Windows x64 execution: not yet performed. macOS x64 was cross-built and
  executed under Rosetta on Apple Silicon; physical Intel execution is unverified.
- No existing application's source, resources, settings or fixtures are used.

## Milestones

| Milestone | Status | Evidence |
| --- | --- | --- |
| M0 static dependency probes | Mac arm64 verified; Mac x64 cross-build and Rosetta CPU verified; Windows pending | [ARM evidence](m0-macos-arm64.md), [x64 evidence](macos-x64.md) |
| M1 reading interface | Implemented on Mac arm64; broader fixture checks remain | Native page navigation, 200% zoom, rotation, fit-page and equal initial panes visually verified; async PDF tests passed |
| M2 layout, selection, search | Implemented on Mac arm64; broader cross-platform GUI acceptance remains | [Layout and fixture evidence](layout.md); worker search, mapping, crop and rotation tests pass; durable overrides verified in M4 |
| M3 real-model translation | Implemented on Mac arm64; broader native race/scroll checks continue in M5 | [Translation evidence](translation.md); native Metal current-page translation and CPU/Metal service probes verified |
| M4 persistence and batch | Implemented on Mac arm64; native position/font-resize and large-document stress continue in M5 | [Persistence evidence](persistence.md); 15 tests, native batch and source restoration verified |
| M5 native behavior | Implemented and checked on Mac arm64; Windows/IME/DPI matrix remains unverified | [Fonts](native-fonts.md), [input](native-input.md), [text](native-text.md), [stress](stress.md), [embedded resources/diagnostics](resources-and-diagnostics.md) |
| M6 distribution validation | Pending | |

No clean-machine offline, Windows, Intel Mac, signing or notarization claim
is made until corresponding results are recorded here.
