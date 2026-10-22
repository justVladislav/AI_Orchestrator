param([string]$SourceRoot = (Split-Path (Split-Path $PSScriptRoot)))
$ErrorActionPreference = 'Stop'
$check = Join-Path $SourceRoot 'packaging/Assert-SourceBundle.ps1'
$fixture = Join-Path $SourceRoot ('out/source-bundle-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $fixture | Out-Null
$sha = '0123456789012345678901234567890123456789'
$good = @('AI Orchestrator 1.0.0 corresponding source', "Application source commit: $sha")
foreach ($name in @('AIOrchestrator-1.0.0-source.zip', 'Qt-6.11.1-corresponding-source.tar.gz', 'vcpkg-dependency-sources.zip')) {
    $file = Join-Path $fixture $name
    [IO.File]::WriteAllText($file, 'fixture, not a real source archive')
    $good += "$((Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant())  $name"
}
function Assert-Case([string]$Name, [string[]]$Lines, [string]$ErrorText = '') {
    Set-Content (Join-Path $fixture 'SOURCE-INFO.txt') $Lines -Encoding UTF8
    $failure = $null
    try { & $check -SourceBundleDir $fixture -SourceSha $sha -Version '1.0.0' } catch { $failure = $_.Exception.Message }
    if (!$ErrorText -and $failure) { throw "$Name unexpectedly failed: $failure" }
    if ($ErrorText -and (!$failure -or !$failure.Contains($ErrorText))) { throw "$Name did not reject as expected: $failure" }
    Write-Host "PASS $Name"
}
Assert-Case 'valid source bundle' $good
Assert-Case 'wrong commit rejected' @($good -replace $sha, ('f' * 40)) 'Source commit mismatch'
Assert-Case 'wrong version rejected' @($good -replace '1.0.0 corresponding', '1.0.1 corresponding') 'Source version mismatch'
Assert-Case 'missing archive entry rejected' $good[0..3] 'Incomplete'
Assert-Case 'duplicate archive rejected' ($good + $good[2]) 'duplicate'
Assert-Case 'path traversal rejected' @($good -replace 'vcpkg-dependency-sources.zip', '../vcpkg-dependency-sources.zip') 'Unexpected'
[IO.File]::AppendAllText((Join-Path $fixture 'vcpkg-dependency-sources.zip'), 'changed')
Assert-Case 'changed archive rejected' $good 'changed'

# Exercise the real collector without a Qt/SDK installation. It must collect the
# compiled-in header's notice before reaching the intentionally absent Qt DLL.
$collector = Join-Path $SourceRoot 'packaging/Add-PackageLicences.ps1'
$headerRelative = 'First_AI_Agents/First_AI_Agents/includes/nlohmann/json.hpp'
$licenceRelative = 'packaging/licences/nlohmann-json-3.12.0-MIT.txt'
$jsonRoot = Join-Path $fixture 'json-source'
$jsonStage = Join-Path $fixture 'json-stage'
foreach ($relative in @($headerRelative, $licenceRelative)) {
    $destination = Join-Path $jsonRoot $relative
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath (Join-Path $SourceRoot $relative) -Destination $destination
}
foreach ($name in @('LICENSE', 'COPYRIGHT', 'THIRD-PARTY-NOTICES.txt')) {
    Copy-Item -LiteralPath (Join-Path $SourceRoot $name) -Destination $jsonRoot
}
$licenceText = (Get-Content -LiteralPath (Join-Path $jsonRoot $licenceRelative) -Raw -Encoding UTF8) -replace "`r`n", "`n" -replace '(?m)[ \t]+$', ''
$hasher = [Security.Cryptography.SHA256]::Create()
try {
    $licenceHash = [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($licenceText.TrimEnd() + "`n"))).Replace('-', '').ToLowerInvariant()
} finally { $hasher.Dispose() }
# Upstream v3.12.0 LICENSE.MIT, normalised for line endings/trailing whitespace.
if ($licenceHash -ne 'c0d068392ea65358b798b8c165103560f06e9e3b38c4ab4e2d8810a7b931af86') {
    throw 'The full upstream nlohmann/json MIT text has changed or is incomplete.'
}
Write-Host 'PASS complete upstream nlohmann/json MIT text'
function Invoke-JsonCollector {
    try {
        & $collector -SourceRoot $jsonRoot -BuildDir $fixture -QtPrefix $fixture -Stage $jsonStage
    } catch { return $_ }
    throw 'Collector unexpectedly succeeded without a Qt DLL.'
}
$failure = Invoke-JsonCollector
$missingQt = Join-Path $jsonStage 'Qt6Core.dll'
if ($failure.TargetObject -ne $missingQt) { throw "Collector did not reach the expected Qt boundary: $failure" }
$copiedLicence = Join-Path $jsonStage 'LICENCES/nlohmann-json.txt'
if (!(Test-Path -LiteralPath $copiedLicence -PathType Leaf) -or
    (Get-FileHash -LiteralPath $copiedLicence).Hash -ne (Get-FileHash -LiteralPath (Join-Path $jsonRoot $licenceRelative)).Hash) {
    throw 'Collector did not stage the full nlohmann/json MIT notice unchanged.'
}
$overview = Get-Content -LiteralPath (Join-Path $jsonStage 'THIRD-PARTY-NOTICES.txt') -Raw
if (!$overview.Contains('nlohmann/json 3.12.0') -or !$overview.Contains('LICENCES/nlohmann-json.txt')) {
    throw 'Third-party overview does not identify the bundled nlohmann/json notice.'
}
Write-Host 'PASS real collector stages nlohmann/json notice and overview'

Remove-Item -LiteralPath (Join-Path $jsonRoot $licenceRelative)
$failure = Invoke-JsonCollector
if ($failure.Exception.Message -ne 'Missing nlohmann/json MIT licence.') { throw "Missing notice was not rejected: $failure" }
Write-Host 'PASS missing nlohmann/json notice rejected'
Copy-Item -LiteralPath (Join-Path $SourceRoot $licenceRelative) -Destination (Join-Path $jsonRoot $licenceRelative)
$header = Get-Content -LiteralPath (Join-Path $jsonRoot $headerRelative) -Raw -Encoding UTF8
foreach ($part in @('MAJOR', 'MINOR', 'PATCH')) {
    $changedHeader = $header -replace ('(?m)^(#define NLOHMANN_JSON_VERSION_' + $part + '\s+)\d+'), '${1}99'
    if ($changedHeader -eq $header) { throw "Test did not change $part" }
    [IO.File]::WriteAllText((Join-Path $jsonRoot $headerRelative), $changedHeader, (New-Object Text.UTF8Encoding($false)))
    $failure = Invoke-JsonCollector
    if ($failure.Exception.Message -ne 'nlohmann/json version changed or is unrecognised; review and update the packaged MIT licence.') {
        throw "Changed JSON $part version was not rejected: $failure"
    }
    Write-Host "PASS nlohmann/json $part version drift rejected"
}

foreach ($script in @('Add-PackageLicences.ps1', 'Assert-SourceBundle.ps1', 'New-SourceBundle.ps1', 'New-PortableZip.ps1')) {
    $tokens = $null; $errors = $null
    [Management.Automation.Language.Parser]::ParseFile((Join-Path $SourceRoot "packaging/$script"), [ref]$tokens, [ref]$errors) | Out-Null
    if ($errors.Count) { throw "Invalid PowerShell: $script" }
    Write-Host "PASS parser: $script"
}
