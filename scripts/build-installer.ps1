# Build the Windows installer from the existing Release build.
# Usage: powershell -ExecutionPolicy Bypass -File .\scripts\build-installer.ps1

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$ReleaseBin = Join-Path $Root "build-release\bin"
$Exe = Join-Path $ReleaseBin "MaterialForge.exe"
if (-not (Test-Path $Exe)) {
    throw "Release executable not found: $Exe. Build build-release first."
}

function Find-SignTool {
    $Kits = @(
        "C:\Program Files (x86)\Windows Kits\10\bin",
        "C:\Program Files\Windows Kits\10\bin"
    )
    foreach ($RootPath in $Kits) {
        if (-not (Test-Path $RootPath)) { continue }
        $Tool = Get-ChildItem $RootPath -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($Tool) { return $Tool.FullName }
    }
    return $null
}

function Sign-IfConfigured([string]$File) {
    $Pfx = $env:MATERIALFORGE_SIGN_PFX
    $Thumbprint = $env:MATERIALFORGE_SIGN_THUMBPRINT
    if ([string]::IsNullOrWhiteSpace($Pfx) -and [string]::IsNullOrWhiteSpace($Thumbprint)) {
        return
    }
    $SignTool = Find-SignTool
    if (-not $SignTool) {
        throw "A signing certificate was configured but signtool.exe was not found."
    }
    $Arguments = @("sign", "/fd", "SHA256")
    if (-not [string]::IsNullOrWhiteSpace($Pfx)) {
        if (-not (Test-Path $Pfx)) { throw "Code signing PFX not found: $Pfx" }
        $Arguments += @("/f", $Pfx)
        if (-not [string]::IsNullOrWhiteSpace($env:MATERIALFORGE_SIGN_PASSWORD)) {
            $Arguments += @("/p", $env:MATERIALFORGE_SIGN_PASSWORD)
        }
    } else {
        $Arguments += @("/sha1", $Thumbprint)
    }
    $Arguments += @("/tr", "http://timestamp.digicert.com", "/td", "SHA256", $File)
    & $SignTool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed while signing $File."
    }
}

Sign-IfConfigured $Exe

$Payload = Join-Path $Root "build\installer-payload"
if (Test-Path $Payload) {
    Remove-Item $Payload -Recurse -Force
}
New-Item -ItemType Directory -Path $Payload -Force | Out-Null

Copy-Item $Exe (Join-Path $Payload "MaterialForge.exe") -Force
foreach ($RuntimeDll in @("vulkan-1.dll", "d3dcompiler_47.dll")) {
    $RuntimePath = Join-Path $ReleaseBin $RuntimeDll
    if (Test-Path $RuntimePath) {
        Copy-Item $RuntimePath (Join-Path $Payload $RuntimeDll) -Force
    } else {
        throw "Required runtime file is missing from the Release build: $RuntimePath"
    }
}
foreach ($Folder in @("assets", "examples", "tools")) {
    $Source = Join-Path $ReleaseBin $Folder
    if (Test-Path $Source) {
        Copy-Item $Source (Join-Path $Payload $Folder) -Recurse -Force
    }
}
Copy-Item (Join-Path $Root "docs") (Join-Path $Payload "docs") -Recurse -Force
Copy-Item (Join-Path $Root "README.md") $Payload -Force
Copy-Item (Join-Path $Root "CHANGELOG.md") $Payload -Force
Copy-Item (Join-Path $Root "THIRD_PARTY_NOTICES.md") $Payload -Force

$Candidates = @(
    (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe"),
    (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
)
$Iscc = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Iscc) {
    throw "Inno Setup 6 ISCC.exe was not found."
}

$Script = Join-Path $Root "installer\MaterialForge.iss"
& $Iscc $Script
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup failed with exit code $LASTEXITCODE."
}

$Setup = Join-Path $Root "dist\installer\MaterialForge-2.6.5-Setup.exe"
if (-not (Test-Path $Setup)) {
    throw "Installer output was not created: $Setup"
}
Sign-IfConfigured $Setup
Write-Host "Installer: $Setup"
