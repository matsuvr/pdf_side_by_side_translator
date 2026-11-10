# Native input evidence

The app follows DESIGN.md's Primary+Enter (current page), Primary+Shift+Enter
(all pages), Primary+G (page number), Primary+, (model settings) and F6/Shift+F6
bindings. Bare navigation keys stay with their focused native control. Escape
closes search or clears a PDF selection; it is not a translation stop shortcut.

Translation key bindings are routed after checking dialog depth and native
composition state. They are not installed as unconditional menu accelerators.
On Cocoa, some Command key equivalents bypass wx key hooks. A process-local
NSEvent monitor therefore handles only the declared shortcuts for the main
window. Marked text, other windows and menu tracking pass through. This is an
app-local event monitor, not a system keyboard hook. The public NSTextInputClient
marked-text state is checked before dispatch. On Windows, the native composition
string state comes from IMM32 and wx's key hook handles routing.

Native macOS testing initially found that Primary+G did not reach the wx hook
when there was no focused child. After the Cocoa monitor and initial PDF focus
fix, Primary+G selected the page field, typing 2 and Enter moved to page 2, and
Primary+Shift+Enter completed the three-page batch with two Complete pages and
one NoText page. Primary+Enter in the open source-edit dialog left that dialog
and its source text intact. These were actual native key events, not direct
calls to controller methods.

The shared policy test checks that every app shortcut passes through while
composition or a modal dialog is active, that bare Enter stays native and that
Escape never maps to Stop. Actual Japanese IME composition and Windows native
execution still require recorded manual acceptance. Long-text selection,
scroll restoration and monitor-scale changes are tracked separately from these
shortcut checks. Nineteen CTest cases pass at this checkpoint.
