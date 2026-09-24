$ErrorActionPreference = 'Stop'

$qtRoot = if ($env:QT_ROOT) { $env:QT_ROOT } else { 'C:\Qt\6.8.3\msvc2022_64' }
$cmakePath = (Get-Command cmake -ErrorAction Stop).Source
$windeployqt = Join-Path $qtRoot 'bin\windeployqt.exe'
if (-not (Test-Path $windeployqt)) { throw "Missing $windeployqt; set QT_ROOT to the Qt MSVC x64 install directory." }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio 2026 C++ x64 toolset is required.' }
$env:VCINSTALLDIR = Join-Path $vsInstall 'VC\'
$toolset = Get-ChildItem (Join-Path $vsInstall 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if ($toolset) { $env:VCToolsInstallDir = $toolset.FullName + '\' }

$build = Join-Path $PSScriptRoot 'build-vs2026'
$dist = Join-Path $PSScriptRoot 'dist'
$qtPrefix = '-DCMAKE_PREFIX_PATH=' + $qtRoot
& $cmakePath -S $PSScriptRoot -B $build -G 'Visual Studio 18 2026' -A x64 $qtPrefix
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $cmakePath --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }

if (Test-Path $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null
$exe = Join-Path $build 'Release\vscode-mcp.exe'
Copy-Item -LiteralPath $exe -Destination $dist
& $windeployqt --release --no-translations --compiler-runtime (Join-Path $dist 'vscode-mcp.exe')
if ($LASTEXITCODE -ne 0) { throw 'windeployqt deploy failed' }
$crtRoot = Join-Path $vsInstall ("VC\Redist\MSVC\{0}\x64" -f $toolset.Name)
$crtDir = Get-ChildItem $crtRoot -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'Microsoft.VC*.CRT' } | Select-Object -First 1
if ($crtDir) { Copy-Item -Path (Join-Path $crtDir.FullName '*.dll') -Destination $dist -Force }
$clangdVersion = '22.1.6'
$clangdCache = Join-Path $PSScriptRoot '.cache'
$clangdArchive = Join-Path $clangdCache "clangd-windows-$clangdVersion.zip"
$clangdExpanded = Join-Path $clangdCache "clangd_$clangdVersion"
if (-not (Test-Path (Join-Path $clangdExpanded 'bin\clangd.exe'))) {
    New-Item -ItemType Directory -Force -Path $clangdCache | Out-Null
    if (-not (Test-Path $clangdArchive)) {
        Invoke-WebRequest "https://github.com/clangd/clangd/releases/download/$clangdVersion/clangd-windows-$clangdVersion.zip" -OutFile $clangdArchive
    }
    Expand-Archive -LiteralPath $clangdArchive -DestinationPath $clangdCache -Force
}
$lspDir = Join-Path $dist 'lsp\clangd'
New-Item -ItemType Directory -Force -Path $lspDir | Out-Null
Copy-Item -Path (Join-Path $clangdExpanded '*') -Destination $lspDir -Recurse -Force
Write-Host "Done: $dist"
