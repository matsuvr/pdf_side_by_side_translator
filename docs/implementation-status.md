# Implementation and validation status

This file records evidence as work progresses. A configured target is not a
validated release. DESIGN.md is the acceptance specification.

## Environment

- Development host: macOS arm64, Xcode 27.2, SDK 27.2.
- Windows x64 and macOS x64 execution: not yet performed.
- No existing application's source, resources, settings or fixtures are used.

## Milestones

| Milestone | Status | Evidence |
| --- | --- | --- |
| M0 static dependency probes | Mac arm64 probe verified; cross-platform acceptance pending | [Evidence](m0-macos-arm64.md) |
| M1 reading interface | Implemented on Mac arm64; broader fixture checks remain | Native page navigation, 200% zoom, rotation, fit-page and equal initial panes visually verified; async PDF tests passed |
| M2 layout, selection, search | Implemented; native GUI acceptance and durable overrides remain | [Layout and fixture evidence](layout.md); worker search, mapping, crop and rotation tests pass |
| M3 real-model translation | Implemented on Mac arm64; broader native race/scroll checks continue in M5 | [Translation evidence](translation.md); native Metal current-page translation and CPU/Metal service probes verified |
| M4 persistence and batch | Pending | |
| M5 native behavior | Pending | |
| M6 distribution validation | Pending | |

No clean-machine offline, Windows, Intel Mac, signing or notarization claim
is made until corresponding results are recorded here.
