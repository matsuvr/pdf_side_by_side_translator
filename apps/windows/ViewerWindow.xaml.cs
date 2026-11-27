using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.ApplicationModel.DataTransfer;
using Windows.System;
using WinRT.Interop;

namespace OfflinePdf;

public sealed partial class ViewerWindow : Window
{
    private const string Product = "Offline PDF Translator";
    private IntPtr reader = Core.reader_new();
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromMilliseconds(60) };
    private readonly DispatcherTimer cpuTimer = new() { Interval = TimeSpan.FromSeconds(1) };
    private readonly List<string> recent = new();
    private ulong revision, imageId, lastIdle, lastTotal;
    private uint page, count;
    private string path = "", currentSource = "", currentTranslation = "";
    private bool passwordDialogOpen;

    public ViewerWindow()
    {
        InitializeComponent();
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1120, 780));
        if (reader == IntPtr.Zero) Status.Text = "閲覧処理を開始できません。";
        timer.Tick += (_, _) => Refresh();
        cpuTimer.Tick += (_, _) => UpdateCpuUse();
        Closed += (_, _) =>
        {
            timer.Stop();
            cpuTimer.Stop();
            Core.reader_free(reader);
            reader = IntPtr.Zero;
        };
        Shortcut(VirtualKey.O, () => Pick(this, new RoutedEventArgs()));
        Shortcut(VirtualKey.W, CloseCurrentDocument);
        Shortcut(VirtualKey.Q, Close);
        Shortcut(VirtualKey.G, () => FocusPage(this, new RoutedEventArgs()));
        Shortcut(VirtualKey.F, ShowSearch);
        Shortcut((VirtualKey)0xBC, SelectModel);
        Shortcut((VirtualKey)0xDB, () => Navigate(page - 1));
        Shortcut((VirtualKey)0xDD, () => Navigate(page + 1));
        Shortcut(VirtualKey.Number0, () => SetZoom(ZoomMode.Page));
        Shortcut(VirtualKey.Add, () => StepZoom(true));
        Shortcut(VirtualKey.Subtract, () => StepZoom(false));
        Shortcut((VirtualKey)0xBB, () => StepZoom(true));
        Shortcut((VirtualKey)0xBD, () => StepZoom(false));
        PaperScroll.AddHandler(UIElement.PointerWheelChangedEvent, new PointerEventHandler(ZoomWheel), true);
    }

    private void Shortcut(VirtualKey key, Action action)
    {
        var shortcut = new KeyboardAccelerator { Key = key, Modifiers = VirtualKeyModifiers.Control };
        shortcut.Invoked += (_, e) => { e.Handled = true; action(); };
        Root.KeyboardAccelerators.Add(shortcut);
    }

    public void Open(string filename, string password = "")
    {
        if (reader == IntPtr.Zero) return;
        if (password.Length == 0)
        {
            rotation = 0;
            SetZoom(ZoomMode.Page);
            SearchBar.Visibility = Visibility.Collapsed;
            Query.Text = "";
        }
        path = filename;
        Title = Path.GetFileName(path) + " — " + Product;
        CloseItem.IsEnabled = true;
        Core.reader_open(reader, path, password);
        cpuTimer.Start();
        Poll();
    }

    private void Pick(object sender, RoutedEventArgs e)
    {
        var filename = FilePicker.Open(WindowNative.GetWindowHandle(this), "PDF (*.pdf)\0*.pdf\0\0");
        if (filename != null) Open(filename);
    }

    private void RememberDocument()
    {
        recent.RemoveAll(entry => string.Equals(entry, path, StringComparison.OrdinalIgnoreCase));
        recent.Insert(0, path);
        if (recent.Count > 8) recent.RemoveAt(8);
        Recent.Items.Clear();
        foreach (var entry in recent)
        {
            var item = new MenuFlyoutItem { Text = Path.GetFileName(entry) };
            ToolTipService.SetToolTip(item, entry);
            item.Click += (_, _) => Open(entry);
            Recent.Items.Add(item);
        }
        Recent.IsEnabled = true;
    }

    private void CloseDocument(object sender, RoutedEventArgs e) => CloseCurrentDocument();
    private void CloseCurrentDocument()
    {
        if (path.Length == 0 || reader == IntPtr.Zero) return;
        Core.reader_close(reader);
        path = "";
        Title = Product;
        CloseItem.IsEnabled = false;
        SearchBar.Visibility = Visibility.Collapsed;
        Query.Text = "";
        cpuTimer.Stop();
        CpuStatus.Text = "";
        lastIdle = lastTotal = 0;
        Poll();
    }

    private void Quit(object sender, RoutedEventArgs e) => Close();

    private void Navigate(uint target)
    {
        if (target < 1 || target > count || target == page) return;
        Core.reader_page(reader, target);
        Poll();
    }
    private void Prev(object sender, RoutedEventArgs e) => Navigate(page - 1);
    private void Forward(object sender, RoutedEventArgs e) => Navigate(page + 1);

    private void FocusPage(object sender, RoutedEventArgs e)
    {
        if (count == 0) return;
        PageNumber.Focus(FocusState.Keyboard);
        PageNumber.SelectAll();
    }

    private void PageEntered(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != VirtualKey.Enter) return;
        e.Handled = true;
        if (uint.TryParse(PageNumber.Text, out var target)) Navigate(target);
        PageNumber.Text = page.ToString(CultureInfo.CurrentCulture);
    }

    private void ShowFind(object sender, RoutedEventArgs e) => ShowSearch();
    private void ShowSearch()
    {
        if (count == 0) return;
        SearchBar.Visibility = Visibility.Visible;
        Query.Focus(FocusState.Keyboard);
        Query.SelectAll();
    }

    private void HideFind(object sender, RoutedEventArgs e)
    {
        SearchBar.Visibility = Visibility.Collapsed;
        Core.reader_cancel_find(reader);
        Refresh();
        if (count != 0) PageNumber.Focus(FocusState.Programmatic);
    }

    private void FindNext(object sender, RoutedEventArgs e) => Search(false);
    private void FindPrevious(object sender, RoutedEventArgs e) => Search(true);
    private void Search(bool backwards)
    {
        if (count == 0) return;
        if (string.IsNullOrWhiteSpace(Query.Text)) { ShowSearch(); return; }
        Core.reader_find_matching(reader, Query.Text, backwards ? 1u : 0u, CaseSensitive.IsChecked == true ? 1u : 0u);
        Poll();
    }

    private void SearchEntered(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Escape) { HideFind(sender, e); e.Handled = true; }
        if (e.Key == VirtualKey.Enter && Query.FocusState != FocusState.Unfocused) { Search(false); e.Handled = true; }
    }

    private void CopySelection(object sender, RoutedEventArgs e) => CopyText(Translation.SelectedText);

    private void SelectAll(object sender, RoutedEventArgs e)
    {
        Translation.Focus(FocusState.Programmatic);
        Translation.SelectAll();
    }
    private void CopySource(object sender, RoutedEventArgs e) => CopyText(currentSource);

    private static void CopyText(string text)
    {
        if (text.Length == 0) return;
        var data = new DataPackage();
        data.SetText(text);
        Clipboard.SetContent(data);
    }

    private void SetTranslationOptions(object sender, RoutedEventArgs e)
    {
        Core.reader_translation_options(reader, AutoTranslate.IsChecked ? 1u : 0u, EasyStyle.IsChecked ? 1u : 0u);
        Poll();
    }

    private void PickModel(object sender, RoutedEventArgs e) => SelectModel();
    private void SelectModel()
    {
        if (reader == IntPtr.Zero) return;
        var filename = FilePicker.Open(WindowNative.GetWindowHandle(this), "同梱形式の翻訳モデル (*.p4x86)\0*.p4x86\0\0");
        if (filename is null) return;
        Core.reader_model(reader, filename);
        Poll();
    }

    private async void ChangeTextSize(object sender, RoutedEventArgs e)
    {
        var input = new NumberBox { Header = "ポイント", Value = Translation.FontSize * 0.75,
            Minimum = 4, Maximum = 200, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Inline };
        var dialog = new ContentDialog { XamlRoot = Root.XamlRoot, Title = "訳文の文字サイズ", Content = input,
            PrimaryButtonText = "適用", CloseButtonText = "キャンセル", DefaultButton = ContentDialogButton.Primary };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary || !double.IsFinite(input.Value)) return;
        var points = Math.Clamp(input.Value, 4, 200);
        Translation.FontSize = points / 0.75;
    }

    private void ShowHelp(object sender, RoutedEventArgs e) => _ = ShowInfo("操作方法",
        "PDFを開くと、現在のページの原文と日本語訳が表示されます。\n" +
        "原文は初期状態でページ全体を表示します。倍率、−／＋、表示メニューで拡大・縮小できます。\n" +
        "ページ番号または矢印で移動し、Ctrl+FでPDF内を検索できます。\n" +
        "原文は「選択・コピー」からページ単位でコピーできます。訳文は読み取り専用で、選んだ範囲をコピーできます。");
    private void ShowAbout(object sender, RoutedEventArgs e) => _ = ShowInfo("このアプリについて",
        Product + " 0.4.0\nPDFと日本語訳を並べて読むオフラインビューアー\nBuilt with PLaMo");
    private void ShowDiagnostics(object sender, RoutedEventArgs e) => _ = ShowInfo("診断情報",
        Product + " 0.4.0\nWindows x64 / WinUI 3\nRust / PLaMo 3 Translate 0.8B / S4 v7", true);
    private void ShowLicenses(object sender, RoutedEventArgs e)
    {
        string ReadLicense(string filename)
        {
            var file = Path.Combine(AppContext.BaseDirectory, filename);
            try { return File.ReadAllText(file); }
            catch (IOException) { return $"{filename} は配布 ZIP に同梱しています。"; }
            catch (UnauthorizedAccessException) { return $"{filename} を読み取れません。"; }
        }
        _ = ShowInfo("ライセンス", ReadLicense("LICENSE") + "\n\n" + ReadLicense("PLaMo-BF16-Community-ja.txt"));
    }

    private async System.Threading.Tasks.Task ShowInfo(string title, string text, bool offerCopy = false)
    {
        var body = new TextBox { Text = text, IsReadOnly = true, TextWrapping = TextWrapping.Wrap, AcceptsReturn = true, MaxHeight = 400 };
        var dialog = new ContentDialog { XamlRoot = Root.XamlRoot, Title = title, Content = body, CloseButtonText = "閉じる" };
        if (offerCopy) dialog.PrimaryButtonText = "コピー";
        if (await dialog.ShowAsync() == ContentDialogResult.Primary) CopyText(text);
    }

    private async void AskPassword()
    {
        if (passwordDialogOpen) return;
        passwordDialogOpen = true;
        var input = new PasswordBox { Header = "PDFのパスワード" };
        var dialog = new ContentDialog { XamlRoot = Root.XamlRoot, Title = Path.GetFileName(path),
            Content = input, PrimaryButtonText = "開く", CloseButtonText = "キャンセル", DefaultButton = ContentDialogButton.Primary };
        var file = path;
        var answer = await dialog.ShowAsync();
        passwordDialogOpen = false;
        if (reader != IntPtr.Zero && path == file)
        {
            if (answer == ContentDialogResult.Primary) Open(file, input.Password);
            else CloseCurrentDocument();
        }
        input.Password = "";
    }

    private void Poll()
    {
        timer.Start();
        Refresh();
    }

    private static ScrollViewer? TextScroller(DependencyObject element)
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(element); i++)
        {
            var child = VisualTreeHelper.GetChild(element, i);
            var scroll = child as ScrollViewer ?? TextScroller(child);
            if (scroll != null) return scroll;
        }
        return null;
    }

    private static void ShowBody(TextBox box, ref string displayed, string value)
    {
        if (value == displayed) return;
        bool appending = displayed.Length > 0 && value.StartsWith(displayed, StringComparison.Ordinal);
        int start = appending ? box.SelectionStart : 0;
        int length = appending ? box.SelectionLength : 0;
        var scroll = TextScroller(box);
        double offset = appending ? scroll?.VerticalOffset ?? 0 : 0;
        box.Text = value;
        if (box.SelectionStart != start || box.SelectionLength != length) box.Select(start, length);
        scroll?.ChangeView(null, offset, null, true);
        displayed = value;
    }

    private void Refresh()
    {
        var snapshot = Core.reader_poll(reader, revision, imageId, out var view);
        if (snapshot == IntPtr.Zero) return;
        try
        {
            revision = view.Revision;
            if (imageId != view.ImageId)
            {
                imageId = view.ImageId;
                SetPaper(view);
            }
            if (count == 0 && view.Count > 0) RememberDocument();
            if (page != view.Page) PageNumber.Text = view.Page == 0 ? "" : view.Page.ToString(CultureInfo.CurrentCulture);
            page = view.Page;
            count = view.Count;
            UpdatePageCount();
            Previous.IsEnabled = PreviousItem.IsEnabled = page > 1;
            Next.IsEnabled = NextItem.IsEnabled = page < count;
            PageNumber.IsEnabled = PageItem.IsEnabled = FindItem.IsEnabled = FindNextItem.IsEnabled = FindPreviousItem.IsEnabled = count > 0;
            currentSource = view.Source.Text();
            ShowBody(Translation, ref currentTranslation, view.Translation.Text());
            CopySourceItem.IsEnabled = currentSource.Length != 0;
            var message = view.Message.Text();
            Status.Text = message.Length > 0 ? message : view.Phase switch
            {
                Phase.Idle => "PDFを開いてください。",
                Phase.Loading => "ページを読み込み中…",
                Phase.ReadingImage => "画像から本文を読み取り中…",
                Phase.Empty => "このページには読み取れる本文がありません。",
                Phase.Password => "パスワードが必要です。",
                _ => ""
            };
            var status = view.Phase switch
            {
                Phase.Translating => "翻訳中",
                Phase.Ready => "完了",
                Phase.Paused => "自動翻訳オフ",
                Phase.Error when count != 0 => "翻訳エラー",
                _ => ""
            };
            if (message == "検索中…") status = TranslationStatus.Text;
            TranslationStatus.Text = status;
            TranslationHeading.Text = status.Length == 0 ? "訳文" : "訳文 — " + status;
            if (view.Phase is not (Phase.Loading or Phase.ReadingImage or Phase.Translating)) timer.Stop();
            if (view.Phase == Phase.Password) AskPassword();
        }
        finally { Core.reader_snapshot_free(snapshot); }
    }

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetSystemTimes(out ulong idle, out ulong kernel, out ulong user);

    private void UpdateCpuUse()
    {
        if (!GetSystemTimes(out var idle, out var kernel, out var user)) return;
        var total = kernel + user;
        if (lastTotal != 0 && total > lastTotal && idle >= lastIdle)
        {
            var usage = 1 - (double)(idle - lastIdle) / (total - lastTotal);
            CpuStatus.Text = $"CPU {Math.Clamp(usage, 0, 1):P0}";
        }
        lastIdle = idle;
        lastTotal = total;
    }
}

internal static class FilePicker
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct OpenFileName
    {
        public int size;
        public IntPtr owner, instance;
        public string filter;
        public IntPtr customFilter;
        public int maxCustomFilter, filterIndex;
        public IntPtr file;
        public int maxFile;
        public IntPtr fileTitle;
        public int maxFileTitle;
        public IntPtr initialDir, title;
        public int flags;
        public short fileOffset, fileExtension;
        public IntPtr defaultExt, customData, hook, template, reserved;
        public int reservedValue, flagsEx;
    }
    [DllImport("comdlg32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetOpenFileNameW(ref OpenFileName data);
    public static string? Open(IntPtr owner, string filter)
    {
        const int capacity = 32768;
        IntPtr buffer = Marshal.AllocHGlobal(capacity * 2);
        try
        {
            Marshal.WriteInt16(buffer, 0);
            var data = new OpenFileName { size = Marshal.SizeOf<OpenFileName>(), owner = owner,
                filter = filter, file = buffer, maxFile = capacity,
                flags = 0x02000000 | 0x00001000 | 0x00000800 | 0x00000008 | 0x00080000 }; // no recent documents, existing file/path, keep cwd, Explorer
            return GetOpenFileNameW(ref data) ? Marshal.PtrToStringUni(buffer) : null;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }
}
