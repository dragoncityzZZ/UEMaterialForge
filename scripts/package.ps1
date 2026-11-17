<#
.SYNOPSIS
    Builds the portable Windows release of MaterialForge and packages it.

.DESCRIPTION
    The script creates a Release build with a static MSVC runtime and a Windows
    GUI-subsystem executable, then copies the runtime content into dist/ and
    creates a zip archive. Run it from a Visual Studio Developer PowerShell, or
    make cmake and ninja available on PATH.

.PARAMETER BuildDirectory
    CMake build directory. Defaults to <repo>/build-release.

.PARAMETER DistDirectory
    Output directory for the portable folder and zip. Defaults to <repo>/dist.
#>
[CmdletBinding()]
param(
    [string]$BuildDirectory = "",
    [string]$DistDirectory = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo "build-release" }
if (-not $DistDirectory) { $DistDirectory = Join-Path $repo "dist" }

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmakeCommand) {
    throw "cmake was not found on PATH. Run this script from a Visual Studio Developer PowerShell."
}
$ninjaCommand = Get-Command ninja -ErrorAction SilentlyContinue

$version = "2.5.0"
$cmakeText = Get-Content (Join-Path $repo "CMakeLists.txt") -Raw
if ($cmakeText -match 'project\(MaterialForge VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    $version = $Matches[1]
}

$configureArguments = @(
    "-S", $repo,
    "-B", $BuildDirectory,
    "-DCMAKE_BUILD_TYPE=Release",
    "-DMF_STATIC_RUNTIME=ON",
    "-DMF_WIN32_GUI=ON",
    "-DMF_BUILD_TESTS=OFF"
)
if ($ninjaCommand) {
    $configureArguments += @("-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=$($ninjaCommand.Source)")
}

& $cmakeCommand.Source @configureArguments
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
& $cmakeCommand.Source --build $BuildDirectory --config Release
if ($LASTEXITCODE -ne 0) { throw "CMake build failed." }

$exe = Join-Path $BuildDirectory "bin\MaterialForge.exe"
if (-not (Test-Path $exe)) { throw "Build did not produce $exe." }

$stageName = "MaterialForge-$version-win64"
$stage = Join-Path $DistDirectory $stageName
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item -LiteralPath $exe -Destination $stage
foreach ($directory in @("assets", "examples", "docs", "tools\ue5")) {
    $source = Join-Path $repo $directory
    if (Test-Path $source) {
        $destination = Join-Path $stage $directory
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Recurse
    }
}
Get-ChildItem -LiteralPath $stage -Recurse -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -eq "__pycache__" } |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath $stage -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -eq ".pyc" } |
    Remove-Item -Force -ErrorAction SilentlyContinue
$glslangSource = Join-Path $repo "third_party\glslang\bin"
if (Test-Path $glslangSource) {
    $glslangDirectory = Join-Path $stage "tools\glslang"
    New-Item -ItemType Directory -Path $glslangDirectory -Force | Out-Null
    Copy-Item -Path (Join-Path $glslangSource "*") -Destination $glslangDirectory -Force
    $glslangLicense = Join-Path $repo "third_party\glslang\LICENSE"
    if (Test-Path $glslangLicense) {
        Copy-Item -LiteralPath $glslangLicense -Destination (Join-Path $glslangDirectory "LICENSE")
    }
}
foreach ($file in @("README.md", "THIRD_PARTY_NOTICES.md")) {
    $source = Join-Path $repo $file
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination $stage
    }
}

$archive = Join-Path $DistDirectory "$stageName.zip"
if (Test-Path $archive) { Remove-Item -LiteralPath $archive -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $archive -CompressionLevel Optimal

Write-Host ""
Write-Host "Portable build: $stage"
Write-Host "Zip archive:    $archive"
