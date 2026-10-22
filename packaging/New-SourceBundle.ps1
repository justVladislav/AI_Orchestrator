<# Creates companion corresponding-source archives. Keep these beside the binary ZIP.
   Existing archives are never overwritten. Inputs are local, public dependency sources. #>
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$QtSourceRoot = 'C:/Qt/6.11.1/Src',
    [string]$QtDeclarativeSourceRoot = '',
    [string]$VcpkgRoot = 'C:/c++_libs_tools/vcpkg'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
if (@(git -C $SourceRoot status --porcelain --untracked-files=no).Count) { throw 'Source archive requires a clean tracked tree' }
$sha = (git -C $SourceRoot rev-parse HEAD).Trim()
$app = Join-Path $OutDir "AIOrchestrator-$Version-source.zip"
$qt = Join-Path $OutDir 'Qt-6.11.1-corresponding-source.tar.gz'
$deps = Join-Path $OutDir 'vcpkg-dependency-sources.zip'
foreach ($file in @($app, $qt, $deps)) { if (Test-Path $file) { throw "Refusing to overwrite $file" } }
& git -C $SourceRoot archive --format=zip "--prefix=AIOrchestrator-$Version-source/" "--output=$app" HEAD
if ($LASTEXITCODE -ne 0) { throw 'Application source archive failed' }
$modules = @('qtbase', 'qtdeclarative', 'qtimageformats', 'qtlottie', 'qtquick3d', 'qtvirtualkeyboard',
    'qtsvg', 'qtshadertools', 'qtquicktimeline', 'qtlanguageserver', 'qttools')
$qtRoots = @('CMakeLists.txt', 'configure', 'configure.bat', 'configure.json', 'cmake', 'coin', 'LICENSES',
    'README.md', 'init-repository.pl', 'init-repository.bat', '.gitmodules', '.tag') + $modules
Write-Host '[source] Compressing Qt source, vendored code, licences and build scripts (this can take several minutes).'
$qtArgs = @('--exclude=.git', '-czf', $qt, '-C', $QtSourceRoot)
if ($QtDeclarativeSourceRoot) {
    if ((Split-Path $QtDeclarativeSourceRoot -Leaf) -ne 'qtdeclarative') { throw 'Override must be named qtdeclarative' }
    $qtArgs += @($qtRoots | Where-Object { $_ -ne 'qtdeclarative' })
    $qtArgs += @('-C', (Split-Path $QtDeclarativeSourceRoot), 'qtdeclarative')
} else { $qtArgs += $qtRoots }
& tar.exe @qtArgs
if ($LASTEXITCODE -ne 0) { throw 'Qt source archive failed' }
# Original vcpkg downloads plus the port recipes/patches used to build them.
$downloads = @('curl-curl-curl-8_20_0.tar.gz', 'sqlite-autoconf-3530100.tar.gz', 'madler-zlib-v1.3.2.tar.gz')
foreach ($file in $downloads) { if (!(Test-Path (Join-Path $VcpkgRoot "downloads/$file"))) { throw "Missing dependency source: $file" } }
$ports = @('ports/curl', 'ports/sqlite3', 'ports/zlib', 'ports/vcpkg-cmake', 'ports/vcpkg-cmake-config', 'scripts', 'LICENSE.txt')
& tar.exe -a -cf $deps -C $VcpkgRoot @ports -C (Join-Path $VcpkgRoot 'downloads') @downloads
if ($LASTEXITCODE -ne 0) { throw 'vcpkg source archive failed' }
$manifest = @("AI Orchestrator $Version corresponding source", "Application source commit: $sha",
    "vcpkg build-tool revision: $((git -C $VcpkgRoot rev-parse HEAD).Trim())",
    'Qt: 6.11.1; upstream source revisions recorded in LICENCES/Qt/*/SOURCE-REVISION.txt.',
    "qtdeclarative source override: $(if ($QtDeclarativeSourceRoot) { (git -C $QtDeclarativeSourceRoot rev-parse HEAD).Trim() } else { 'installed source' })",
    'Qt configuration and per-module SPDX inventory: see LICENCES/Qt in the binary ZIP.',
    'vcpkg sources: curl 8.20.0, SQLite 3.53.1, zlib 1.3.2; port patches and scripts included.',
    'Application build: see README.md, subsystem READMEs, CMakePresets.json and BUILD-INFO.',
    'Publish all three archives alongside the binary ZIP, with the same access permissions.',
    'Microsoft runtime/SDK components remain subject to Microsoft terms, not GPL.', '')
foreach ($file in @($app, $qt, $deps)) {
    $hash = (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest += "$hash  $([IO.Path]::GetFileName($file))"
    Set-Content "$file.sha256" "$hash  $([IO.Path]::GetFileName($file))" -Encoding ASCII
}
Set-Content (Join-Path $OutDir 'SOURCE-INFO.txt') $manifest -Encoding UTF8
Write-Host "[source] Source archives prepared for $sha"
