param(
    [switch]$Test,
    [switch]$Werror
)

$ErrorActionPreference = "Stop"

$vsDevCmd = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $vsDevCmd)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installDir = & $vswhere -latest -products * -property installationPath
        if ($installDir) {
            $vsDevCmd = Join-Path $installDir "Common7\Tools\VsDevCmd.bat"
        }
    }
}

if (-not (Test-Path $vsDevCmd)) {
    Write-Error "Visual Studio Build Tools (VsDevCmd.bat) not found."
    exit 1
}

$rootDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $rootDir "build"

# If HandsOff.dll is currently loaded in explorer.exe, hot-unload it before linking so the binary can be overwritten
$existingExe = Join-Path $rootDir "HandsOff.exe"
if (Test-Path $existingExe) {
    & $existingExe uninstall | Out-Null
    Start-Sleep -Milliseconds 250
}

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$werrorFlag = if ($Werror) { "ON" } else { "OFF" }
$cmd = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 && cmake -S `"$rootDir`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DHANDSOFF_WERROR=$werrorFlag -DBUILD_TESTING=ON && cmake --build `"$buildDir`" --config Release"
if ($Test) {
    $cmd += " && ctest --test-dir `"$buildDir`" --output-on-failure --no-tests=error"
}

Write-Host "[HandsOff] Building Release x64 targets..." -ForegroundColor Cyan
cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build or tests failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "[HandsOff] Build succeeded! Binaries in: $rootDir" -ForegroundColor Green
Get-ChildItem $rootDir -Include "HandsOff.exe", "HandsOff.dll" -Name | ForEach-Object {
    Get-Item (Join-Path $rootDir $_) | Select-Object Name, Length
}
