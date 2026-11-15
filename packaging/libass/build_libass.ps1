# Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
# Apache License 2.0, see LICENSE and NOTICE.
#
# Builds libass and the libraries it needs with vcpkg, at the vcpkg commit that vcpkg.json beside this script names as
# its builtin-baseline, and copies the DLLs a Windows package ships beside tsMuxeR.exe into -Out. tsMuxeR is not linked
# against them: it loads ass-9.dll when an ASS or SSA track is drawn (tsMuxer/assRender.cpp).
#
#   build_libass.ps1 -Triplet x64-windows -Work <scratch folder> -Out <folder for the DLLs>
#
# Used by .github/workflows/windows_64.yml (x64-windows) and windows_32.yml (x86-windows).
param(
    [Parameter(Mandatory = $true)][string]$Triplet,
    [Parameter(Mandatory = $true)][string]$Work,
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = "Stop"

# The DLLs libass loads, and nothing else: ass-9.dll needs freetype, fribidi and harfbuzz; freetype needs libpng, zlib,
# bzip2 and the Brotli decoder. The other DLLs vcpkg builds (harfbuzz-subset and the like) are not used.
$dlls = "ass-9", "freetype", "harfbuzz", "fribidi-0", "libpng16", "z", "bz2", "brotlidec", "brotlicommon"

$commit = (Get-Content (Join-Path $PSScriptRoot "vcpkg.json") -Raw | ConvertFrom-Json).'builtin-baseline'
if (-not $commit) { throw "vcpkg.json names no builtin-baseline" }
New-Item -ItemType Directory -Force $Work | Out-Null
$vcpkg = Join-Path $Work "vcpkg"
if (-not (Test-Path (Join-Path $vcpkg "vcpkg.exe"))) {
    New-Item -ItemType Directory -Force $vcpkg | Out-Null
    git -C $vcpkg init -q
    git -C $vcpkg fetch -q --depth 1 https://github.com/microsoft/vcpkg.git $commit
    if ($LASTEXITCODE -ne 0) { throw "fetching vcpkg $commit failed" }
    git -C $vcpkg checkout -q FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw "checking out vcpkg $commit failed" }
    & (Join-Path $vcpkg "bootstrap-vcpkg.bat") -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }
}

# Run from the scratch folder, where no vcpkg.json is found: vcpkg then builds the ports of that commit as they are
# (classic mode), and the vcpkg.json of the repository root, for developer builds, is not read.
Push-Location $Work
try {
    & (Join-Path $vcpkg "vcpkg.exe") install "libass:$Triplet" --disable-metrics
    if ($LASTEXITCODE -ne 0) { throw "vcpkg install libass:$Triplet failed" }
} finally {
    Pop-Location
}

$bin = Join-Path $vcpkg "installed\$Triplet\bin"
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($dll in $dlls) {
    $src = Join-Path $bin "$dll.dll"
    if (-not (Test-Path $src)) { throw "$dll.dll was not built (looked in $bin)" }
    Copy-Item $src $Out -Force
}
Write-Host "libass DLLs for $Triplet in ${Out}:" ($dlls -join ", ")
