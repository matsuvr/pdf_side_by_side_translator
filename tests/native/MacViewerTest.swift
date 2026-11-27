import AppKit
import Foundation

// Build with Viewer.swift and -DVIEWER_UI_TEST. This exercises the real
// hosting window and AppKit text controls without Accessibility permissions.
@main
enum MacViewerTest {
    static func textViews(in parent: NSView) -> [NSTextView] {
        parent.subviews.flatMap { view in
            (view as? NSTextView).map { [$0] } ?? textViews(in: view)
        }
    }
    static func wait(_ test: () -> Bool) {
        let deadline = Date().addingTimeInterval(120)
        while !test() && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        precondition(test(), "Timed out waiting for the translated page")
    }
    static func main() {
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        let delegate = Delegate()
        app.delegate = delegate
        if delegate.window == nil {
            delegate.applicationDidFinishLaunching(Notification(name: NSApplication.didFinishLaunchingNotification))
        }
        let reader = delegate.reader
        let root = delegate.window!.contentView!
        reader.open(URL(fileURLWithPath: CommandLine.arguments[1]))
        wait { reader.status == "完了" && reader.image != nil && textViews(in: root).contains {
            $0.string.contains("3") && $0.string.contains("センサ") && !$0.isEditable && $0.isSelectable
                && $0.font == NSFont.preferredFont(forTextStyle: .body)
        } }
        let first = reader.translation
        reader.go(2)
        wait { reader.status == "完了" && reader.page == 2 && textViews(in: root).contains { $0.string.contains("12") } }
        reader.go(1)
        wait { reader.page == 1 && textViews(in: root).contains { $0.string == first } }
        delegate.applicationWillTerminate(Notification(name: NSApplication.willTerminateNotification))
        print("SwiftUI/AppKit rendered both translated pages in selectable, read-only text views.")
    }
}
