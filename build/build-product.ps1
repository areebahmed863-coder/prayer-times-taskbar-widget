param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Content (Join-Path $RepoRoot "VERSION") -Raw).Trim()
}
if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw "Invalid version: $Version" }

$NativeSource = Join-Path $RepoRoot "src\native"
$NativeBuild = Join-Path $RepoRoot "artifacts\native-build"
$PublishDir = Join-Path $RepoRoot "artifacts\TaskbarWidgets"

function Find-CMake {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in @(
        (Join-Path $RepoRoot "artifacts\tools\python-cmake\cmake\data\bin\cmake.exe"),
        (Join-Path $RepoRoot "artifacts\tools\cmake-python\cmake\data\bin\cmake.exe")
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    $roots = @(
        "C:\Program Files\Microsoft Visual Studio\2022",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022"
    )
    foreach ($root in $roots) {
        $candidate = Get-ChildItem $root -Filter cmake.exe -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
        if ($candidate) { return $candidate }
    }
    throw "CMake/MSVC was not found. Install Visual Studio 2022 Build Tools with Desktop development with C++."
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

& (Join-Path $PSScriptRoot "generate-widget-catalog.ps1")

Remove-Item -Recurse -Force $PublishDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $PublishDir, $NativeBuild | Out-Null
Get-Process "TaskbarWidgets" -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process "TaskbarWidgets.MediaHelper" -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

$cmake = Find-CMake
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsFound = $false
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not [string]::IsNullOrWhiteSpace($vsPath)) { $vsFound = $true }
}

$useNinja = $false
if (-not $vsFound -and (Import-VsDevEnv)) {
    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    if (-not $ninja) {
        $ninjaCandidate = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022" -Filter ninja.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
        if ($ninjaCandidate) { $env:PATH = "$(Split-Path $ninjaCandidate -Parent);$env:PATH"; $ninja = $ninjaCandidate }
    }
    if ($ninja) { $useNinja = $true }
}

if ($useNinja) {
    & $cmake -S $NativeSource -B $NativeBuild -G "Ninja Multi-Config" -DBUILD_TESTING=ON
} else {
    & $cmake -S $NativeSource -B $NativeBuild -A x64 -DBUILD_TESTING=ON
}
if ($LASTEXITCODE -ne 0) { throw "Native configure failed." }

& $cmake --build $NativeBuild --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Native build failed." }

$NativeOutput = Join-Path $NativeBuild $Configuration
$LoaderOutput = Join-Path $NativeOutput "TaskbarWidgets.exe"
$HookOutput = Join-Path $NativeOutput "TaskbarWidgets.Hook.dll"
$MediaHelperOutput = Join-Path $NativeOutput "TaskbarWidgets.MediaHelper.exe"

if (-not (Test-Path $LoaderOutput)) { throw "Loader output missing: $LoaderOutput" }
if (-not (Test-Path $HookOutput)) { throw "Hook DLL output missing: $HookOutput" }
if (-not (Test-Path $MediaHelperOutput)) { throw "Media helper output missing: $MediaHelperOutput" }

Copy-Item -Force $LoaderOutput (Join-Path $PublishDir "TaskbarWidgets.exe")
Copy-Item -Force $HookOutput (Join-Path $PublishDir "TaskbarWidgets.Hook.dll")
Copy-Item -Force $MediaHelperOutput (Join-Path $PublishDir "TaskbarWidgets.MediaHelper.exe")

$AssetsWidgetsDir = Join-Path $PublishDir "Assets\widgets"
New-Item -ItemType Directory -Force $AssetsWidgetsDir | Out-Null
$MediaCover = Join-Path $RepoRoot "widgets\media-player\assets\media_cover.png"
if (Test-Path $MediaCover) {
    Copy-Item -Force $MediaCover (Join-Path $AssetsWidgetsDir "media_cover.png")
}

$PortableReadme = Join-Path $PublishDir "README-PORTABLE.txt"
Set-Content $PortableReadme "Run TaskbarWidgets.exe. Right-click or left-click the tray icon to configure Prayer Times, toggle Media Player, or open Data\config.json." -Encoding UTF8

Write-Host "Product output: $PublishDir"
Get-ChildItem $PublishDir | Select-Object Name, Length, LastWriteTime
