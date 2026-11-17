# Downloads and extracts the official foobar2000 SDK into external\foobar2000-sdk.
# The SHA-256 below was recorded from https://www.foobar2000.org/downloads/SDK-2026-10-01.7z
# on 2026-11-17; the script refuses to continue if the download differs.
param(
    [string]$Version = "2026-10-01",
    [string]$Sha256 = "D4C55077336FAE81BF8DF0259B5B2748FA45EA84132C656EAD93EB123CBCDC26",
    [string]$Dest = (Join-Path $PSScriptRoot "..\external\foobar2000-sdk")
)
$ErrorActionPreference = "Stop"
$url = "https://www.foobar2000.org/downloads/SDK-$Version.7z"
$archive = Join-Path ([System.IO.Path]::GetTempPath()) "SDK-$Version.7z"
Write-Host "Downloading $url"
Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
$hash = (Get-FileHash $archive -Algorithm SHA256).Hash
Write-Host "SHA-256: $hash"
if ($Version -eq "2026-10-01" -and $hash -ne $Sha256) { throw "SDK archive hash mismatch (expected $Sha256)" }
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
$sevenZip = Get-Command 7z -ErrorAction SilentlyContinue
if ($sevenZip) { & 7z x $archive "-o$Dest" -y | Out-Null }
else { & tar -xf $archive -C $Dest }
if ($LASTEXITCODE -ne 0) { throw "extraction failed" }
if (-not (Test-Path (Join-Path $Dest "foobar2000\SDK\foobar2000.h"))) { throw "SDK layout not found after extraction" }
Write-Host "SDK $Version extracted to $Dest"
