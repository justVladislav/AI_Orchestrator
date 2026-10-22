param(
    [Parameter(Mandatory = $true)][string]$SourceBundleDir,
    [Parameter(Mandatory = $true)][string]$SourceSha,
    [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = 'Stop'
$info = Get-Content (Join-Path $SourceBundleDir 'SOURCE-INFO.txt') -Raw -Encoding UTF8
if (($info -split '\r?\n') -notcontains "Application source commit: $SourceSha") { throw 'Source commit mismatch' }
if (($info -split '\r?\n') -notcontains "AI Orchestrator $Version corresponding source") { throw 'Source version mismatch' }
$expected = @("AIOrchestrator-$Version-source.zip", 'Qt-6.11.1-corresponding-source.tar.gz', 'vcpkg-dependency-sources.zip')
$seen = @{}
foreach ($line in ($info -split '\r?\n')) {
    if ($line -match '^([0-9a-f]{64})  (.+)$') {
        $hash = $Matches[1]; $name = $Matches[2]
        if ($name -notin $expected -or $seen.ContainsKey($name)) { throw "Unexpected or duplicate source archive: $name" }
        $file = Join-Path $SourceBundleDir $name
        if (!(Test-Path $file -PathType Leaf) -or (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) {
            throw "Missing or changed source archive: $name"
        }
        $seen[$name] = $true
    }
}
if ($seen.Count -ne $expected.Count) { throw 'Incomplete corresponding-source manifest' }
