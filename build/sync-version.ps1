param(
    [switch]$Check
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$Version = (Get-Content (Join-Path $RepoRoot "VERSION") -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw "VERSION must contain a numeric version." }
$Parts = $Version.Split('.')
$RcVersion = if ($Parts.Count -eq 3) { "$($Parts[0]),$($Parts[1]),$($Parts[2]),0" } else { $Parts -join "," }

$updates = @(
    @{ Path = "src\native\CMakeLists.txt"; Pattern = 'project\(TaskbarWidgetsNative VERSION [^ ]+'; Replacement = "project(TaskbarWidgetsNative VERSION $Version" },
    @{ Path = "src\native\loader\loader.rc"; Pattern = '(?m)^ FILEVERSION [0-9,]+'; Replacement = " FILEVERSION $RcVersion" },
    @{ Path = "src\native\loader\loader.rc"; Pattern = '(?m)^ PRODUCTVERSION [0-9,]+'; Replacement = " PRODUCTVERSION $RcVersion" },
    @{ Path = "src\native\loader\loader.rc"; Pattern = 'VALUE "FileVersion", "[^"]+"'; Replacement = "VALUE `"FileVersion`", `"$Version`"" },
    @{ Path = "src\native\loader\loader.rc"; Pattern = 'VALUE "ProductVersion", "[^"]+"'; Replacement = "VALUE `"ProductVersion`", `"$Version`"" }
)

$changed = @()
foreach ($update in $updates) {
    $path = Join-Path $RepoRoot $update.Path
    $current = [IO.File]::ReadAllText($path)
    $next = [Text.RegularExpressions.Regex]::Replace($current, $update.Pattern, $update.Replacement, 1)
    if ($next -ne $current) {
        $changed += $update.Path
        if (-not $Check) { [IO.File]::WriteAllText($path, $next, [Text.UTF8Encoding]::new($false)) }
    }
}

if ($Check -and $changed.Count -gt 0) {
    throw "Version metadata is out of sync with VERSION: $($changed -join ', ')"
}
Write-Host "Version metadata validated: $Version"
