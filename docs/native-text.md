# Native text and completion validation

The macOS arm64 `native_text_test` runs a real wxTextCtrl/NSTextView in a
foreground desktop window. It registers the bundled Noto font, displays 800
lines containing Japanese, combining marks and supplementary emoji, establishes
a nonempty selection and a viewport around line 300, then appends 80 updates.
It verifies the native selection, focus and pixel scroll position are unchanged.
It also verifies logical line restoration after a 14-to-21-point font change
and a window width change, and replacement of a changed output suffix.
The test passed on the development desktop. Run it in an interactive session;
it is deliberately separate from headless CTest. Windows execution is pending.

The reader offers a 6–72-point text-size setting, persists it, and restores the
logical reading anchor and selection when changing it. Window and splitter
resizes also restore a logical anchor. This complements the per-page anchors
and durable last-reading position; it does not synchronize PDF and text scroll.

Additional completion regressions verify that embedded NUL, DEL and terminal
controls cannot become a completed translation or completed cache entry.
Ordinary tab/CR/LF are retained in stored/exported text. The Rich Edit adapter
canonicalizes CRLF/LF/CR to one native CR for display, including a stream split
between CR and LF. Its temporary UTF-8 buffer retains the owning wxString.
The completion policy version is now 2.

Cache/state modification sequences advance above the persisted high-water mark
on restart. An integration regression stores an older completion with a future
timestamp, restarts, creates a forced attempt and verifies that the new attempt
is selected and the superseded completion cannot be exported.

The source editor has separate Save and Save-and-retranslate actions. Resetting
to extracted source changes only the dialog buffer/mode until saved; Cancel
therefore retains the previous override. Exports record the supported model and
the selected prose style. A current partial result requires an explicit choice
and is labeled partial in the file; complete-page export identifies its scope.
