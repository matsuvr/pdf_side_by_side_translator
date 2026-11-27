using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.System;
using WinRT.Interop;

namespace OfflinePdf;

public sealed partial class ViewerWindow : Window
{
    private IntPtr reader = Core.reader_new();
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromMilliseconds(75) };
    private ulong revision, imageId;
    private uint page, count;
    private string path = "";

    public ViewerWindow()
    {
        InitializeComponent();
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1120, 780));
        if (reader == IntPtr.Zero) Status.Text = "閲覧処理を開始できません。";
        timer.Tick += (_, _) => Refresh();
        Closed += (_, _) => { timer.Stop(); Core.reader_free(reader); reader = IntPtr.Zero; };
        Shortcut(VirtualKey.O, () => Pick(this, new RoutedEventArgs()));
        Shortcut(VirtualKey.G, () => { PageNumber.Focus(FocusState.Keyboard); PageNumber.SelectAll(); });
        Shortcut(VirtualKey.F, () => ShowFind(this, new RoutedEventArgs()));
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
        path = filename;
        Title = Path.GetFileName(path) + " — Offline PDF Translator";
        Core.reader_open(reader, path, password);
        timer.Start();
        Refresh();
    }
    private void Pick(object sender, RoutedEventArgs e)
    {
        var filename = PdfPicker.Open(WindowNative.GetWindowHandle(this));
        if (filename != null) Open(filename);
    }
    private void Navigate(uint target)
    {
        if (target < 1 || target > count || target == page) return;
        Core.reader_page(reader, target); timer.Start(); Refresh();
    }
    private void Prev(object sender, RoutedEventArgs e) => Navigate(page - 1);
    private void Forward(object sender, RoutedEventArgs e) => Navigate(page + 1);
    private void PageEntered(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != VirtualKey.Enter) return;
        e.Handled = true;
        if (uint.TryParse(PageNumber.Text, out var target)) Navigate(target);
        PageNumber.Text = page.ToString();
    }
    private void ShowFind(object sender, RoutedEventArgs e)
    {
        if (count == 0) return;
        Query.Visibility = Visibility.Visible;
        Query.Focus(FocusState.Keyboard); Query.SelectAll();
    }
    private void SearchEntered(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Escape) { Query.Visibility = Visibility.Collapsed; e.Handled = true; }
        if (e.Key != VirtualKey.Enter || string.IsNullOrWhiteSpace(Query.Text)) return;
        e.Handled = true;
        Core.reader_find(reader, Query.Text); timer.Start(); Refresh();
    }
    private void ToggleSource(object sender, RoutedEventArgs e)
    {
        bool text = SourceMode.IsChecked == true;
        SourceScroll.Visibility = text ? Visibility.Visible : Visibility.Collapsed;
        PaperScroll.Visibility = text ? Visibility.Collapsed : Visibility.Visible;
    }
    private async void AskPassword()
    {
        var input = new PasswordBox { Header = "PDFのパスワード" };
        var dialog = new ContentDialog { XamlRoot = Root.XamlRoot, Title = Path.GetFileName(path),
            Content = input, PrimaryButtonText = "開く", CloseButtonText = "キャンセル", DefaultButton = ContentDialogButton.Primary };
        var file = path;
        if (await dialog.ShowAsync() == ContentDialogResult.Primary && reader != IntPtr.Zero && path == file)
            Open(file, input.Password);
        input.Password = "";
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
                var bytes = view.Png.Copy();
                if (bytes.Length == 0) Paper.Source = null;
                else
                {
                    using var memory = new MemoryStream(bytes);
                    using var stream = memory.AsRandomAccessStream();
                    var bitmap = new BitmapImage();
                    bitmap.SetSource(stream);
                    Paper.Source = bitmap;
                }
                PaperScroll.ChangeView(0, 0, 1f, true);
                SourceScroll.ChangeView(null, 0, null, true);
                TranslationScroll.ChangeView(null, 0, null, true);
            }
            if (page != view.Page) PageNumber.Text = view.Page == 0 ? "" : view.Page.ToString();
            page = view.Page; count = view.Count;
            PageCount.Text = count == 0 ? "" : "/ " + count;
            Previous.IsEnabled = page > 1; Next.IsEnabled = page < count;
            PageNumber.IsEnabled = FindButton.IsEnabled = count > 0;
            string source = view.Source.Text(), translation = view.Translation.Text();
            if (Source.Text != source) Source.Text = source;
            if (Translation.Text != translation) Translation.Text = translation;
            SourceMode.IsEnabled = source.Length != 0 || SourceMode.IsChecked == true;
            var message = view.Message.Text();
            Status.Text = message.Length > 0 ? message : view.Phase switch
            {
                Phase.Loading => "ページを読み込み中…", Phase.ReadingImage => "画像から本文を読み取り中…",
                Phase.Translating => "日本語に翻訳中…", Phase.Empty => "このページには読み取れる本文がありません。",
                Phase.Password => "パスワードが必要です。", _ => ""
            };
            if (view.Phase is not (Phase.Loading or Phase.ReadingImage or Phase.Translating)) timer.Stop();
            if (view.Phase == Phase.Password) AskPassword();
        }
        finally { Core.reader_snapshot_free(snapshot); }
    }
}

internal static class PdfPicker
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
    public static string? Open(IntPtr owner)
    {
        const int capacity = 32768;
        IntPtr buffer = Marshal.AllocHGlobal(capacity * 2);
        try
        {
            Marshal.WriteInt16(buffer, 0);
            var data = new OpenFileName { size = Marshal.SizeOf<OpenFileName>(), owner = owner,
                filter = "PDF (*.pdf)\0*.pdf\0\0", file = buffer, maxFile = capacity,
                flags = 0x02000000 | 0x00001000 | 0x00000800 | 0x00000008 | 0x00080000 }; // no recent documents, existing file/path, keep cwd, Explorer
            return GetOpenFileNameW(ref data) ? Marshal.PtrToStringUni(buffer) : null;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }
}
