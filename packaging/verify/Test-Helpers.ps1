<#
.SYNOPSIS
  Build-machine self-test of the helper functions of Test-PortablePackage.ps1. It starts NO AI Orchestrator process and touches no profile data.
  Usage: .\Test-Helpers.ps1 -OldDb <a copy of a database of schema version N> -ExpectedVersion N
#>
param([Parameter(Mandatory = $true)][string]$OldDb, [Parameter(Mandatory = $true)][int]$ExpectedVersion)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Test-PortablePackage.ps1')   # dot-sourced: defines the helpers only
$failed = 0
function Check([string]$what, [bool]$ok) { if ($ok) { "PASS  $what" } else { "FAIL  $what"; $script:failed++ } }

Check "schema version read from the header ($ExpectedVersion)" ((Get-SqliteUserVersion $OldDb) -eq $ExpectedVersion)
Check 'a non-SQLite file has no schema version' ($null -eq (Get-SqliteUserVersion (Join-Path $PSScriptRoot 'Test-Helpers.ps1')))

$tmp = Join-Path ([IO.Path]::GetTempPath()) 'aio_helper_test.bin'
$filler = New-Object byte[] (9MB)
for ($i = 0; $i -lt 40; $i++) { $filler[$i * 200000] = 82 }     # many 'R' (the first letter of the needle) to exercise the matcher
$needle = [Text.Encoding]::UTF8.GetBytes('RC check project')
[Array]::Copy($needle, 0, $filler, 4MB - 7, $needle.Length)      # straddles the 4 MB chunk border on purpose
[IO.File]::WriteAllBytes($tmp, $filler)
Check 'a text straddling the chunk border is found' (Test-FileContainsText $tmp 'RC check project')
Check 'a text that is not there is not found' (-not (Test-FileContainsText $tmp 'RC check projecT'))
$cyrillic = (0x41F, 0x440, 0x43E, 0x435, 0x43A, 0x442, 0x20, 0x2116, 0x31 | ForEach-Object { [string][char]$_ }) -join ''   # no non-ASCII literal: PS 5.1 reads BOM-less files as ANSI
[IO.File]::WriteAllBytes($tmp, [Text.Encoding]::UTF8.GetBytes("xx $cyrillic yy"))
Check 'a text with non-ASCII letters is found (UTF-8)' (Test-FileContainsText $tmp $cyrillic)
Remove-Item $tmp

$self = [System.Diagnostics.Process]::GetCurrentProcess()
$origins = @(Get-ModuleOrigins $self (Join-Path $env:windir 'System32\WindowsPowerShell'))
Check 'module origins are classified (Windows/Package/Other all possible)' ($origins.Count -gt 20 -and (@($origins | Where-Object Origin -eq 'Windows').Count -gt 0))
Check 'a module outside the "package" folder and Windows is reported as Other when the folder differs' (@(Get-ModuleOrigins $self 'C:\NoSuchPackage' | Where-Object Origin -eq 'Package').Count -eq 0)

$realRoaming = Get-RoamingAppData
Check 'the Roaming AppData folder comes from the system and is a rooted path' ($realRoaming -and [IO.Path]::IsPathRooted($realRoaming))
Check 'it equals the folder .NET reports from the known-folder API' ($realRoaming -ieq [Environment]::GetFolderPath('ApplicationData'))
$savedAppData = $env:APPDATA
try {
    $env:APPDATA = 'C:\FakeProfile\Roaming'
    Check 'a replaced APPDATA variable does NOT change the folder the script protects' ((Get-RoamingAppData) -ieq $realRoaming)
} finally { $env:APPDATA = $savedAppData }

$parseErrors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Test-PortablePackage.ps1'), [ref]$null, [ref]$parseErrors)
Check 'Test-PortablePackage.ps1 parses without errors' ($parseErrors.Count -eq 0)
if ($failed -eq 0) { 'ALL PASS' } else { "$failed FAIL" }
exit $(if ($failed -eq 0) { 0 } else { 1 })
