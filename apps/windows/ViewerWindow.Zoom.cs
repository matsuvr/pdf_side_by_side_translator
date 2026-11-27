using System;
using System.Globalization;
using System.IO;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.System;

namespace OfflinePdf;

public sealed partial class ViewerWindow
{
    private enum ZoomMode { Page, Width, Manual }
    private static readonly double[] ZoomSteps = [0.05, 0.10, 0.125, 0.25, 0.33, 0.5, 0.67, 0.75, 1, 1.25, 1.5, 2, 3, 4, 6, 8, 12, 16];
    private ZoomMode zoomMode = ZoomMode.Page;
    private double paperWidth, paperHeight, manualScale = 1, actualScale = 1;
    private int rotation;

    private void SetPaper(Core.View view)
    {
        paperWidth = view.PageWidth * (96.0 / 72);
        paperHeight = view.PageHeight * (96.0 / 72);
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
        UpdateZoomControls();
        UpdatePaperSize();
        DispatcherQueue.TryEnqueue(() => PaperScroll.ChangeView(0, 0, null, true));
    }

    private void UpdateZoomControls()
    {
        bool available = paperWidth > 0 && paperHeight > 0;
        ZoomPercent.IsEnabled = ZoomPreset.IsEnabled = FitWidthItem.IsEnabled = FitPageItem.IsEnabled
            = RotateLeftItem.IsEnabled = RotateRightItem.IsEnabled = available;
        Smaller.IsEnabled = available && actualScale > ZoomSteps[0] + 0.001;
        Larger.IsEnabled = available && actualScale < ZoomSteps[^1] - 0.001;
        if (!available) ZoomPercent.Text = "";
    }

    private void PaperViewportChanged(object sender, SizeChangedEventArgs e) => UpdatePaperSize();

    private void HeaderResized(object sender, SizeChangedEventArgs e) => UpdatePageCount();

    private void UpdatePageCount()
    {
        if (ZoomPreset is null || PageCount is null) return;
        bool roomy = OriginalHeader.ActualWidth >= 470;
        PageCount.Text = count == 0 ? "" : roomy ? $"/ {count} ページ" : $"/ {count}";
        ZoomPreset.Width = roomy ? 128 : 88;
    }

    private void UpdatePaperSize()
    {
        if (paperWidth <= 0 || paperHeight <= 0)
        {
            PaperCanvas.Width = PaperCanvas.Height = 0;
            return;
        }
        bool sideways = rotation % 180 != 0;
        double width = sideways ? paperHeight : paperWidth;
        double height = sideways ? paperWidth : paperHeight;
        // Canvas has an 8 DIP margin. Leave an extra DIP for layout rounding.
        double fitWidth = Math.Max(1, PaperScroll.ActualWidth - 17) / width;
        double fitHeight = Math.Max(1, PaperScroll.ActualHeight - 17) / height;
        actualScale = zoomMode switch
        {
            ZoomMode.Page => Math.Min(fitWidth, fitHeight),
            ZoomMode.Width => fitWidth,
            _ => manualScale
        };
        PaperCanvas.Width = width * actualScale;
        PaperCanvas.Height = height * actualScale;
        Paper.Width = paperWidth * actualScale;
        Paper.Height = paperHeight * actualScale;
        Canvas.SetLeft(Paper, (PaperCanvas.Width - Paper.Width) / 2);
        Canvas.SetTop(Paper, (PaperCanvas.Height - Paper.Height) / 2);
        PaperAngle.Angle = rotation;
        UpdateZoomControls();
        if (ZoomPercent.FocusState == FocusState.Unfocused) ShowZoomPercent();
    }

    private void ShowZoomPercent()
    {
        ZoomPercent.Text = ZoomPercent.IsEnabled ? $"{actualScale * 100:0.#}%" : "";
    }

    private void SetZoom(ZoomMode mode, double scale = 1)
    {
        zoomMode = mode;
        if (mode == ZoomMode.Manual) manualScale = Math.Clamp(scale, ZoomSteps[0], ZoomSteps[^1]);
        if (ZoomPreset.SelectedIndex != (int)mode) ZoomPreset.SelectedIndex = (int)mode;
        UpdatePaperSize();
        if (ZoomPercent.IsEnabled) ShowZoomPercent();
    }

    private void ZoomPresetChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ZoomPreset is null || ZoomPreset.SelectedIndex < 0) return;
        var selected = (ZoomMode)ZoomPreset.SelectedIndex;
        if (selected != zoomMode) SetZoom(selected, actualScale);
    }

    private void ZoomEntered(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != VirtualKey.Enter) return;
        e.Handled = true;
        var entry = ZoomPercent.Text.Trim().TrimEnd('%', '％');
        if ((double.TryParse(entry, NumberStyles.Float, CultureInfo.CurrentCulture, out var value)
             || double.TryParse(entry, NumberStyles.Float, CultureInfo.InvariantCulture, out value))
            && double.IsFinite(value) && value > 0)
            SetZoom(ZoomMode.Manual, value / 100);
        ShowZoomPercent();
        ZoomPercent.SelectAll();
    }

    private void ZoomLostFocus(object sender, RoutedEventArgs e) => ShowZoomPercent();
    private void ZoomOut(object sender, RoutedEventArgs e) => StepZoom(false);
    private void ZoomIn(object sender, RoutedEventArgs e) => StepZoom(true);
    private void StepZoom(bool enlarge)
    {
        if (!ZoomPercent.IsEnabled) return;
        int index = enlarge
            ? Array.FindIndex(ZoomSteps, scale => scale > actualScale + 0.001)
            : Array.FindLastIndex(ZoomSteps, scale => scale < actualScale - 0.001);
        if (index >= 0) SetZoom(ZoomMode.Manual, ZoomSteps[index]);
    }

    private void ZoomWheel(object sender, PointerRoutedEventArgs e)
    {
        if ((e.KeyModifiers & VirtualKeyModifiers.Control) == 0) return;
        int delta = e.GetCurrentPoint(PaperScroll).Properties.MouseWheelDelta;
        if (delta == 0) return;
        e.Handled = true;
        StepZoom(delta > 0);
    }

    private void FitWidth(object sender, RoutedEventArgs e) => SetZoom(ZoomMode.Width);
    private void FitPage(object sender, RoutedEventArgs e) => SetZoom(ZoomMode.Page);
    private void RotateLeft(object sender, RoutedEventArgs e) => RotatePaper(-90);
    private void RotateRight(object sender, RoutedEventArgs e) => RotatePaper(90);
    private void RotatePaper(int degrees)
    {
        rotation = (rotation + degrees + 360) % 360;
        UpdatePaperSize();
    }

    private void ResizePanes(object sender, DragDeltaEventArgs e)
    {
        double total = Panes.ActualWidth - Panes.ColumnDefinitions[1].ActualWidth;
        if (total <= 0) return;
        double minimum = Math.Min(320, total / 2);
        double left = Math.Clamp(Panes.ColumnDefinitions[0].ActualWidth + e.HorizontalChange, minimum, total - minimum);
        Panes.ColumnDefinitions[0].Width = new GridLength(left, GridUnitType.Star);
        Panes.ColumnDefinitions[2].Width = new GridLength(total - left, GridUnitType.Star);
    }

    private void EqualPanes(object sender, RoutedEventArgs e)
    {
        Panes.ColumnDefinitions[0].Width = new GridLength(1, GridUnitType.Star);
        Panes.ColumnDefinitions[2].Width = new GridLength(1, GridUnitType.Star);
    }
}
