# Packages the Release x64 build as foo-true-random.fb2k-component.
#
# A .fb2k-component file is a ZIP archive. foobar2000 v2 loads 64-bit
# component DLLs from the archive's "x64" subfolder; the archive root is for
# 32-bit DLLs, which this component deliberately does not ship.
param(
    [string]$Configuration = "Release",
    [string]$OutDir = (Join-Path $PSScriptRoot "..\dist")
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$dll = Join-Path $root "build\x64\$Configuration\foo_true_random.dll"
if (-not (Test-Path $dll)) { throw "Build output not found: $dll" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = Join-Path (Resolve-Path $OutDir) "foo-true-random.fb2k-component"
Remove-Item -Force -ErrorAction SilentlyContinue $out
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [System.IO.Compression.ZipFile]::Open($out, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $dll, "x64/foo_true_random.dll",
        [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
} finally { $z.Dispose() }
$stage = Join-Path ([System.IO.Path]::GetTempPath()) "ftr-package-verify"
New-Item -ItemType Directory -Force -Path $stage | Out-Null
# Verify: the archive must contain exactly x64/foo_true_random.dll, byte-identical to the build.
$z = [System.IO.Compression.ZipFile]::OpenRead($out)
try {
    $entries = @($z.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    Write-Host "Archive entries: $($entries -join ', ')"
    if ($entries.Count -ne 1 -or $entries[0] -ne "x64/foo_true_random.dll") { throw "unexpected archive layout" }
    $tmp = Join-Path $stage "verify.dll"
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($z.Entries[0], $tmp, $true)
} finally { $z.Dispose() }
if ((Get-FileHash $tmp).Hash -ne (Get-FileHash $dll).Hash) { throw "packaged DLL differs from build output" }
Write-Host "Package: $out"
Write-Host "SHA-256: $((Get-FileHash $out -Algorithm SHA256).Hash)"
