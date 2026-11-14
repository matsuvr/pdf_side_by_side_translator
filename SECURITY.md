# Security reports

This is a local desktop application. PDF and GGUF parsing still process input
files, and dependency updates need a fresh build, notice review and regression
validation. The app does not provide a network listener or remote inference API.

The memory-only viewer starts at version 0.1.2. Earlier releases stored state,
translations and logs on disk. Fixes are developed against the current source.
PDFium and llama.cpp run with the application's OS privileges in the same
process as the viewer. A parser failure can terminate the viewer, and a native
code defect can affect the entire process. The per-page and per-output limits
do not bound all decoder allocations or the operating system's swap and dumps.
On Windows, the viewer requests dynamic code prohibition before starting workers; if the host policy does not permit it, the viewer continues with the other process mitigations instead of refusing to start.

The development workflow checks direct Git revisions against OSV and SQLite's
version against NVD on source changes and weekly. A matching advisory requires
review against the features linked into the product. Package manifests and
the build-time CycloneDX inventory identify the dependencies used by a release.

Please report a problem with the app version, target, diagnostic codes and a
small original reproduction fixture. Do not put private PDFs, generated private
translations, passwords, credentials or personal state databases into a public
issue. If the repository's private vulnerability reporting is available, use
that route for a sensitive report. Diagnostics are displayed locally and are
never submitted automatically.

Only the tested configurations documented in the repository have evidence of
execution. Development ad-hoc Mac signatures are not Developer ID signatures
or Apple notarization.
