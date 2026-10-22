<#
.SYNOPSIS
  Clean-machine check of the AI Orchestrator portable package: first start, database creation, second start, optional upgrade.

.DESCRIPTION
  Run it INSIDE the unzipped package folder (or pass -PackageDir) on a CLEAN Windows machine or in Windows Sandbox: a machine with no
  Qt, no Visual Studio and no AI Orchestrator data. It starts the real application, so it WRITES the user's profile database; it
  therefore refuses to run when <Roaming AppData>\AIOrchestrator\AIOrchestrator\orchestrator.sqlite already exists (unless -ExistingDb
  places one on purpose, see below). The Roaming AppData folder comes from the system (SHGetKnownFolderPath), never from the APPDATA
  environment variable, which the application ignores; if the system gives no answer the script stops. Never run it on a profile that
  holds real data.

  What it proves, step by step (every line of the report is PASS / FAIL / INFO):
    A  the machine: Windows version, whether Qt / Visual Studio / a system Visual C++ runtime are present (INFO - tells you how clean it is)
    B  the package: executable, version resource, BUILD-INFO, no data/secret file types inside
    C  first start: the window appears and responds; EVERY module the process loaded comes from the package folder or from Windows
       (a module from anywhere else - a Qt or Visual Studio folder, the PATH - is a FAIL); the C++ runtime DLLs are the package's own
    D  the database file was created at the expected place with the expected schema version after a normal close
    E  second start: same database file (not recreated), window appears again, normal close
    F  with -Interactive: you create a project in the window (see CLEAN-MACHINE-CHECKLIST.md); the script then proves the project name
       is stored in the database file and asks you to confirm it is listed after the restart
  Upgrade check: put a COPY of an older database (for example one of schema version 66) next to this script and pass it as
  -ExistingDb; the script installs it into the clean profile before the first start and then expects the schema version of
  -ExpectedSchema (default 70) after the first normal close.

  The report is written to package-check-report.txt (next to the package) and echoed. Exit code 0 only when there is no FAIL.
#>
param(
    [string]$PackageDir = '',
    [string]$ExeName = 'appUI_AI_Orchestrator.exe',
    [int]$ExpectedSchema = 70,
    [int]$StartupTimeoutSeconds = 40,
    [string]$ExistingDb = '',
    [switch]$Interactive,
    [string]$ProjectName = 'RC check project',
    [switch]$SkipLaunch
)
$ErrorActionPreference = 'Stop'
if ($PackageDir -eq '') {
    $PackageDir = if (Test-Path (Join-Path $PSScriptRoot $ExeName)) { $PSScriptRoot } else { (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
}
$PackageDir = (Resolve-Path $PackageDir).Path
$exe = Join-Path $PackageDir $ExeName
$report = New-Object System.Collections.Generic.List[string]
$failures = 0
function Log([string]$kind, [string]$text) {
    $line = '{0,-5} {1}' -f $kind, $text
    $script:report.Add($line); Write-Host $line
    if ($kind -eq 'FAIL') { $script:failures++ }
}

# ---------- helpers (also exercised by packaging\verify\Test-Helpers.ps1 on the build machine) ----------
function Get-RoamingAppData {
    # The profile's real Roaming AppData folder from the system (SHGetKnownFolderPath, FOLDERID_RoamingAppData) - the call Qt's
    # QStandardPaths makes. The APPDATA environment variable is NOT consulted: the application ignores it, so it must not decide
    # where this script looks for (and protects) the database. Returns $null when the system gives no usable answer.
    if (-not ('AioKnownFolder' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AioKnownFolder
{
    [DllImport("shell32.dll")]
    private static extern int SHGetKnownFolderPath([MarshalAs(UnmanagedType.LPStruct)] Guid rfid, uint dwFlags, IntPtr hToken, out IntPtr ppszPath);
    public static string RoamingAppData()
    {
        IntPtr pointer;
        if (SHGetKnownFolderPath(new Guid("3EB685DB-65F9-4CF6-A03A-E3EF65729F3D"), 0, IntPtr.Zero, out pointer) != 0) return null;
        try { return Marshal.PtrToStringUni(pointer); } finally { Marshal.FreeCoTaskMem(pointer); }
    }
}
'@
    }
    $path = [AioKnownFolder]::RoamingAppData()
    if ([string]::IsNullOrWhiteSpace($path) -or -not [IO.Path]::IsPathRooted($path)) { return $null }
    return $path
}
function Get-SqliteUserVersion([string]$path) {
    $stream = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        $bytes = New-Object byte[] 100
        if ($stream.Read($bytes, 0, 100) -lt 100) { return $null }
    } finally { $stream.Close() }
    if ([Text.Encoding]::ASCII.GetString($bytes, 0, 15) -ne 'SQLite format 3') { return $null }
    return ([int]$bytes[60] -shl 24) -bor ([int]$bytes[61] -shl 16) -bor ([int]$bytes[62] -shl 8) -bor [int]$bytes[63]
}
function Test-FileContainsText([string]$path, [string]$text) {
    # UTF-8 bytes of $text inside the (possibly large) file, read through a shared handle in chunks.
    $needle = [Text.Encoding]::UTF8.GetBytes($text)
    $stream = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        $buffer = New-Object byte[] (4MB + $needle.Length)
        $carry = 0
        while (($read = $stream.Read($buffer, $carry, 4MB)) -gt 0) {
            $total = $carry + $read
            $i = 0
            while ($i -le $total - $needle.Length) {
                $i = [Array]::IndexOf($buffer, $needle[0], $i, $total - $needle.Length + 1 - $i)
                if ($i -lt 0) { break }
                $match = $true
                for ($j = 1; $j -lt $needle.Length; $j++) { if ($buffer[$i + $j] -ne $needle[$j]) { $match = $false; break } }
                if ($match) { return $true }
                $i++
            }
            $carry = [Math]::Min($needle.Length - 1, $total)
            [Array]::Copy($buffer, $total - $carry, $buffer, 0, $carry)
        }
    } finally { $stream.Close() }
    return $false
}
function Get-ModuleOrigins([System.Diagnostics.Process]$process, [string]$packageDir) {
    # Every loaded module classified: Package / Windows / Other.
    $windows = $env:windir.TrimEnd('\') + '\'
    $package = $packageDir.TrimEnd('\') + '\'
    foreach ($module in $process.Modules) {
        $path = $module.FileName
        $origin = if ($path.StartsWith($package, 'OrdinalIgnoreCase')) { 'Package' } elseif ($path.StartsWith($windows, 'OrdinalIgnoreCase')) { 'Windows' } else { 'Other' }
        [pscustomobject]@{ Name = $module.ModuleName; Path = $path; Origin = $origin }
    }
}
function Wait-MainWindow([System.Diagnostics.Process]$process, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) { return $false }
        if ($process.MainWindowHandle -ne 0 -and $process.Responding) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
function Stop-Normally([System.Diagnostics.Process]$process) {
    [void]$process.CloseMainWindow()
    if ($process.WaitForExit(30000)) { return $true }
    $process.Kill(); return $false
}
function Start-App {
    $out = Join-Path ([IO.Path]::GetTempPath()) 'aio_check_stdout.txt'
    $err = Join-Path ([IO.Path]::GetTempPath()) 'aio_check_stderr.txt'
    return @{ Process = (Start-Process -FilePath $exe -WorkingDirectory $PackageDir -RedirectStandardOutput $out -RedirectStandardError $err -PassThru); Err = $err }
}

if ($MyInvocation.InvocationName -eq '.') { return }   # dot-sourced: helpers only

# Where the application keeps its data, from the system and not from %APPDATA% (see Get-RoamingAppData). Without a trustworthy answer the
# script cannot protect an existing database, so it stops before it starts anything.
$roaming = Get-RoamingAppData
if (-not $roaming) { Log 'FAIL' 'STOPPED: the system did not give the Roaming AppData folder, so the profile database cannot be located or protected; nothing was started.'; $report | Set-Content (Join-Path ([IO.Path]::GetTempPath()) 'package-check-report.txt'); exit 1 }
$dataDir = Join-Path $roaming 'AIOrchestrator\AIOrchestrator'
$dbFile = Join-Path $dataDir 'orchestrator.sqlite'

# ---------- A. the machine ----------
Log 'INFO' "Check started $(Get-Date -Format s); package $PackageDir"
Log 'INFO' "profile data folder (from the system, not from APPDATA): $dataDir"
if ($env:APPDATA -and ($env:APPDATA.TrimEnd('\') -ine $roaming.TrimEnd('\'))) { Log 'INFO' "note: the APPDATA variable ($env:APPDATA) differs from the real Roaming folder; the application ignores it and so does this script" }
Log 'INFO' "Windows: $((Get-CimInstance Win32_OperatingSystem).Caption) build $([Environment]::OSVersion.Version)"
$qtHints = @($env:QTDIR, $env:Qt6_DIR, $env:QT_ROOT_DIR) | Where-Object { $_ }
$onPath = @('qmake.exe', 'windeployqt.exe', 'cl.exe') | Where-Object { Get-Command $_ -ErrorAction SilentlyContinue }
$vsInstalled = (Test-Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio") -or (Test-Path "$env:ProgramFiles\Microsoft Visual Studio")
$qtFolders = @('C:\Qt', "$env:USERPROFILE\Qt") | Where-Object { Test-Path $_ }
Log 'INFO' ("Qt/Visual Studio present on this machine: Qt env vars={0}; on PATH={1}; VS folder={2}; Qt folders={3}  (all should be empty/False for a CLEAN machine)" -f ($qtHints -join ';'), ($onPath -join ','), $vsInstalled, ($qtFolders -join ';'))
$systemCrt = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') | Where-Object { Test-Path (Join-Path $env:windir "System32\$_") }
Log 'INFO' "System32 already has: $(if ($systemCrt) { $systemCrt -join ', ' } else { 'none of vcruntime140/vcruntime140_1/msvcp140 (the package must bring them)' })"

# ---------- B. the package ----------
if (Test-Path $exe) { Log 'PASS' "found $ExeName" } else { Log 'FAIL' "missing $exe"; $report | Set-Content (Join-Path $PackageDir '..\package-check-report.txt'); exit 1 }
$versionInfo = (Get-Item $exe).VersionInfo
Log 'INFO' "version resource: FileVersion=$($versionInfo.FileVersion) ProductVersion=$($versionInfo.ProductVersion) PreRelease=$($versionInfo.IsPreRelease)"
$expectedVersion = $versionInfo.ProductVersion
foreach ($needed in 'README.txt', 'BUILD-INFO\BUILD-INFO.txt') {
    if (Test-Path (Join-Path $PackageDir $needed)) { Log 'PASS' "present: $needed" } else { Log 'FAIL' "missing: $needed" }
}
$bad = @(Get-ChildItem $PackageDir -Recurse -Force -File | Where-Object { $_.Name -match '\.(sqlite|db|wal|shm|pdb|ilk|pfx|pem|key|env)(-wal|-shm)?$' })
if ($bad.Count -eq 0) { Log 'PASS' 'no database/secret/debug file types in the package' } else { Log 'FAIL' "unexpected files: $(($bad | ForEach-Object Name) -join ', ')" }
$crt = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') | Where-Object { -not (Test-Path (Join-Path $PackageDir $_)) }
if ($crt.Count -eq 0) { Log 'PASS' 'the C++ runtime DLLs are in the package folder' } else { Log 'FAIL' "runtime DLLs missing from the package: $($crt -join ', ')" }

if ($SkipLaunch) { Log 'INFO' 'launch steps skipped (-SkipLaunch)' }
else {
    # ---------- safety: a clean profile only ----------
    if (Test-Path $dbFile) { Log 'FAIL' "REFUSING TO START: $dbFile already exists. This check must run in a clean profile (Windows Sandbox / a new machine)."; $report | Set-Content (Join-Path $PackageDir '..\package-check-report.txt'); exit 1 }
    if ($ExistingDb -ne '') {
        if (-not (Test-Path $ExistingDb)) { Log 'FAIL' "-ExistingDb not found: $ExistingDb"; exit 1 }
        New-Item -ItemType Directory -Force $dataDir | Out-Null
        Copy-Item $ExistingDb $dbFile
        $before = Get-SqliteUserVersion $dbFile
        Log 'INFO' "upgrade check: installed a copy of $ExistingDb as the profile database, schema version $before"
    }

    # ---------- C. first start ----------
    $first = Start-App
    $p = $first.Process
    if (Wait-MainWindow $p $StartupTimeoutSeconds) { Log 'PASS' "first start: window '$($p.MainWindowTitle)' is up and responds" } else { Log 'FAIL' "first start: no responding window within $StartupTimeoutSeconds s (exited=$($p.HasExited))" }
    if (-not $p.HasExited) {
        Start-Sleep -Seconds 5
        $modules = @(Get-ModuleOrigins $p $PackageDir)
        $other = @($modules | Where-Object { $_.Origin -eq 'Other' })
        Log 'INFO' "loaded modules: $($modules.Count) (package $(@($modules | Where-Object Origin -eq 'Package').Count), Windows $(@($modules | Where-Object Origin -eq 'Windows').Count), other $($other.Count))"
        if ($other.Count -eq 0) { Log 'PASS' 'every loaded module comes from the package folder or from Windows' } else { $other | ForEach-Object { Log 'FAIL' "module from outside the package and Windows: $($_.Path)" } }
        foreach ($name in 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') {
            $m = $modules | Where-Object { $_.Name -ieq $name } | Select-Object -First 1
            if ($m -and $m.Origin -eq 'Package') { Log 'PASS' "$name is the package's own copy" } elseif ($m) { Log 'FAIL' "$name was loaded from $($m.Path), not from the package" } else { Log 'INFO' "$name not loaded" }
        }
        $versionLine = $modules | Where-Object { $_.Name -ieq 'Qt6Core.dll' } | Select-Object -First 1
        if ($versionLine) { Log 'INFO' "Qt6Core: $($versionLine.Path) $((Get-Item $versionLine.Path).VersionInfo.FileVersion)" }
        if ($Interactive) {
            Log 'INFO' "INTERACTIVE: in the application window create a project named exactly '$ProjectName' (see CLEAN-MACHINE-CHECKLIST.md, steps 2-4), and look at Settings > Diagnostics for the version ($expectedVersion)."
            [void](Read-Host 'When the project is created and you have read the version, press Enter here')
        }
        if (Stop-Normally $p) { Log 'PASS' 'first start: closed normally' } else { Log 'FAIL' 'first start: did not close within 30 s (killed)' }
    }
    $stderr = if (Test-Path $first.Err) { (Get-Item $first.Err).Length } else { 0 }
    if ($stderr -eq 0) { Log 'PASS' 'first start: nothing written to stderr' } else { Log 'FAIL' "first start: stderr has $stderr bytes ($($first.Err))" }

    # ---------- D. database ----------
    if (Test-Path $dbFile) {
        Log 'PASS' "database exists: $dbFile ($([Math]::Round((Get-Item $dbFile).Length / 1KB)) KB)"
        $schema = Get-SqliteUserVersion $dbFile
        if ($schema -eq $ExpectedSchema) { Log 'PASS' "schema version $schema" } else { Log 'FAIL' "schema version is '$schema', expected $ExpectedSchema" }
        $created = (Get-Item $dbFile).CreationTimeUtc
    } else { Log 'FAIL' "no database at $dbFile after the first start"; $created = $null }
    if ($Interactive -and (Test-Path $dbFile)) {
        if (Test-FileContainsText $dbFile $ProjectName) { Log 'PASS' "the project name '$ProjectName' is stored in the database file" } else { Log 'FAIL' "the project name '$ProjectName' is NOT in the database file after a normal close" }
    }

    # ---------- E. second start ----------
    $second = Start-App
    $p2 = $second.Process
    if (Wait-MainWindow $p2 $StartupTimeoutSeconds) { Log 'PASS' 'second start: window is up and responds' } else { Log 'FAIL' "second start: no responding window within $StartupTimeoutSeconds s" }
    if (-not $p2.HasExited) {
        if ($Interactive) {
            $answer = Read-Host "Is the project '$ProjectName' listed in the Projects page after the restart? (y/n)"
            if ($answer -match '^[yY]') { Log 'PASS' "owner confirms: '$ProjectName' is listed after the restart" } else { Log 'FAIL' "owner reports: '$ProjectName' is NOT listed after the restart" }
        }
        if (Stop-Normally $p2) { Log 'PASS' 'second start: closed normally' } else { Log 'FAIL' 'second start: did not close within 30 s (killed)' }
    }
    if ($created -and (Test-Path $dbFile)) {
        if ((Get-Item $dbFile).CreationTimeUtc -eq $created) { Log 'PASS' 'the database file was reused, not recreated' } else { Log 'FAIL' 'the database file was recreated by the second start' }
        $schema2 = Get-SqliteUserVersion $dbFile
        if ($schema2 -eq $ExpectedSchema) { Log 'PASS' "schema version still $schema2" } else { Log 'FAIL' "schema version after the second start is '$schema2'" }
        if ($Interactive -and (Test-FileContainsText $dbFile $ProjectName)) { Log 'PASS' 'the project is still in the database file after the second close' }
    }
}

$verdict = if ($failures -eq 0) { 'RESULT: no FAIL' } else { "RESULT: $failures FAIL" }
Log 'INFO' $verdict
$reportPath = Join-Path (Split-Path $PackageDir -Parent) 'package-check-report.txt'
$report | Set-Content $reportPath -Encoding UTF8
Write-Host "Report: $reportPath"
exit $(if ($failures -eq 0) { 0 } else { 1 })
