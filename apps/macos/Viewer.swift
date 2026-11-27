import AppKit
import Combine
import SwiftUI
import UniformTypeIdentifiers
import ReaderCore

private extension ReaderBytes {
    var text: String {
        guard let data, len > 0 else { return "" }
        return String(decoding: UnsafeBufferPointer(start: data, count: len), as: UTF8.self)
    }
}

final class Reading: ObservableObject {
    private var core = reader_new()
    private var timer: Timer?
    private var revision: UInt64 = 0
    private var imageID: UInt64 = 0
    private var file: URL?
    @Published var image: NSImage?
    @Published var source = ""
    @Published var translation = ""
    @Published var status = "PDFを開いてください。"
    @Published var page: UInt32 = 0
    @Published var count: UInt32 = 0
    @Published var title = "Offline PDF Translator"
    @Published var needsPassword = false

    init() {
        if core == nil { status = "閲覧処理を開始できません。" }
    }
    func shutdown() {
        timer?.invalidate()
        timer = nil
        reader_free(core)
        core = nil
    }
    deinit { shutdown() }

    func pick() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.pdf]
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url { open(url) }
    }
    func open(_ url: URL, password: String = "") {
        guard core != nil else { return }
        file = url
        title = url.lastPathComponent + " — Offline PDF Translator"
        reader_open(core, url.path, password)
        watch()
    }
    func unlock(_ password: String) { if let file { open(file, password: password) } }
    func go(_ number: UInt32) {
        guard number > 0, number <= count, number != page else { return }
        reader_page(core, number)
        watch()
    }
    func find(_ query: String) {
        guard count > 0, !query.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        reader_find(core, query)
        watch()
    }
    private func watch() {
        if timer == nil {
            let timer = Timer(timeInterval: 0.075, repeats: true) { [weak self] _ in self?.refresh() }
            self.timer = timer
            RunLoop.main.add(timer, forMode: .common)
        }
        refresh()
    }
    private func refresh() {
        var view = ReaderView()
        guard let snapshot = reader_poll(core, revision, imageID, &view) else { return }
        defer { reader_snapshot_free(snapshot) }
        revision = view.revision
        if imageID != view.image_id {
            imageID = view.image_id
            if let data = view.png.data, view.png.len > 0 {
                image = NSImage(data: Data(bytes: data, count: view.png.len))
            } else { image = nil }
        }
        if page != view.page { page = view.page }
        if count != view.count { count = view.count }
        let newSource = view.source.text, newTranslation = view.translation.text
        if source != newSource { source = newSource }
        if translation != newTranslation { translation = newTranslation }
        let phase = ReaderPhase(rawValue: view.phase)
        let message = view.message.text
        switch phase {
        case ReaderLoading: status = "ページを読み込み中…"
        case ReaderReadingImage: status = "画像から本文を読み取り中…"
        case ReaderTranslating: status = "日本語に翻訳中…"
        case ReaderEmpty: status = "このページには読み取れる本文がありません。"
        case ReaderPassword: needsPassword = true; status = "パスワードが必要です。"
        default: status = ""
        }
        if !message.isEmpty { status = message }
        if ![ReaderLoading, ReaderReadingImage, ReaderTranslating].contains(phase) {
            timer?.invalidate()
            timer = nil
        }
    }
}

private struct SelectableText: NSViewRepresentable {
    let text: String
    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSTextView.scrollableTextView()
        let view = scroll.documentView as! NSTextView
        view.isEditable = false
        view.isSelectable = true
        view.isRichText = false
        view.allowsUndo = false
        view.usesFindPanel = false
        view.isAutomaticLinkDetectionEnabled = false
        view.textContainerInset = NSSize(width: 14, height: 10)
        view.font = .preferredFont(forTextStyle: .body)
        view.drawsBackground = false
        scroll.drawsBackground = false
        return scroll
    }
    func updateNSView(_ scroll: NSScrollView, context: Context) {
        guard let view = scroll.documentView as? NSTextView, view.string != text else { return }
        view.string = text
        view.scrollToBeginningOfDocument(nil)
    }
}

struct Viewer: View {
    @ObservedObject var reader: Reading
    @State private var showText = false
    @State private var showSearch = false
    @State private var query = ""
    @State private var number = ""
    @State private var password = ""
    @State private var zoom: CGFloat = 1
    @GestureState private var pinch: CGFloat = 1
    @FocusState private var searchFocus: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Button("PDFを開く", action: reader.pick).keyboardShortcut("o")
                Button { if reader.page > 1 { reader.go(reader.page - 1) } } label: { Image(systemName: "chevron.left") }
                    .accessibilityLabel("前のページ").disabled(reader.page <= 1)
                TextField("ページ", text: $number).frame(width: 48).multilineTextAlignment(.center)
                    .accessibilityLabel("ページ番号").disabled(reader.count == 0)
                    .onSubmit { if let value = UInt32(number) { reader.go(value) }; number = String(reader.page) }
                Text(reader.count == 0 ? "" : "/ \(reader.count)").monospacedDigit()
                Button { reader.go(reader.page + 1) } label: { Image(systemName: "chevron.right") }
                    .accessibilityLabel("次のページ").disabled(reader.page >= reader.count)
                Button("検索") { showSearch = true; searchFocus = true }.keyboardShortcut("f").disabled(reader.count == 0)
                if showSearch {
                    TextField("PDF内を検索", text: $query).frame(maxWidth: 220).focused($searchFocus)
                        .onSubmit { reader.find(query) }.onExitCommand { showSearch = false }
                }
                Spacer()
            }.padding(10)
            Divider()
            HSplitView {
                VStack(alignment: .leading, spacing: 0) {
                    HStack {
                        Text("原文").font(.headline)
                        Toggle("本文", isOn: $showText).toggleStyle(.button)
                            .disabled(reader.source.isEmpty && !showText)
                    }.padding(10)
                    if showText {
                        SelectableText(text: reader.source)
                    } else {
                        GeometryReader { geometry in
                            ScrollView([.horizontal,.vertical]) {
                                if let image = reader.image {
                                    Image(nsImage: image).resizable().aspectRatio(contentMode: .fit)
                                        .frame(width: geometry.size.width * max(0.5, min(4, zoom * pinch)))
                                        .accessibilityLabel("現在のPDFページ")
                                }
                            }.gesture(MagnificationGesture().updating($pinch) { value, state, _ in state = value }
                                .onEnded { zoom = max(0.5, min(4, zoom * $0)) })
                        }
                    }
                }.frame(minWidth: 220, maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                VStack(alignment: .leading, spacing: 0) {
                    Text("日本語訳").font(.headline).padding(12)
                    SelectableText(text: reader.translation)
                }.frame(minWidth: 220, maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            }
            if !reader.status.isEmpty { Divider(); Text(reader.status).font(.callout).padding(8) }
        }
        .navigationTitle(reader.title)
        .onChange(of: reader.page) { _, page in number = page == 0 ? "" : String(page); zoom = 1 }
        .sheet(isPresented: $reader.needsPassword) {
            VStack(alignment: .leading, spacing: 12) {
                SecureField("PDFのパスワード", text: $password)
                HStack {
                    Button("キャンセル") { password = ""; reader.needsPassword = false }.keyboardShortcut(.cancelAction)
                    Button("開く") {
                        let entered = password
                        password = ""
                        reader.needsPassword = false
                        reader.unlock(entered)
                    }.keyboardShortcut(.defaultAction)
                }
            }.padding().frame(minWidth: 300)
        }
    }
}

final class Delegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let reader = Reading()
    var window: NSWindow?
    private var titleObservation: AnyCancellable?
    func applicationDidFinishLaunching(_ notification: Notification) {
        let window = NSWindow(contentRect: NSRect(x: 0,y: 0,width: 1120,height: 760),
            styleMask: [.titled,.closable,.miniaturizable,.resizable], backing: .buffered,defer: false)
        window.isRestorable = false
        window.minSize = NSSize(width: 600,height: 360)
        titleObservation = reader.$title.sink { [weak window] title in window?.title = title }
        window.contentView = NSHostingView(rootView: Viewer(reader: reader))
        window.delegate = self
        self.window = window
        let bar = NSMenu()
        let appMenu = NSMenu()
        let appItem = bar.addItem(withTitle: "", action: nil, keyEquivalent: "")
        appItem.submenu = appMenu
        appMenu.addItem(withTitle: "終了", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let fileMenu = NSMenu(title: "ファイル")
        bar.addItem(withTitle: "ファイル", action: nil, keyEquivalent: "").submenu = fileMenu
        fileMenu.addItem(withTitle: "PDFを開く…", action: #selector(pick), keyEquivalent: "o").target = self
        let edit = NSMenu(title: "編集")
        bar.addItem(withTitle: "編集", action: nil, keyEquivalent: "").submenu = edit
        edit.addItem(withTitle: "コピー", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "すべてを選択", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        NSApp.mainMenu = bar
        window.center(); window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
        if CommandLine.arguments.count == 2, CommandLine.arguments[1].lowercased().hasSuffix(".pdf") {
            reader.open(URL(fileURLWithPath: CommandLine.arguments[1]))
        }
    }
    @objc private func pick() { reader.pick() }
    func application(_ sender: NSApplication, open urls: [URL]) { if let url = urls.first { reader.open(url) } }
    func applicationWillTerminate(_ notification: Notification) { reader.shutdown() }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool { false }
}

#if !VIEWER_UI_TEST
@main
#endif
enum ViewerApp {
    static func main() {
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        let delegate = Delegate()
        app.delegate = delegate
        withExtendedLifetime(delegate) { app.run() }
    }
}
