# Security reports

This is a local desktop application. PDF and GGUF parsing still process input
files, and dependency updates need a fresh build, notice review and regression
validation. The app does not provide a network listener or remote inference API.

The memory-only viewer starts at version 0.1.2. Earlier releases stored state,
translations and logs on disk. Fixes are developed against the current source.
On Windows the application sends read-only PDF handles to a separate
low-integrity, privilege-restricted PDFium process. The UI only receives
bounded, validated metadata, image and text records. A process job limits the
parser's committed memory, child creation and UI operations, and terminates it
when the document is closed or replaced. Failure to establish the restricted
worker is a document-open error. The worker establishes dynamic code
prohibition before initializing PDFium. See [Windows PDF worker](docs/windows-pdf-worker.md)
for the process boundary and release acceptance tests.

This is an initial process and write containment layer, not a confidentiality
sandbox: a low-integrity token does not remove all user-level read or network
access. On macOS PDFium remains in the viewer process; llama.cpp remains in
the viewer process on both platforms. Native parser defects, OS swap and
crash dumps remain part of the threat model. The Windows worker and viewer
must not be described as a complete sandbox.


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
