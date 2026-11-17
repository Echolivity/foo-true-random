# Builds Debug|x64 and Release|x64 and packages the Release build.
# Run from a "Developer PowerShell for VS 2022" (or newer) so msbuild is on PATH.
param([switch]$SkipFetch)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $SkipFetch -and -not (Test-Path (Join-Path $root "external\foobar2000-sdk\foobar2000\SDK\foobar2000.h"))) {
    & (Join-Path $PSScriptRoot "fetch-sdk.ps1")
}
$proj = Join-Path $root "foobar2000\foo_true_random\foo_true_random.vcxproj"
foreach ($cfg in "Debug", "Release") {
    & msbuild $proj /m /nologo /v:minimal "/p:Configuration=$cfg" "/p:Platform=x64"
    if ($LASTEXITCODE -ne 0) { throw "msbuild $cfg|x64 failed" }
}
& (Join-Path $PSScriptRoot "package.ps1")
