param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

function Find-CMake {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in @(
        (Join-Path $RepoRoot "artifacts\tools\python-cmake\cmake\data\bin\cmake.exe"),
        (Join-Path $RepoRoot "artifacts\tools\cmake-python\cmake\data\bin\cmake.exe")
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    foreach ($root in @("C:\Program Files\Microsoft Visual Studio\2022", "C:\Program Files (x86)\Microsoft Visual Studio\2022")) {
        $candidate = Get-ChildItem $root -Filter cmake.exe -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
        if ($candidate) { return $candidate }
    }
    throw "CMake/MSVC is required for native verification."
}

function Import-VsDevEnv {
    $vcvars = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $vcvars) { return $false }
    $lines = cmd /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) { return $false }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }
    return $true
}

& (Join-Path $PSScriptRoot "sync-version.ps1") -Check
& (Join-Path $PSScriptRoot "generate-widget-catalog.ps1") -Check

$cmake = Find-CMake
$ctest = Join-Path (Split-Path $cmake -Parent) "ctest.exe"
$nativeBuild = Join-Path $RepoRoot "artifacts\native-verify"

$useNinja = $false
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsFound = $false
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not [string]::IsNullOrWhiteSpace($vsPath)) { $vsFound = $true }
}

if (-not $vsFound -and (Import-VsDevEnv)) {
    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    if (-not $ninja) {
        $ninjaCandidate = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022" -Filter ninja.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
        if ($ninjaCandidate) { $env:PATH = "$(Split-Path $ninjaCandidate -Parent);$env:PATH"; $ninja = $ninjaCandidate }
    }
    if ($ninja) { $useNinja = $true }
}

if ($useNinja) {
    & $cmake -S (Join-Path $RepoRoot "src\native") -B $nativeBuild -G "Ninja Multi-Config" -DBUILD_TESTING=ON
} else {
    & $cmake -S (Join-Path $RepoRoot "src\native") -B $nativeBuild -A x64 -DBUILD_TESTING=ON
}
if ($LASTEXITCODE -ne 0) { throw "Native configure failed." }

& $cmake --build $nativeBuild --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Native build failed." }

& $ctest --test-dir $nativeBuild -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Native tests failed." }

Write-Host "Taskbar Widgets Lite verification completed."
