# Run from a Visual Studio developer PowerShell with the Windows SDK on PATH.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ModelFile,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{40}$')]
    [string]$CertificateThumbprint,
    [Parameter(Mandatory)][ValidatePattern('^https?://[^\s]+$')]
    [string]$TimestampUrl
)

$ErrorActionPreference = 'Stop'
$ModelFile = (Resolve-Path -LiteralPath $ModelFile).ProviderPath
$target = 'windows-x64-release'
$root = Split-Path -Parent $PSScriptRoot
foreach ($command in @('git', 'cmake', 'ctest', 'python', 'signtool.exe', 'dumpbin.exe')) {
    Get-Command -Name $command -CommandType Application -ErrorAction Stop | Out-Null
}

Push-Location -LiteralPath $root
try {
    $changes = git status --porcelain
    if ($LASTEXITCODE -ne 0) { throw 'git status failed.' }
    if ($changes) { throw 'Commit reviewed source before building a release.' }
    cmake --preset $target
    if ($LASTEXITCODE -ne 0) { throw 'Release configure failed.' }
    cmake --build --preset $target
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
    ctest --preset $target --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Release tests failed.' }
    python scripts/package.py --target $target --model-file $ModelFile `
        --windows-cert $CertificateThumbprint --windows-timestamp-url $TimestampUrl
    if ($LASTEXITCODE -ne 0) { throw 'Signing or packaging failed.' }
}
finally {
    Pop-Location
}
