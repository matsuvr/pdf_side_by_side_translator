param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$pdf = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../fixtures/P01-simple.pdf')).Path
$process = Start-Process -FilePath (Resolve-Path -LiteralPath $Exe).Path -ArgumentList ('"' + $pdf + '"') -WindowStyle Hidden -PassThru
function Control([string]$id) {
    $window = [System.Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle)
    $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
    return $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function WaitText([string]$expected) {
    $until = (Get-Date).AddMinutes(2)
    $ready = -join ([char]0x5B8C, [char]0x4E86)
    do {
        $process.Refresh()
        if ($process.HasExited) { throw "Viewer exited with code $($process.ExitCode)" }
        if ($process.MainWindowHandle -ne 0) {
            $translated = Control 'Translation'
            $status = Control 'TranslationStatus'
            if ($translated -and $status -and $status.Current.Name -eq $ready) {
                $text = $translated.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern).DocumentRange.GetText(-1)
                if ($text -match $expected) { return $text.TrimEnd() }
            }
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $until)
    throw "Timed out waiting for page text"
}
function Click([string]$id) {
    (Control $id).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
try {
    $first = WaitText '3'
    if ($first -notmatch '[\u3040-\u30ff]') { throw 'Expected a Japanese translation' }
    $range = (Control 'Translation').GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern).DocumentRange
    $range.MoveEndpointByRange([System.Windows.Automation.Text.TextPatternRangeEndpoint]::End, $range, [System.Windows.Automation.Text.TextPatternRangeEndpoint]::Start)
    $range.ExpandToEnclosingUnit([System.Windows.Automation.Text.TextUnit]::Character)
    Write-Output ("WinUI translation font: " + $range.GetAttributeValue([System.Windows.Automation.TextPattern]::FontNameAttribute))
    Click 'Next'
    $null = WaitText '12'
    Click 'Previous'
    if ((WaitText '3') -ne $first) { throw 'Revisited page differs' }
    $viewer = Control 'Translation'
    $value = $viewer.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    if (!$value.Current.IsReadOnly) { throw 'Translation is editable' }
    if (Control 'SourceMode') { throw 'Unexpected source toggle' }
    $zoom = (Control 'ZoomPercent').Current.BoundingRectangle
    $pageNumber = (Control 'PageNumber').Current.BoundingRectangle
    if ($zoom.Left -le $pageNumber.Right -or [Math]::Abs($zoom.Top - $pageNumber.Top) -gt 10) {
        throw 'Zoom and page number are in different rows'
    }
    Write-Output 'WinUI displayed both read-only translations and placed the zoom alongside the page number.'
} finally {
    if (!$process.HasExited) { $null = $process.CloseMainWindow(); $null = $process.WaitForExit(10000) }
}
