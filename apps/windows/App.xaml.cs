using System;
using System.IO;
using Microsoft.UI.Xaml;

namespace OfflinePdf;

public partial class App : Application
{
    private ViewerWindow? window;
    public App() => InitializeComponent();
    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        window = new ViewerWindow();
        window.Activate();
        var argv = Environment.GetCommandLineArgs();
        if (argv.Length == 2 && File.Exists(argv[1])) window.Open(Path.GetFullPath(argv[1]));
    }
}
