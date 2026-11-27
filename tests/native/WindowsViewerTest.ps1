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
    do {
        $process.Refresh()
        if ($process.HasExited) { throw "Viewer exited with code $($process.ExitCode)" }
        if ($process.MainWindowHandle -ne 0) {
            $translated = Control 'Translation'
            if ($translated -and $translated.Current.Name -match $expected) { return $translated.Current.Name }
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
    (Control 'SourceMode').GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
    $deadline = (Get-Date).AddSeconds(5)
    do {
        $source = Control 'Source'
        if ($source -and $source.Current.Name -match '3 sensors') { break }
        Start-Sleep -Milliseconds 50
    } while ((Get-Date) -lt $deadline)
    if (!$source -or $source.Current.Name -notmatch '3 sensors') { throw 'Selectable source text is missing' }
    foreach ($id in @('Translation', 'Source')) {
        $control = Control $id
        $value = $null
        if ($control.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$value) -and !$value.Current.IsReadOnly) { throw "Editable body: $id" }
        $null = $control.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
    }
    Write-Output 'WinUI displayed both translations and read-only source text.'
} finally {
    if (!$process.HasExited) { $null = $process.CloseMainWindow(); $null = $process.WaitForExit(10000) }
}
