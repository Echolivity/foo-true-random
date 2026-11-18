# Real-foobar2000 integration test.
#
# 1. Installs foobar2000 v2 (x64) silently into a throw-away PORTABLE folder,
#    so an existing foobar2000 installation and profile are never touched.
# 2. Installs the SHIPPED component from dist\foo-true-random.fb2k-component
#    plus the separate test-driver component foo_true_random_itest.dll.
# 3. Starts foobar2000 with FTR_ITEST_REPORT set; the driver runs the shipped
#    commands via foobar2000's menu services, writes a report and exits.
#
# Usage (from the repository root, after scripts\build.ps1 and building the
# driver with: msbuild foobar2000\foo_true_random\foo_true_random.vcxproj
#   /p:Configuration=Release /p:Platform=x64 /p:FTR_ITEST=true):
#   ./scripts/integration-test.ps1
param(
    [string]$Fb2kVersion = "2.26.3",
    # SHA-256 of foobar2000-x64_v2.26.3.exe as downloaded from foobar2000.org on 2026-11-17.
    [string]$Fb2kSha256 = "B8A92891013D3AF81EE7B51393E85E963DC099E7E4958F8CF392BFA252425183",
    [string]$InstallDir = "C:\ftr-fb2k-itest",
    [string]$Package = (Join-Path $PSScriptRoot "..\dist\foo-true-random.fb2k-component"),
    [string]$Driver = (Join-Path $PSScriptRoot "..\build\x64\Release\foo_true_random_itest.dll"),
    [int]$TimeoutSeconds = 180
)
$ErrorActionPreference = "Stop"
$Package = (Resolve-Path $Package).Path
$Driver = (Resolve-Path $Driver).Path

Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class FtrWin {
    delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr w, EnumProc f, IntPtr p);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    static string T(IntPtr h) { var s = new StringBuilder(1024); GetWindowText(h, s, 1024); return s.ToString(); }
    static string C(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    public static List<string> Dump(int pid) {
        var o = new List<string>();
        EnumWindows((h, p) => {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp == pid && IsWindowVisible(h)) {
                o.Add("window [" + C(h) + "] '" + T(h) + "'");
                EnumChildWindows(h, (c, q) => { var t = T(c); if (t.Length > 0) o.Add("    [" + C(c) + "] " + t); return true; }, IntPtr.Zero);
            }
            return true; }, IntPtr.Zero);
        return o;
    }
}
"@

function Stop-Fb2k {
    Get-Process foobar2000 -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$InstallDir*" } | Stop-Process -Force -ErrorAction SilentlyContinue
}

# --- 1. install foobar2000 portable
$installer = Join-Path ([System.IO.Path]::GetTempPath()) "foobar2000-x64_v$Fb2kVersion.exe"
if (-not (Test-Path $installer)) {
    Invoke-WebRequest -UseBasicParsing "https://www.foobar2000.org/downloads/foobar2000-x64_v$Fb2kVersion.exe" -OutFile $installer
}
$installerHash = (Get-FileHash $installer -Algorithm SHA256).Hash
Write-Host "Installer SHA-256: $installerHash"
if ($Fb2kVersion -eq "2.26.3" -and $installerHash -ne $Fb2kSha256) { throw "foobar2000 installer hash mismatch (expected $Fb2kSha256)" }
if (Test-Path $InstallDir) { Stop-Fb2k; Remove-Item -Recurse -Force $InstallDir }
Start-Process -Wait -FilePath $installer -ArgumentList "/S", "/D=$InstallDir"
$exe = Join-Path $InstallDir "foobar2000.exe"
if (-not (Test-Path $exe)) { throw "foobar2000 was not installed to $InstallDir" }
New-Item -ItemType File -Force -Path (Join-Path $InstallDir "portable_mode_enabled") | Out-Null
Write-Host "foobar2000 file version: $((Get-Item $exe).VersionInfo.FileVersion)"

# First run initialises the portable profile; then close it gracefully.
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 15
[FtrWin]::Dump($p.Id) | ForEach-Object { "first run: $_" }
Start-Process -FilePath $exe -ArgumentList "/exit" -Wait
$null = $p.WaitForExit(30000)
if (-not $p.HasExited) { Write-Host "first run did not exit gracefully; killing"; Stop-Fb2k }
Start-Sleep -Seconds 2
$profileDir = @((Join-Path $InstallDir "profile"), $InstallDir) | Where-Object { Test-Path $_ } | Select-Object -First 1
Write-Host "Profile folder: $profileDir"
Get-ChildItem $profileDir | Select-Object -ExpandProperty Name | ForEach-Object { "  $_" }

# --- 2. install shipped component (from the package) + test driver
$uc = Join-Path $profileDir "user-components-x64"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$dest = Join-Path $uc "foo_true_random"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$z = [System.IO.Compression.ZipFile]::OpenRead($Package)
try {
    $entry = $z.Entries | Where-Object { $_.FullName -eq "x64/foo_true_random.dll" }
    if (-not $entry) { throw "package does not contain x64/foo_true_random.dll" }
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $dest "foo_true_random.dll"), $true)
} finally { $z.Dispose() }
$ddest = Join-Path $uc "foo_true_random_itest"
New-Item -ItemType Directory -Force -Path $ddest | Out-Null
Copy-Item $Driver (Join-Path $ddest "foo_true_random_itest.dll")

# --- test media: 40 short silent WAV files
$media = Join-Path $InstallDir "itest-media"
New-Item -ItemType Directory -Force -Path $media | Out-Null
$samples = 4410; $dataLen = $samples * 2
for ($k = 0; $k -lt 40; $k++) {
    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter($ms)
    $w.Write([Text.Encoding]::ASCII.GetBytes("RIFF")); $w.Write([int](36 + $dataLen))
    $w.Write([Text.Encoding]::ASCII.GetBytes("WAVEfmt ")); $w.Write([int]16); $w.Write([int16]1); $w.Write([int16]1)
    $w.Write([int]44100); $w.Write([int]88200); $w.Write([int16]2); $w.Write([int16]16)
    $w.Write([Text.Encoding]::ASCII.GetBytes("data")); $w.Write([int]$dataLen); $w.Write((New-Object byte[] $dataLen))
    [IO.File]::WriteAllBytes((Join-Path $media ("t{0:D2}.wav" -f $k)), $ms.ToArray())
}

# --- 3. run
$report = Join-Path $InstallDir "itest-report.txt"
Remove-Item -Force -ErrorAction SilentlyContinue $report
$env:FTR_ITEST_REPORT = $report
$env:FTR_ITEST_MEDIA = $media
$p = Start-Process -FilePath $exe -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
if (-not $p.HasExited) {
    Write-Host "foobar2000 did not exit within $TimeoutSeconds s. Visible windows of the process:"
    [FtrWin]::Dump($p.Id) | ForEach-Object { "  $_" }
    if (Test-Path $report) { "Partial report:"; Get-Content $report }
    Stop-Fb2k
}
Remove-Item Env:FTR_ITEST_REPORT, Env:FTR_ITEST_MEDIA

if (-not (Test-Path $report)) {
    Write-Host "No report produced. Profile tree:"
    Get-ChildItem -Recurse $profileDir -Depth 2 | Select-Object -ExpandProperty FullName | Select-Object -First 80
    throw "integration test produced no report (component or driver not loaded?)"
}
Get-Content $report
if (-not (Select-String -Path $report -Pattern "^RESULT: PASS" -Quiet)) { throw "integration test FAILED" }
Write-Host "Integration test passed."
