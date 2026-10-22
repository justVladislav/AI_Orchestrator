<# Collect licence material from the dependencies actually used by a Release tree.
   No download, profile access or mutation of the installed SDKs is performed. #>
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$QtPrefix,
    [Parameter(Mandatory = $true)][string]$Stage,
    [string]$QtSourceRoot = '',
    [string]$QtDeclarativeSourceRoot = ''
)
$ErrorActionPreference = 'Stop'
if (!$QtSourceRoot) { $QtSourceRoot = Join-Path (Split-Path $QtPrefix) 'Src' }
$modules = @('qtbase', 'qtdeclarative', 'qtimageformats', 'qtlottie', 'qtquick3d',
    'qtvirtualkeyboard', 'qtsvg', 'qtshadertools', 'qtquicktimeline', 'qtlanguageserver', 'qttools')
$licences = Join-Path $Stage 'LICENCES'
New-Item -ItemType Directory -Force $licences | Out-Null
foreach ($name in @('LICENSE', 'COPYRIGHT', 'THIRD-PARTY-NOTICES.txt')) {
    Copy-Item -LiteralPath (Join-Path $SourceRoot $name) -Destination $Stage
}
# Header-only dependencies are not visible in the DLL import/deployment inventory.
$jsonHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'First_AI_Agents/First_AI_Agents/includes/nlohmann/json.hpp') -Raw -Encoding UTF8
foreach ($part in @(@('MAJOR', 3), @('MINOR', 12), @('PATCH', 0))) {
    if ($jsonHeader -notmatch ('(?m)^#define NLOHMANN_JSON_VERSION_' + $part[0] + '\s+' + $part[1] + '\b')) {
        throw 'nlohmann/json version changed or is unrecognised; review and update the packaged MIT licence.'
    }
}
$jsonLicence = Join-Path $SourceRoot 'packaging/licences/nlohmann-json-3.12.0-MIT.txt'
if (!(Test-Path -LiteralPath $jsonLicence -PathType Leaf)) { throw 'Missing nlohmann/json MIT licence.' }
Copy-Item -LiteralPath $jsonLicence -Destination (Join-Path $licences 'nlohmann-json.txt')

$version = (Get-Item (Join-Path $Stage 'Qt6Core.dll')).VersionInfo.ProductVersion
$version = ([version]$version).ToString(3)
$notice = New-Object System.Collections.Generic.List[string]
$notice.Add("Qt ${version}: upstream attribution records (a conservative superset of deployed features).")
$notice.Add('Each dependency retains its own terms. See the accompanying full licence texts and source archives.')
foreach ($module in $modules) {
    $root = Join-Path $QtSourceRoot $module
    if ($module -eq 'qtdeclarative' -and $QtDeclarativeSourceRoot) { $root = $QtDeclarativeSourceRoot }
    $sbom = Join-Path $QtPrefix "sbom/$module-$version.spdx.json"
    if (!(Test-Path $sbom)) { throw "Missing Qt SPDX inventory: $sbom" }
    $inventory = Get-Content $sbom -Raw -Encoding UTF8 | ConvertFrom-Json
    $upstream = $inventory.packages | Where-Object { $_.name -eq $module } | Select-Object -First 1
    $tag = if (Test-Path (Join-Path $root '.git')) { (git -C $root rev-parse HEAD).Trim() }
        elseif (Test-Path (Join-Path $root '.tag')) { (Get-Content (Join-Path $root '.tag') -Raw).Trim() }
        else { throw "Missing source revision for $module" }
    if (!$upstream -or ($upstream.downloadLocation -match '@([0-9a-f]{40})$' -and $Matches[1] -ne $tag) -or
        ($upstream.downloadLocation -notmatch '@[0-9a-f]{40}$' -and $upstream.versionInfo -ne $version)) {
        throw "Qt source/binary revision mismatch for $module"
    }
    $dest = Join-Path $licences "Qt/$module"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item -LiteralPath $sbom -Destination $dest
    Set-Content (Join-Path $dest 'SOURCE-REVISION.txt') "$module $tag (upstream binary inventory: $($upstream.downloadLocation); $($upstream.versionInfo))" -Encoding UTF8
    $texts = Join-Path $dest 'LICENSES'
    New-Item -ItemType Directory -Force $texts | Out-Null
    Get-ChildItem (Join-Path $root 'LICENSES') -File | Copy-Item -Destination $texts -Force
    foreach ($meta in Get-ChildItem (Join-Path $root 'src') -Filter qt_attribution.json -Recurse -File | Sort-Object FullName) {
        $entries = Get-Content $meta.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($entry in $entries) {
            $notice.Add("`n=== $module / $($entry.Id): $($entry.Name) ===")
            $notice.Add("Version: $($entry.Version); licence: $($entry.LicenseId); homepage: $($entry.Homepage)")
            $notice.Add(($entry.Copyright -join "`n"))
            foreach ($file in $entry.LicenseFile) {
                if (!$file) { continue }
                $candidates = @((Join-Path $meta.DirectoryName $file))
                if ($entry.Path) { $candidates += Join-Path (Join-Path $meta.DirectoryName $entry.Path) $file }
                $found = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
                if (!$found) { throw "Missing attribution licence: $module/$($entry.Id)/$file" }
                $notice.Add((Get-Content -LiteralPath $found -Raw -Encoding UTF8))
            }
        }
    }
    foreach ($config in @("config_$module.opt", "config_$module.summary")) {
        Copy-Item -LiteralPath (Join-Path $QtPrefix $config) -Destination $dest
    }
}
[IO.File]::WriteAllLines((Join-Path $licences 'Qt/THIRD-PARTY.txt'), $notice, (New-Object Text.UTF8Encoding($false)))
foreach ($dependency in @('curl', 'sqlite3', 'zlib')) {
    Copy-Item -LiteralPath (Join-Path $BuildDir "vcpkg_installed/x64-windows/share/$dependency/copyright") `
        -Destination (Join-Path $licences "$dependency.txt")
}
Copy-Item -LiteralPath (Join-Path $SourceRoot 'packaging/licences/Mesa-LLVM.txt') -Destination $licences
Copy-Item -LiteralPath (Join-Path $SourceRoot 'packaging/licences/Mesa-11.2.2-COPYING.txt') -Destination $licences
Copy-Item -LiteralPath (Join-Path $SourceRoot 'packaging/licences/Microsoft-runtime.txt') -Destination $licences
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Licenses/10.0.26100.0'
foreach ($name in @('sdk_license.rtf', 'sdk_third_party_notices.rtf')) {
    Copy-Item -LiteralPath (Join-Path $sdk $name) -Destination $licences
}
Write-Host "[package] Full application/dependency licences and Qt revision checks collected."
