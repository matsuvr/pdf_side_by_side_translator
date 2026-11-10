> Historical validation through v0.1.0. Current behavior and release checks: [implementation status](implementation-status.md).

# Distribution and publication checks

## Build and package provenance

Both Mac architectures have linked product builds from the same locked source
graph. Apple Silicon executes natively; x86_64 executes under Rosetta on that
Mac. The latest headless suite has 24 passing checks in both configurations.
Physical Intel, Windows, macOS 14 hardware and a clean machine without developer
tools remain unverified. Configured targets are not presented as tested systems.

The package script requires a clean source tree and a matching build identity
embedded in the executable. That identity includes the source commit/content
digest, target, version, dependency lock and notice-input lock. Generated
`BUILD-INFO.json` and `package-manifest.json` identify each local artifact.
The script checks every Mach-O in the final bundle, the native architecture,
minimum OS load command, OS-only dylib/framework imports and runtime search
paths. Windows packaging inspects normal and delay PE imports and static CRT
compile evidence; it has not been run on Windows.

The Mac ZIP is created after an ad-hoc signature, then extracted into another
directory and checked with strict/deep codesign verification and import
inspection again. The font hash, product icon and bundle identity/version are
verified. Help, notices, ICU data, Metal source and libraries are internal to
the executable/bundle. There is no separate Python, inference server, dynamic
backend, model or helper executable in the package. Package metadata redacts
the local source root, and source prefix mapping removes that root from CMake
compilation file-name strings.

Release ZIPs contain the Windows executable or Mac .app, the official pinned
Q4_K_M GGUF, `README.md` and `PLaMo-Community-ja.txt` at the top level.
The startup screen reads the adjacent license file when its link is activated.
Packaging verifies the GGUF against the model fixture's SHA-256, then extracts
the ZIP and checks the executable and all companion files before publication.

These are development artifacts. No Developer ID identity, notarization ticket,
stapling, quarantined Gatekeeper first launch or clean-machine acceptance is
claimed. Source publication does not silently imply those distribution checks.

## Relocated, fresh-state, network-denied product run

A signed Apple Silicon ZIP candidate was extracted outside the workspace into
a directory containing Japanese and spaces. Original PDF fixtures and the
locally supplied fixed GGUF were copied beside that app. The app was started
with a new `--data-dir`, a PATH containing only `/usr/bin:/bin`, and no developer
library/framework overrides. A process-local sandbox denied all networking and
all reads under the development workspace. It did not change the Mac's system
network settings. Negative controls verified a loopback socket connect failed
with EPERM and a workspace file could not be read.

Observed through the actual product UI:

- First start had no selected model, while PDF reading already worked.
- A Japanese/space-path GGUF loaded and current-page Metal translation completed.
- All-page translation reported Complete 2/3, NoText 1, Partial 0, Failed 0.
- Complete-page UTF-8 export contained exactly the two completed pages and the
  model/style/output-use information.
- Saving a 9-sensor source override and retranslation preserved digits and
  negation; resetting the dialog then cancelling retained that override.
- A long actual generation stopped with visible partial text and a partial
  label; Stop became disabled and automatic work did not restart on its own.
- Forced CPU model reconstruction and fresh retranslation completed. CPU output
  differed in word choice from the prior Metal result, while retaining 9 and
  negation; byte equality is not a translation-quality requirement.
- After exit/restart under the same restrictions, CPU preference, 18-point text
  size, last physical page, edited source and completed attempt were restored.
- Cache deletion followed by explicit fresh CPU generation still worked with
  no network or workspace access.
- An unembedded-font Japanese PDF displayed its Japanese characters using the
  bundled PDF fallback font. The font callback also has an independent actual
  PDFium test distinguishing it from embedded-font rendering.

The saved database and export were inspected after closing the app. The state
contained the expected source override and reading preferences. Structured logs
contained no source sentence, GGUF path, private temporary path or user path.
The sandbox test is stronger than a relocation-only launch but is **not** a
substitute for an independent clean Mac: OS caches, system fonts and system
services still belonged to the development host.

This run also found two UI issues, corrected before final packaging: inactive
auto-translation pages retained a preparation message/old completion status,
and the toolbar's last button expanded into unused row space. Changed model,
style and source conditions now clear the old result/label, and manual waiting
state has a specific instruction. Cache clear releases the retained attempt
before the database clear so a later save cannot silently repopulate the old
result. Final native checks cover these corrections separately from the first
candidate's evidence.

The corrected Apple Silicon ZIP was then extracted and run with the same
network/workspace restrictions. With automatic translation off it showed the
manual instruction and no old completion label. A cached result could be
displayed explicitly. After Clear Cache and exit, `completed_chunks`,
`page_attempts` and `attempt_chunks` all contained zero rows, while the source
override remained. The final x86_64 ZIP was separately extracted and launched
under Rosetta with that empty cache and the same sandbox; the product UI
completed a fresh CPU translation of the preserved 9-sensor source. These were
the packaged applications, not only the dependency/inference test programs.
Both final candidates contain exactly one native Mach-O, minimum OS 14.0,
SDK 27.2, no rpaths and no non-OS load dependencies. Neither executable contains
the literal development source root.

## Storage failure and source audit

Runtime SQLite write failures are distinguished from programming/constraint
errors. On recoverable storage failure, the I/O worker clones readable state
and cache into memory and retries the accepted immutable request once. It
preserves the original disk data and marks the session unsaved. Fault-injection
tests verify old and new source edits remain refetchable after leaving their
pages and can be restored from a single-file rescue backup. An invalid backup
destination does not disable healthy active storage.

`scripts/audit_publication.py` inspects every tree and blob reachable from the
branch to publish, including deleted historical content and reused blob paths,
plus current tracked/unignored files. It rejects runtime/private artifact paths
and reports known credential/private-key patterns without printing their
values. A regression verifies a removed historical synthetic token is detected.
Manual review also checks fixture/resource provenance, complete license texts,
Git author metadata and the exact public destination. This pattern-based check
does not claim to prove the absence of every possible secret or vulnerability.

No GGUF weights, user state/cache, development toolchain, downloaded dependency
source tree or private PDF is included in Git. The PDF/font test fixtures and
icon are newly authored; upstream code is fetched at build time under its
recorded terms. The original application is MIT, while upstream/font/model
conditions remain distinct and their complete notices are included.

Before publication, the previously unpublished history was normalized to the
owner's GitHub noreply identity while preserving file trees, messages, parent
structure and dates. No already published branch was rewritten. The audit then
reported no findings across 12 commits / 287 historical blobs and confirmed
noreply authors throughout; a final repeat includes this documentation update.
The former private author email was also checked against the published source
content without printing it and was absent. Public third-party copyright and
license notices retain their upstream attribution.
