# Native text and completion validation

The macOS arm64 `native_text_test` runs a real wxTextCtrl/NSTextView in a
foreground desktop window. It registers the bundled Noto font, displays 800
lines containing Japanese, combining marks and supplementary emoji, establishes
a nonempty selection and a viewport around line 300, then appends 80 updates.
It verifies the native selection, focus and pixel scroll position are unchanged.
It also verifies logical line restoration after a 14-to-21-point font change
and a window width change, and replacement of a changed output suffix.
The original viewport test passed on macOS arm64. The Windows Release test also
passes, including read-only selection/copy and attempted text changes. Run it
in an interactive session; it is deliberately separate from headless CTest.

The reader offers a 6–72-point text-size setting, keeps it in memory for the session, and restores the
logical reading anchor and selection when changing it. Window and splitter
resizes also restore a logical anchor. This complements the per-page anchors
and session last-reading position; it does not synchronize PDF and text scroll.

Additional completion regressions verify that embedded NUL, DEL and terminal
controls cannot become a completed translation or completed cache entry.
Ordinary tab/CR/LF are retained in the in-memory text. The Rich Edit adapter
canonicalizes CRLF/LF/CR to one native CR for display, including a stream split
between CR and LF. Its temporary UTF-8 buffer retains the owning wxString.
The completion policy version is now 2.

The text pane is read-only. Native acceptance checks selection and copying,
then attempts cut, paste, clear, undo, typing, backspace and delete on Windows.
The text must remain unchanged after every operation, including after streaming
updates. Completed translations are reused in memory when a page is revisited.
Source editing, forced retranslation and file exports have been removed.
