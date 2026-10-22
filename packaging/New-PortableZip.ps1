<#
.SYNOPSIS
  Builds the Windows x64 portable ZIP of AI Orchestrator from a finished RELEASE build tree.

.DESCRIPTION
  The package is assembled in a clean staging folder instead of being cut out of the build tree (which is mixed with CMake
  artifacts): the executable and the vcpkg libraries are copied there and windeployqt is run on that folder with the same
  options as the CMake post-build step, so the COMPLETE windeployqt result is kept and nothing is pruned. The MSVC runtime DLLs
  the binaries import are then copied app-local from the Visual Studio redistributable that matches the compiler. Nothing from the
  user's profile (database, keys, logs, settings) is ever read or copied.

  The script refuses to continue when: the tree is not a Release tree of this source, the source has local changes (unless
  -AllowDirty, which is recorded), any binary imports a DLL that is neither in the package nor a Windows system DLL, any binary
  imports a debug runtime, or the package would contain forbidden file types.

  Output (in -OutDir): AIOrchestrator-<version>-win-x64.zip, its .sha256, and the staged folder.
  Everything that identifies the build (source SHA, configuration, Qt, MSVC, vcpkg, CRT origin) is written to BUILD-INFO.txt inside the package.

.EXAMPLE
  .\packaging\New-PortableZip.ps1 -SourceRoot C:\aio_rc -BuildDir C:\aio_rc\out\build\x64-release
#>
param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$OutDir = '',
    [Parameter(Mandatory = $true)][string]$SourceBundleDir,
    [string]$QtSourceRoot = '',
    [string]$QtDeclarativeSourceRoot = '',
    [switch]$AllowDirty
)
$ErrorActionPreference = 'Stop'
if ($OutDir -eq '') { $OutDir = Join-Path $SourceRoot 'out\package' }

function Fail([string]$message) { throw "PACKAGING REFUSED: $message" }
function Info([string]$message) { Write-Host "[package] $message" }

# ---------- 1. what is being packaged ----------
$cachePath = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path $cachePath)) { Fail "no CMakeCache.txt in $BuildDir" }
$cache = @{}
foreach ($line in Get-Content $cachePath) { if ($line -match '^([^#/][^:=]*):[A-Z]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] } }
if ($cache['CMAKE_BUILD_TYPE'] -ne 'Release') { Fail "the build tree is '$($cache['CMAKE_BUILD_TYPE'])', not Release" }
$configuredFrom = ($cache['CMAKE_HOME_DIRECTORY'] -replace '/', '\').TrimEnd('\')
if ((Resolve-Path $SourceRoot).Path.TrimEnd('\') -ne $configuredFrom) { Fail "the build tree was configured from '$configuredFrom', not from '$SourceRoot'" }

$sha = (git -C $SourceRoot rev-parse HEAD).Trim()
$dirtyLines = @(git -C $SourceRoot status --porcelain --untracked-files=no)
$dirty = $dirtyLines.Count -gt 0
if ($dirty -and -not $AllowDirty) { Fail "the source has local changes (use -AllowDirty to record and accept that):`n$($dirtyLines -join "`n")" }

$appBuild = Join-Path $BuildDir 'UI_AI_Orchestrator'
$exe = Join-Path $appBuild 'appUI_AI_Orchestrator.exe'
if (-not (Test-Path $exe)) { Fail "no executable at $exe" }
$exeInfo = (Get-Item $exe).VersionInfo
$version = $exeInfo.ProductVersion
if (-not $version) { Fail 'the executable has no ProductVersion resource' }
if (-not $exeInfo.IsPreRelease -and $version -match '-') { Fail "version '$version' has a suffix but the pre-release flag is not set" }
Info "source $sha (dirty=$dirty), version $version, tree $BuildDir"

# ---------- 2. tools ----------
$compiler = ($cache['CMAKE_CXX_COMPILER'] -replace '/', '\')
if (-not (Test-Path $compiler)) { Fail "compiler not found: $compiler" }
$binDir = Split-Path $compiler
$dumpbin = Join-Path $binDir 'dumpbin.exe'
if (-not (Test-Path $dumpbin)) { Fail "dumpbin.exe not next to the compiler: $dumpbin" }
$windeployqt = ($cache['AI_ORCHESTRATOR_WINDEPLOYQT'] -replace '/', '\')
if (-not (Test-Path $windeployqt)) { Fail "windeployqt not found: $windeployqt" }
if ($compiler -notmatch '^(.*\\VC)\\Tools\\MSVC\\([^\\]+)\\bin\\') { Fail "cannot derive the toolset from $compiler" }
$vcRoot = $Matches[1]; $toolset = $Matches[2]
$crtSource = Get-ChildItem (Join-Path $vcRoot "Redist\MSVC\$toolset\x64") -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $crtSource) { Fail "no redistributable CRT folder for toolset $toolset under $vcRoot\Redist\MSVC" }

# ---------- 3. stage ----------
$name = "AIOrchestrator-$version-win-x64"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$outFull = (Resolve-Path $OutDir).Path
$stage = Join-Path $outFull $name
if (Test-Path $stage) {
    if ((Resolve-Path $stage).Path -notlike "$outFull\*") { Fail "refusing to delete outside the output folder: $stage" }
    Remove-Item $stage -Recurse -Force
}
New-Item -ItemType Directory $stage | Out-Null
Copy-Item $exe $stage
# The libraries that are not Qt's: windeployqt recreates everything Qt, so only what the build brought from vcpkg is copied.
$qtDeployed = '^(Qt6|d3dcompiler_|dxcompiler|dxil|opengl32sw)'
$thirdParty = @(Get-ChildItem $appBuild -File -Filter *.dll | Where-Object { $_.Name -notmatch $qtDeployed })
foreach ($dll in $thirdParty) { Copy-Item $dll.FullName $stage }
Info "third-party DLLs: $(($thirdParty | ForEach-Object { $_.Name }) -join ', ')"

# ---------- 4. windeployqt: the complete result, same options as the CMake post-build step ----------
$stagedExe = Join-Path $stage 'appUI_AI_Orchestrator.exe'
$deployArgs = @('--release', '--qmldir', (Join-Path $SourceRoot 'UI_AI_Orchestrator\qml'), '--no-compiler-runtime', '--no-translations', $stagedExe)
# Inside the Visual Studio developer environment, like the CMake post-build step: only there does windeployqt find dxcompiler.dll and
# dxil.dll of the Windows SDK. (Its warnings go to stderr, which must not become a terminating error here.)
$vcvars = Join-Path $vcRoot 'Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { Fail "vcvars64.bat not found: $vcvars" }
$quotedArgs = ($deployArgs | ForEach-Object { '"' + $_ + '"' }) -join ' '
$previousPreference = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
$deployOutput = @(cmd /c "`"$vcvars`" >nul 2>&1 && `"$windeployqt`" $quotedArgs 2>&1" | ForEach-Object { "$_" })
$deployExit = $LASTEXITCODE
$ErrorActionPreference = $previousPreference
if ($deployExit -ne 0) { Fail "windeployqt failed ($deployExit):`n$($deployOutput -join "`n")" }
Info "windeployqt done ($($deployOutput.Count) lines of output)"

# ---------- 5. imports: app-local CRT, nothing unresolved, no debug runtime ----------
function Get-ImportMap([string[]]$files) {
    # file -> imported DLL names (one dumpbin process per batch).
    $map = @{}
    for ($i = 0; $i -lt $files.Count; $i += 40) {
        $batch = $files[$i..([Math]::Min($i + 39, $files.Count - 1))]
        $current = $null
        foreach ($line in (& $dumpbin /nologo /dependents @batch 2>&1)) {
            if ($line -match '^Dump of file (.+)$') { $current = $Matches[1].Trim(); $map[$current] = New-Object System.Collections.Generic.List[string] }
            elseif ($current -and $line -match '^\s+(\S+\.dll)\s*$') { $map[$current].Add($Matches[1]) }
        }
    }
    return $map
}
$binaries = @(Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.dll', '.exe' } | ForEach-Object { $_.FullName })
$imports = Get-ImportMap $binaries
if ($imports.Count -ne $binaries.Count) { Fail "dumpbin listed $($imports.Count) of $($binaries.Count) binaries" }
$stageNames = @{}
Get-ChildItem $stage -Recurse -File -Filter *.dll | ForEach-Object { $stageNames[$_.Name.ToLowerInvariant()] = $true }

$crtPattern = '^(vcruntime|msvcp|concrt|vccorlib)\d+[_a-z0-9]*\.dll$'
$wantedCrt = @($imports.Values | ForEach-Object { $_ } | Where-Object { $_ -match $crtPattern } | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object -Unique)
$debugRuntime = @($imports.Values | ForEach-Object { $_ } | Where-Object { $_ -match '(vcruntime|msvcp|concrt|ucrtbase)\d*[_a-z0-9]*d\.dll$' } | Sort-Object -Unique)
if ($debugRuntime.Count -gt 0) { Fail "a binary imports a debug runtime: $($debugRuntime -join ', ')" }
$crtCopied = @()
foreach ($crt in $wantedCrt) {
    $source = Join-Path $crtSource.FullName $crt
    if (-not (Test-Path $source)) { Fail "$crt is imported but not in the redistributable folder $($crtSource.FullName)" }
    Copy-Item $source $stage
    $stageNames[$crt] = $true
    $crtCopied += Get-Item (Join-Path $stage $crt)
}
Info "app-local CRT: $($wantedCrt -join ', ') from $($crtSource.FullName)"

# Licence notices are part of the package, not a later hand-off-only attachment.
$qtPrefix = Split-Path (Split-Path $windeployqt)
& (Join-Path $PSScriptRoot 'Add-PackageLicences.ps1') -SourceRoot $SourceRoot -BuildDir $BuildDir `
    -QtPrefix $qtPrefix -Stage $stage -QtSourceRoot $QtSourceRoot -QtDeclarativeSourceRoot $QtDeclarativeSourceRoot
$sourceInfoPath = Join-Path $SourceBundleDir 'SOURCE-INFO.txt'
if (!(Test-Path $sourceInfoPath)) { Fail 'Corresponding-source archives must be prepared first (New-SourceBundle.ps1)' }
& (Join-Path $PSScriptRoot 'Assert-SourceBundle.ps1') -SourceBundleDir $SourceBundleDir -SourceSha $sha -Version $version
Copy-Item -LiteralPath $sourceInfoPath -Destination (Join-Path $stage 'SOURCE-INFO.txt')

$system32 = Join-Path $env:windir 'System32'
$unresolved = @(); $systemUsed = @{}
foreach ($file in $imports.Keys) {
    foreach ($dep in $imports[$file]) {
        $low = $dep.ToLowerInvariant()
        if ($stageNames.ContainsKey($low) -or $low -like 'api-ms-win-*' -or $low -like 'ext-ms-win-*') { continue }
        if (Test-Path (Join-Path $system32 $dep)) { $systemUsed[$low] = $true; continue }
        $unresolved += "$([IO.Path]::GetFileName($file)) -> $dep"
    }
}
if ($unresolved.Count -gt 0) { Fail "imports resolved neither in the package nor in Windows:`n$(($unresolved | Sort-Object -Unique) -join "`n")" }

# ---------- 6. content audit ----------
$forbidden = @(Get-ChildItem $stage -Recurse -Force | Where-Object {
    $_.Name -match '\.(sqlite|sqlite3|db|db3|wal|shm|owner|pdb|ilk|lib|exp|obj|log|env|pem|key|pfx)(-wal|-shm)?$' -or
    $_.Name -match '^(CMakeFiles|CMakeCache\.txt|cmake_install\.cmake|CTestTestfile\.cmake|\.qt|\.rcc)$' -or
    $_.Name -match '(_tests|Test)\.exe$' })
if ($forbidden.Count -gt 0) { Fail "forbidden content in the package:`n$(($forbidden | ForEach-Object { $_.FullName.Substring($stage.Length + 1) }) -join "`n")" }

# ---------- 7. facts ----------
$vcpkgRoot = $null
if ($cache['CMAKE_TOOLCHAIN_FILE']) { $vcpkgRoot = (($cache['CMAKE_TOOLCHAIN_FILE'] -replace '/', '\') -replace '\\scripts\\buildsystems\\vcpkg\.cmake$', '') }
$vcpkgCommit = if ($vcpkgRoot -and (Test-Path (Join-Path $vcpkgRoot '.git'))) { (git -C $vcpkgRoot rev-parse HEAD).Trim() } else { 'unknown (not a git checkout)' }
$vcpkgInfo = Get-ChildItem (Join-Path $BuildDir 'vcpkg_installed\vcpkg\info') -Filter '*.list' -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(curl|sqlite3|zlib)_' } | ForEach-Object { $_.BaseName }
$qtVersion = (Get-Item (Join-Path $stage 'Qt6Core.dll')).VersionInfo.ProductVersion
$qtDir = ($cache['Qt6_DIR'] -replace '/', '\')
$clText = (& cmd /c "`"$compiler`" 2>&1" | Select-Object -First 1) -join ''
$cmakeVersion = (cmake --version | Select-Object -First 1)

$files = @(Get-ChildItem $stage -Recurse -File)
$totalMb = [Math]::Round((($files | Measure-Object Length -Sum).Sum) / 1MB, 1)
$buildInfo = New-Object System.Collections.Generic.List[string]
$buildInfo.Add("AI Orchestrator $version - Windows x64 portable package")
$buildInfo.Add('')
$buildInfo.Add("Source commit       : $sha")
$buildInfo.Add("Source tree dirty   : $dirty$(if ($dirty) { ' (' + ($dirtyLines -join '; ') + ')' })")
$buildInfo.Add("Configuration       : Release ($($cache['CMAKE_CXX_FLAGS_RELEASE']))")
$buildInfo.Add("Executable version  : FileVersion $($exeInfo.FileVersion), ProductVersion $($exeInfo.ProductVersion), pre-release flag $($exeInfo.IsPreRelease)")
$buildInfo.Add("Qt                  : $qtVersion ($qtDir)")
$buildInfo.Add("MSVC                : $clText; toolset $toolset")
$buildInfo.Add("CMake / generator   : $cmakeVersion / $($cache['CMAKE_GENERATOR'])")
$buildInfo.Add("vcpkg               : commit $vcpkgCommit, triplet $($cache['VCPKG_TARGET_TRIPLET']); $(@($vcpkgInfo) -join ', ')")
$buildInfo.Add("windeployqt         : $windeployqt $($deployArgs -join ' ')  (the complete result is kept; nothing was pruned)")
$buildInfo.Add('')
$buildInfo.Add("MSVC runtime (app-local, copied unchanged from the Visual Studio redistributable of the compiler's toolset):")
$buildInfo.Add("  origin: $($crtSource.FullName)")
foreach ($crt in $crtCopied) { $buildInfo.Add(('  {0,-28} {1}  sha256 {2}' -f $crt.Name, $crt.VersionInfo.FileVersion, (Get-FileHash $crt.FullName -Algorithm SHA256).Hash.ToLowerInvariant())) }
$buildInfo.Add('  The Universal CRT (api-ms-win-crt-*) is part of Windows 10/11 and is not bundled.')
$buildInfo.Add('')
$buildInfo.Add("Windows system DLLs the package imports ($($systemUsed.Count)), none resolved from anywhere else:")
$buildInfo.Add('  ' + (($systemUsed.Keys | Sort-Object) -join ', '))
$buildInfo.Add('')
$buildInfo.Add("Files: $($files.Count + 3) (including README.txt, this file and windeployqt-output.txt), $totalMb MB uncompressed. No database, key, log, settings or debug file is included.")
$buildInfo.Add('The package was NOT yet verified on a clean Windows machine unless a verification report says so.')
$infoDir = Join-Path $stage 'BUILD-INFO'
New-Item -ItemType Directory $infoDir | Out-Null
Set-Content -Path (Join-Path $infoDir 'BUILD-INFO.txt') -Value $buildInfo -Encoding UTF8
Set-Content -Path (Join-Path $infoDir 'windeployqt-output.txt') -Value $deployOutput -Encoding UTF8
$readme = (Get-Content (Join-Path $PSScriptRoot 'README-portable.txt') -Raw).Replace('@VERSION@', $version)
Set-Content -Path (Join-Path $stage 'README.txt') -Value $readme -Encoding UTF8

# ---------- 8. zip, round-trip check, checksum ----------
$zip = Join-Path $outFull "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Push-Location $outFull
try { & tar.exe -a -c -f "$name.zip" $name; if ($LASTEXITCODE -ne 0) { Fail "tar failed ($LASTEXITCODE)" } } finally { Pop-Location }
$entries = @(& tar.exe -t -f $zip | Where-Object { $_ -and -not $_.EndsWith('/') })
$stageFiles = @(Get-ChildItem $stage -Recurse -File)
if ($entries.Count -ne $stageFiles.Count) { Fail "the zip has $($entries.Count) files, the staged folder $($stageFiles.Count)" }
if ($entries | Where-Object { $_ -match '\\' }) { Fail 'the zip has entries with backslashes' }
$check = Join-Path $outFull "_roundtrip_$name"
if (Test-Path $check) { Remove-Item $check -Recurse -Force }
New-Item -ItemType Directory $check | Out-Null
& tar.exe -x -f $zip -C $check
if ($LASTEXITCODE -ne 0) { Fail "extracting the zip failed ($LASTEXITCODE)" }
foreach ($file in $stageFiles) {
    $relative = $file.FullName.Substring($stage.Length + 1)
    $copy = Join-Path (Join-Path $check $name) $relative
    if (-not (Test-Path $copy) -or (Get-FileHash $copy -Algorithm SHA256).Hash -ne (Get-FileHash $file.FullName -Algorithm SHA256).Hash) { Fail "round trip differs for $relative" }
}
Remove-Item $check -Recurse -Force
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path "$zip.sha256" -Value "$hash  $name.zip" -Encoding ASCII
Info "ZIP   : $zip ($([Math]::Round((Get-Item $zip).Length / 1MB, 1)) MB, $($entries.Count) files, round trip identical)"
Info "SHA256: $hash"
[pscustomobject]@{ Zip = $zip; Sha256 = $hash; SourceSha = $sha; Version = $version; Files = $entries.Count }
