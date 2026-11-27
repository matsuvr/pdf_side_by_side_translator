# Security

The viewer processes local, read-only PDFs and bundled models in-process. The Windows reader maps a read-shared PDF. The Mac reader owns a snapshot of the PDF. Both readers verify the bundled model before inference. Application caches and OCR images live in memory for the session.

The PDF interpreter and image decoders may consume resources before producing a page. The output raster and cache budgets do not bound every internal allocation. OS memory management, crash reporting, and the clipboard are controlled by the operating system.

Please report security issues privately when possible. Include the version, operating system and a small original reproduction fixture. Keep private documents, translations, passwords and credentials out of reports.
