<#
.SYNOPSIS
  Collects hardware and system diagnostics that help explain unexplained crashes of a recompiled game.

.DESCRIPTION
  Read-only. Writes a folder (and a .zip of it) that the player can attach to a bug report.
  Nothing is sent anywhere: the script makes no network access. It changes no system setting.
  What is read is listed in tools/diag/README.md and in the README.txt written into the output folder.

  Generic: the game-specific values (user folder identity, executable name) come from the
  [diagnostics] section of games/<game>/game.toml, or from the parameters below.

.PARAMETER Game        Folder name under games/ (default: the only game present).
.PARAMETER OutDir      Parent folder of the output (default: %LOCALAPPDATA%\<user_org>\<user_app>\diagnostics, a
                       folder that OneDrive typically does not synchronise [hypothesis]; the Desktop often is). A
                       folder that does not exist is created; if that fails the tool stops (exit code 2).
.PARAMETER Days        How far back to read hardware error events (default 30).
.PARAMETER MaxCrashes  How many of the most recent crash reports to copy (default 5).
.PARAMETER CrashDir    Extra folders to search for crash reports (crash-*.txt / crash-*.dmp).
.PARAMETER IncludeGuestRam  Also copy the *-guest-ram.bin dumps (about 25 MiB each).
.PARAMETER NoZip       Do not create the .zip.
.PARAMETER NoPause     Do not wait for a key at the end (for scripts).
.PARAMETER CheckOnly   Collect nothing: run the anonymization check on the files of an existing folder (recursively)
                       and exit with code 0 if no name or SID of this account and computer is found, 1 otherwise.
.NOTES
  Exit code: 0 done (anonymization check OK), 1 the check found a name or SID left in the output or could not read a
  file (no .zip is made), 2 the folder given to -OutDir could not be created, or the one given to -CheckOnly is missing.
#>
param(
    [string]$Game = "",
    [string]$OutDir = "",
    [int]$Days = 30,
    [int]$MaxCrashes = 5,
    [string[]]$CrashDir = @(),
    [switch]$IncludeGuestRam,
    [switch]$NoZip,
    [switch]$NoPause,
    [string]$CheckOnly = ""
)

$ErrorActionPreference = "Continue"
Set-StrictMode -Version 2

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)

# --------------------------------------------------------------------------------------------------------------------
# Game description (games/<game>/game.toml, section [diagnostics]): a minimal reader for "key = \"value\"" lines.
# --------------------------------------------------------------------------------------------------------------------
function Read-TomlSection([string]$Path, [string]$Section) {
    $values = @{}
    if (-not (Test-Path -LiteralPath $Path)) { return $values }
    $inSection = $false
    foreach ($line in Get-Content -LiteralPath $Path) {
        $t = $line.Trim()
        if ($t -match '^\[(.+)\]$') { $inSection = ($Matches[1].Trim() -eq $Section); continue }
        if ($inSection -and $t -match '^([A-Za-z0-9_]+)\s*=\s*"([^"]*)"') { $values[$Matches[1]] = $Matches[2] }
    }
    return $values
}

$gamesDir = Join-Path $RepoRoot "games"
if (-not $Game) {
    $candidates = @()
    if (Test-Path -LiteralPath $gamesDir) {
        $candidates = @(Get-ChildItem -LiteralPath $gamesDir -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "game.toml") })
    }
    if ($candidates.Count -eq 1) { $Game = $candidates[0].Name }
}
$diag = @{}
if ($Game) { $diag = Read-TomlSection (Join-Path (Join-Path $gamesDir $Game) "game.toml") "diagnostics" }
$label = if ($Game) { $Game } else { "game" }
$exeName = if ($diag.ContainsKey("exe_name")) { $diag["exe_name"] } else { "" }

# Crash report folders: <APPDATA>\<user_org>\<user_app>\crashes (where the runtime's crash handler writes when started
# normally), plus the folders given with -CrashDir.
$crashDirs = @()
if ($diag.ContainsKey("user_org") -and $diag.ContainsKey("user_app") -and $env:APPDATA) {
    $crashDirs += (Join-Path (Join-Path (Join-Path $env:APPDATA $diag["user_org"]) $diag["user_app"]) "crashes")
}
$crashDirs += $CrashDir

# --------------------------------------------------------------------------------------------------------------------
# Anonymization. Secrets: profile folder name, USERNAME, computer name, USERDOMAIN / USERDNSDOMAIN, the full name of a
# local account (Win32_UserAccount), the 8.3 short form of the profile folder if TEMP / TMP shows one (JEREMI~1 style),
# account SIDs (S-1-5-21-..., string forms; binary form of the current user's SID; the 12 bytes of its three
# sub-authorities, shared by every other account and group of the machine, which differ only by their last number).
# Text files: names replaced by [user] / [computer] / [domain] / [name], SIDs by S-1-5-21-[removed].
# Binary files (minidumps, guest RAM): same-length replacement by 'x' (or 'x' 0x00 pairs), so that their structure,
# offsets and sizes stay valid. To avoid altering code bytes and ordinary values (an account named "user" must not turn
# user32.dll into xxxx32.dll), a name is replaced in binary files only when it stands alone (no ASCII letter or digit
# just before or after it), and, if it is shorter than 4 characters, only in a context: after \Users\ or /Users/
# (UTF-16 and 8-bit), after "=" (environment block) or "\\" (network name) in UTF-16. A name stuck to a letter or a
# digit is neither masked nor detected (text files included).
# --------------------------------------------------------------------------------------------------------------------
$RxCI = [System.Text.RegularExpressions.RegexOptions]::IgnoreCase -bor [System.Text.RegularExpressions.RegexOptions]::CultureInvariant
$RxNone = [System.Text.RegularExpressions.RegexOptions]::None
$latin1 = [System.Text.Encoding]::GetEncoding(28591)   # one char per byte: exact round trip of any byte sequence
$utf16 = [System.Text.Encoding]::Unicode
$utf8 = New-Object System.Text.UTF8Encoding($false)
$ansi = [System.Text.Encoding]::Default                # the ANSI code page of this Windows
$MaskTokens = @("[user]", "[computer]", "[domain]", "[name]", "[removed]")

$secrets = New-Object System.Collections.Generic.List[object]
function Add-Secret([string]$Value, [string]$Label) {
    if (-not $Value) { return }
    $v = $Value.Trim()
    if (-not $v) { return }
    foreach ($s in $secrets) { if ([string]::Equals($s.Value, $v, [StringComparison]::OrdinalIgnoreCase)) { return } }
    $secrets.Add([pscustomobject]@{ Value = $v; Label = $Label })
}
if ($env:USERPROFILE) { Add-Secret (Split-Path -Leaf $env:USERPROFILE) "user" }
Add-Secret $env:USERNAME "user"
# 8.3 short form of a long profile folder name (JEREMI~1 style), as it can appear in TEMP / TMP. [hypothesis]: not
# verified on an account with a long name; the segment after \Users\ is taken as a secret when it contains "~".
foreach ($tp in @($env:TEMP, $env:TMP)) {
    if ($tp -and $tp -match '(?i)\\Users\\([^\\]+)') { if ($Matches[1].Contains("~")) { Add-Secret $Matches[1] "user" } }
}
Add-Secret $env:COMPUTERNAME "computer"
Add-Secret $env:USERDOMAIN "domain"
Add-Secret $env:USERDNSDOMAIN "domain"
if ($env:USERNAME) {
    # Full name of a local account (for a Microsoft account, the name shown at sign-in). Domain accounts: not read.
    try {
        $wql = $env:USERNAME.Replace("\", "\\").Replace("'", "\'")
        foreach ($a in @(Get-CimInstance -ClassName Win32_UserAccount -Filter ("LocalAccount=True AND Name='{0}'" -f $wql) -ErrorAction Stop)) {
            if ($a.FullName) {
                Add-Secret $a.FullName "name"
                foreach ($part in ($a.FullName -split '\s+')) { if ($part.Length -ge 3) { Add-Secret $part "name" } }
            }
        }
    } catch { }
}
$secrets = @($secrets | Sort-Object { $_.Value.Length } -Descending)
$userSidBytes = $null
$sidPrefixBytes = $null
try {
    $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User
    $userSidBytes = New-Object byte[] $sid.BinaryLength
    $sid.GetBinaryForm($userSidBytes, 0)
    # Binary SID: revision (1), sub-authority count (1), authority (6), then 32-bit sub-authorities: 21, a, b, c, RID.
    # The 12 bytes a, b, c are the same for every account and group of the machine (or domain): other accounts and
    # groups seen in a dump differ only by the RID. Other SID shapes (for example Azure AD): only the exact SID is hidden.
    if ($userSidBytes.Length -eq 28 -and $userSidBytes[0] -eq 1 -and $userSidBytes[1] -eq 5 -and $userSidBytes[7] -eq 5 -and [BitConverter]::ToUInt32($userSidBytes, 8) -eq 21) {
        $sidPrefixBytes = New-Object byte[] 12
        [Array]::Copy($userSidBytes, 12, $sidPrefixBytes, 0, 12)
    }
} catch { $userSidBytes = $null; $sidPrefixBytes = $null }

function Hide-Text([string]$s) {
    if ($null -eq $s) { return $s }
    foreach ($k in $secrets) {
        if ($k.Value.Length -lt 2) { continue }
        $s = [regex]::Replace($s, '(?<![A-Za-z0-9])' + [regex]::Escape($k.Value) + '(?![A-Za-z0-9])', ('[' + $k.Label + ']'), $RxCI)
    }
    return [regex]::Replace($s, 'S-1-5-21-\d+(?:-\d+){2,3}', 'S-1-5-21-[removed]')
}

# Byte forms of a name (as Latin-1 strings): UTF-16LE, UTF-8 and ANSI, for the name as written, upper and lower case;
# a form that the encoding cannot represent (it would become '?') is skipped.
function Get-Forms([string]$v) {
    $forms = New-Object 'System.Collections.Generic.Dictionary[string,object]'
    foreach ($variant in @($v, $v.ToUpperInvariant(), $v.ToLowerInvariant())) {
        foreach ($e in @(@{ E = $utf16; W = $true }, @{ E = $utf8; W = $false }, @{ E = $ansi; W = $false })) {
            $b = $e.E.GetBytes($variant)
            if ($e.E.GetString($b) -cne $variant) { continue }
            $p = $latin1.GetString($b)
            $key = $p.ToLowerInvariant()
            if (-not $forms.ContainsKey($key)) { $forms[$key] = [pscustomobject]@{ Pat = $p; Wide = $e.W } }
        }
    }
    return @($forms.Values)
}

function New-Rule([string]$Pattern, [bool]$Wide, [string]$Label, $Options) {
    [pscustomobject]@{ Rx = (New-Object System.Text.RegularExpressions.Regex($Pattern, $Options)); Wide = $Wide; Label = $Label }
}

# Byte-level rules, used both to mask binary files and to check every output file. $Binary: rules for minidumps and
# guest RAM (names of 2-3 characters only in context); otherwise rules for text files (every name of 2+ characters).
function Get-ByteRules([bool]$Binary) {
    $rules = New-Object System.Collections.Generic.List[object]
    foreach ($k in $secrets) {
        $n = $k.Value.Length
        if ($n -lt 2) { continue }
        foreach ($f in Get-Forms $k.Value) {
            $e = [regex]::Escape($f.Pat)
            if (-not $Binary -or $n -ge 4) {
                if ($f.Wide) { $rx = '(?<![A-Za-z0-9]\x00)' + $e + '(?![A-Za-z0-9]\x00)' }
                else { $rx = '(?<![A-Za-z0-9])' + $e + '(?![A-Za-z0-9])' }
            } elseif ($f.Wide) {
                $rx = '(?<=(?:[\\/]\x00U\x00s\x00e\x00r\x00s\x00[\\/]\x00|=\x00|\\\x00\\\x00))' + $e + '(?=(?:[\\/"'']\x00|\x00\x00))'
            } else {
                $rx = '(?<=[\\/]Users[\\/])' + $e + '(?=[\\/"''\x00])'
            }
            $rules.Add((New-Rule $rx $f.Wide $k.Label $RxCI))
        }
    }
    $rules.Add((New-Rule '(?<=S-1-5-21-)\d+(?:-\d+){2,3}' $false "SID" $RxNone))
    $rules.Add((New-Rule '(?<=S\x00-\x001\x00-\x005\x00-\x002\x001\x00-\x00)(?:\d\x00)+(?:-\x00(?:\d\x00)+){2,3}' $true "SID" $RxNone))
    if ($userSidBytes) { $rules.Add((New-Rule ([regex]::Escape($latin1.GetString($userSidBytes))) $false "SID" $RxNone)) }
    # The 12-byte prefix (a, b, c) of the machine SID: covers the other accounts and groups (same prefix, other RID).
    if ($sidPrefixBytes) { $rules.Add((New-Rule ([regex]::Escape($latin1.GetString($sidPrefixBytes))) $false "SID" $RxNone)) }
    return $rules
}
$binRules = Get-ByteRules $true
$textRules = Get-ByteRules $false
$evNarrow = [System.Text.RegularExpressions.MatchEvaluator] { param($m) "x" * $m.Length }
$evWide = [System.Text.RegularExpressions.MatchEvaluator] { param($m) ("x" + [char]0) * [int]($m.Length / 2) }

function Test-BinaryFile([string]$Name) { return ($Name -like "*.dmp" -or $Name -like "*.bin") }

function Hide-Binary([string]$Path) {
    $text = $latin1.GetString([System.IO.File]::ReadAllBytes($Path))
    foreach ($r in $binRules) { $text = $r.Rx.Replace($text, $(if ($r.Wide) { $evWide } else { $evNarrow })) }
    [System.IO.File]::WriteAllBytes($Path, $latin1.GetBytes($text))
}

# Anonymization check: the same byte-level rules, run again on the files as written (text files: every name of 2+
# characters in UTF-16, UTF-8 and ANSI form, the mask tokens being ignored). Returns "file (what)" for each hit.
function Find-Leaks($Files, [string]$Root) {
    $hits = @()
    foreach ($f in $Files) {
        $rel = $f.FullName
        if ($Root -and $rel.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase)) { $rel = $rel.Substring($Root.Length).TrimStart('\', '/') }
        if ($f.Name -like "*.zip") { $hits += ("{0} (compressed: not checked)" -f $rel); continue }
        # A file that cannot be read has not been checked: it counts as a possible leak (no OK, no .zip, exit code 1).
        try { $t = $latin1.GetString([System.IO.File]::ReadAllBytes($f.FullName)) }
        catch { $hits += ("{0} (could not be read: not checked)" -f $rel); continue }
        if (Test-BinaryFile $f.Name) { $rules = $binRules }
        else { $rules = $textRules; foreach ($m in $MaskTokens) { $t = $t.Replace($m, " ") } }
        $what = @($rules | Where-Object { $_.Rx.IsMatch($t) } | ForEach-Object { $_.Label } | Sort-Object -Unique)
        if ($what.Count) { $hits += ("{0} ({1})" -f $rel, ($what -join ", ")) }
    }
    return $hits
}

if ($CheckOnly) {
    if (-not (Test-Path -LiteralPath $CheckOnly -PathType Container)) { Write-Host ("Not a folder: {0}" -f $CheckOnly); exit 2 }
    $root = (Resolve-Path -LiteralPath $CheckOnly).Path
    $hits = @(Find-Leaks @(Get-ChildItem -LiteralPath $root -Recurse -File -Force) $root)
    if ($hits.Count) { Write-Host ("WARNING: a name or SID of this account or computer is still present, or a file could not be checked, in: {0}" -f ($hits -join "; ")); exit 1 }
    Write-Host "OK: no name or SID of this account or computer found."
    exit 0
}

# --------------------------------------------------------------------------------------------------------------------
# Output folder. Default: the local application data folder of the game (typically not synchronised by OneDrive,
# unlike the Desktop when OneDrive folder backup is on [hypothesis]). A folder given with -OutDir that does not exist is
# created; if that fails the tool stops with exit code 2 rather than writing somewhere else without saying so.
# --------------------------------------------------------------------------------------------------------------------
$outNote = ""
if ($OutDir) {
    if (-not (Test-Path -LiteralPath $OutDir -PathType Container)) {
        # New-Item -Force can return without error and without creating anything (a file in the path): check the result.
        try { New-Item -ItemType Directory -Path $OutDir -Force -ErrorAction Stop | Out-Null; if (-not (Test-Path -LiteralPath $OutDir -PathType Container)) { throw "not created" } }
        catch { Write-Host ("ERROR: the folder given with -OutDir does not exist and could not be created: {0}" -f $OutDir); exit 2 }
        $outNote = "Note: the folder given with -OutDir did not exist; it was created."
    }
} else {
    if ($env:LOCALAPPDATA) {
        if ($diag.ContainsKey("user_org") -and $diag.ContainsKey("user_app")) {
            $OutDir = Join-Path (Join-Path (Join-Path $env:LOCALAPPDATA $diag["user_org"]) $diag["user_app"]) "diagnostics"
        } else { $OutDir = Join-Path $env:LOCALAPPDATA "game-diagnostics" }
        try { New-Item -ItemType Directory -Path $OutDir -Force -ErrorAction Stop | Out-Null; if (-not (Test-Path -LiteralPath $OutDir -PathType Container)) { throw "not created" } }
        catch { $OutDir = "" }
    }
    if (-not $OutDir) {
        $OutDir = (Get-Location).Path
        $outNote = "Note: the default output folder could not be created; the current folder is used. Use -OutDir to choose another."
    }
}
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$out = Join-Path $OutDir ("{0}-diagnostics-{1}" -f $label, $stamp)
New-Item -ItemType Directory -Path $out -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $out "crashes") -Force | Out-Null
$out = (Resolve-Path -LiteralPath $out).Path

$summary = New-Object System.Collections.Generic.List[string]
function Add-Line([string]$s) { $summary.Add($s) | Out-Null; Write-Host $s }
function Add-Section([string]$s) { Add-Line ""; Add-Line ("== " + $s + " ==") }

function Save-Text([string]$Name, [string]$Content) {
    $p = Join-Path $out $Name
    [System.IO.File]::WriteAllText($p, (Hide-Text $Content), $utf8)
}

function Try-Cim([string]$Class) {
    try { return @(Get-CimInstance -ClassName $Class -ErrorAction Stop) } catch { return @() }
}

Add-Line ("Diagnostics for: {0}" -f $label)
Add-Line ("Collected: {0} (local time), UTC offset {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), ([TimeZoneInfo]::Local.GetUtcOffset((Get-Date))).ToString())
Add-Line "Nothing was sent anywhere. Review the files before sharing them."
if ($outNote) { Add-Line $outNote }
foreach ($od in @($env:OneDrive, $env:OneDriveConsumer, $env:OneDriveCommercial)) {
    if ($od -and $out.StartsWith($od.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Add-Line "Note: the output folder is inside a OneDrive folder, so OneDrive may upload it. Use -OutDir to choose another folder."
        break
    }
}

# --------------------------------------------------------------------------------------------------------------------
# (b) Processor, microcode, BIOS, motherboard model, memory modules
# --------------------------------------------------------------------------------------------------------------------
Add-Section "Processor"
foreach ($c in Try-Cim "Win32_Processor") {
    Add-Line ("Name: {0}" -f $c.Name.Trim())
    Add-Line ("Cores / logical processors: {0} / {1}; max clock {2} MHz; current clock {3} MHz" -f $c.NumberOfCores, $c.NumberOfLogicalProcessors, $c.MaxClockSpeed, $c.CurrentClockSpeed)
}
$cpuKey = "HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0"
try {
    $k = Get-ItemProperty -LiteralPath $cpuKey -ErrorAction Stop
    Add-Line ("Identifier: {0}" -f $k.Identifier)
    foreach ($name in @("Update Revision", "Previous Update Revision")) {
        $prop = $k.PSObject.Properties[$name]
        if ($null -eq $prop) { Add-Line ("{0}: not present" -f $name); continue }
        $raw = [byte[]]$prop.Value
        $hex = ($raw | ForEach-Object { "{0:X2}" -f $_ }) -join " "
        # 8-byte form (older Windows): revision in the high 32 bits. 4-byte form: the whole value.
        $rev = 0
        if ($raw.Length -ge 8) { $rev = [BitConverter]::ToUInt32($raw, 4); if ($rev -eq 0) { $rev = [BitConverter]::ToUInt32($raw, 0) } }
        elseif ($raw.Length -ge 4) { $rev = [BitConverter]::ToUInt32($raw, 0) }
        Add-Line ("{0} (microcode): 0x{1:X} (raw bytes {2})" -f $name, $rev, $hex)
    }
} catch { Add-Line "Processor registry key: not readable" }

Add-Section "BIOS and motherboard"
foreach ($b in Try-Cim "Win32_BIOS") {
    Add-Line ("BIOS: {0}, version {1}, date {2}" -f $b.Manufacturer, $b.SMBIOSBIOSVersion, $b.ReleaseDate)
}
foreach ($b in Try-Cim "Win32_BaseBoard") {
    Add-Line ("Motherboard: {0} {1}" -f $b.Manufacturer, $b.Product)
}

Add-Section "Memory modules (a configured speed above the JEDEC speed of the part number typically indicates an XMP/EXPO profile)"
foreach ($m in Try-Cim "Win32_PhysicalMemory") {
    Add-Line ("{0}: {1} GiB, {2} {3}, speed {4} MT/s, configured {5} MT/s" -f $m.DeviceLocator,[math]::Round($m.Capacity / 1GB, 1), $m.Manufacturer, ("" + $m.PartNumber).Trim(), $m.Speed, $m.ConfiguredClockSpeed)
}

# --------------------------------------------------------------------------------------------------------------------
# (c) Windows, graphics driver, power plan
# --------------------------------------------------------------------------------------------------------------------
Add-Section "Windows"
foreach ($o in Try-Cim "Win32_OperatingSystem") {
    Add-Line ("{0}, version {1}, build {2}, {3}" -f $o.Caption, $o.Version, $o.BuildNumber, $o.OSArchitecture)
    Add-Line ("Last boot: {0}" -f $o.LastBootUpTime)
}
try {
    $nt = Get-ItemProperty -LiteralPath "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion" -ErrorAction Stop
    $dv = if ($nt.PSObject.Properties["DisplayVersion"]) { $nt.DisplayVersion } else { "" }
    $ubr = if ($nt.PSObject.Properties["UBR"]) { $nt.UBR } else { "" }
    Add-Line ("Release {0}, update build revision {1}" -f $dv, $ubr)
} catch { }

Add-Section "Graphics"
# Win32_VideoController: Name, VideoProcessor, AdapterCompatibility (chipset vendor), DriverVersion, DriverDate, and
# the hardware id part of PNPDeviceID (vendor, device, board subsystem, revision; the instance suffix is dropped).
foreach ($g in Try-Cim "Win32_VideoController") {
    $hw = ("" + $g.PNPDeviceID) -replace '^([^\\]+\\[^\\]+).*$', '$1'
    Add-Line ("{0} ({1}, {2}): driver {3} ({4}); hardware id {5}" -f $g.Name, $g.VideoProcessor, $g.AdapterCompatibility, $g.DriverVersion, $g.DriverDate, $hw)
}
# Capture / overlay helper DLLs installed with graphics software (version information only). nvspcap64.dll ("NVIDIA
# Game Proxy") was seen loaded into the game process on a development machine with the NVIDIA App installed.
foreach ($n in @("nvspcap64.dll")) {
    $p = Join-Path (Join-Path $env:SystemRoot "System32") $n
    if (Test-Path -LiteralPath $p) {
        $v = (Get-Item -LiteralPath $p).VersionInfo
        Add-Line ("{0} installed: {1}, {2}, version {3}" -f $n, $v.FileDescription, $v.ProductName, $v.FileVersion)
    } else { Add-Line ("{0}: not installed" -f $n) }
}

Add-Section "Power plan"
try {
    # Output is localized and in the console code page: keep only the GUID and the name in parentheses.
    $pc = (& powercfg.exe /getactivescheme 2>&1 | Out-String)
    if ($pc -match '([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})\s*\((.*)\)') {
        Add-Line ("Active power scheme: {0} ({1})" -f $Matches[1], $Matches[2])
    } else { Add-Line ("Active power scheme: {0}" -f $pc.Trim()) }
} catch { Add-Line "powercfg not available" }

# --------------------------------------------------------------------------------------------------------------------
# (d) Crash reports of the game: the most recent ones (txt + dmp, guest RAM only on request)
# --------------------------------------------------------------------------------------------------------------------
Add-Section "Crash reports"
$crashTimes = @()
$stems = @{}
foreach ($d in $crashDirs) {
    if (-not $d -or -not (Test-Path -LiteralPath $d)) { Add-Line ("Folder not found: {0}" -f (Hide-Text $d)); continue }
    Add-Line ("Searched: {0}" -f (Hide-Text $d))
    foreach ($f in Get-ChildItem -LiteralPath $d -File -Filter "crash-*") {
        if ($f.Name -match '^(crash-\d{8}-\d{6}-\d+)') {
            $stem = $Matches[1]
            if (-not $stems.ContainsKey($stem) -or $stems[$stem].Dir -ne $d) {
                $stems[$stem] = @{ Dir = $d; Time = $f.LastWriteTime }
            }
        }
    }
}
$recent = @($stems.GetEnumerator() | Sort-Object { $_.Value.Time } -Descending | Select-Object -First $MaxCrashes)
if ($recent.Count -eq 0) { Add-Line "No crash report found." }
foreach ($e in $recent) {
    $stem = $e.Key
    $t = $e.Value.Time
    if ($stem -match '^crash-(\d{8})-(\d{6})-') {
        try { $t = [datetime]::ParseExact($Matches[1] + $Matches[2], "yyyyMMddHHmmss", $null) } catch { }
    }
    $crashTimes += $t
    $copied = @()
    foreach ($f in Get-ChildItem -LiteralPath $e.Value.Dir -File -Filter "crash-*") {
        # Exact stem: crash-...-12 must not take the files of crash-...-123.
        if (-not ($f.Name -match '^(crash-\d{8}-\d{6}-\d+)') -or $Matches[1] -ne $stem) { continue }
        if ($f.Name -like "*-guest-ram.bin" -and -not $IncludeGuestRam) { continue }
        $dst = Join-Path (Join-Path $out "crashes") $f.Name
        Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
        if (-not (Test-BinaryFile $f.Name)) {
            # Text: UTF-8 if it decodes as such, the ANSI code page otherwise; written back as UTF-8.
            $raw = [System.IO.File]::ReadAllBytes($dst)
            try { $txt = (New-Object System.Text.UTF8Encoding($false, $true)).GetString($raw) } catch { $txt = $ansi.GetString($raw) }
            if ($txt.Length -gt 0 -and $txt[0] -eq [char]0xFEFF) { $txt = $txt.Substring(1) }
            [System.IO.File]::WriteAllText($dst, (Hide-Text $txt), $utf8)
        } else {
            Hide-Binary $dst
        }
        $copied += $f.Name
    }
    Add-Line ("{0} (local time {1}): {2}" -f $stem, $t.ToString("yyyy-MM-dd HH:mm:ss"), ($copied -join ", "))
}

# --------------------------------------------------------------------------------------------------------------------
# (a) Hardware error events (WHEA): System log, provider Microsoft-Windows-WHEA-Logger, and the Kernel-WHEA logs.
# Get-WinEvent -LogName <log> -FilterXPath <XPath> (Microsoft Learn, Get-WinEvent, PowerShell 5.1; the timediff()
# filter is the one of its example 16). Same query by hand: wevtutil qe System /q:"<XPath>" /rd:true /f:text
# (Microsoft Learn, "wevtutil"). Get-WinEvent is used because its Message text is not mangled by console code pages.
# --------------------------------------------------------------------------------------------------------------------
Add-Section ("Hardware error events (WHEA), last {0} days" -f $Days)
$ms = [int64]$Days * 86400000
$queries = @(
    @{ Log = "System"; File = "whea-system"; Q = "*[System[Provider[@Name='Microsoft-Windows-WHEA-Logger'] and TimeCreated[timediff(@SystemTime) <= $ms]]]" },
    @{ Log = "Microsoft-Windows-Kernel-WHEA/Errors"; File = "whea-kernel-errors"; Q = "*[System[TimeCreated[timediff(@SystemTime) <= $ms]]]" },
    @{ Log = "Microsoft-Windows-Kernel-WHEA/Operational"; File = "whea-kernel-operational"; Q = "*[System[TimeCreated[timediff(@SystemTime) <= $ms]]]" }
)
$wheaTimes = @()   # times of warning / error / critical events only (informational ones are boot-time bookkeeping)
foreach ($q in $queries) {
    $err = $null
    $events = @(Get-WinEvent -LogName $q.Log -FilterXPath $q.Q -ErrorAction SilentlyContinue -ErrorVariable err)
    if ($events.Count -eq 0) {
        $why = if ($err) { ($err | Select-Object -First 1).FullyQualifiedErrorId } else { "" }
        if (-not $why -or $why -like "NoMatchingEventsFound*") { Add-Line ("{0}: 0 event(s)" -f $q.Log) }
        else { Add-Line ("{0}: not readable ({1})" -f $q.Log, $why) }
        continue
    }
    $byLevel = ($events | Group-Object Level | Sort-Object Name | ForEach-Object {
        $n = switch ([int]$_.Name) { 1 { "critical" } 2 { "error" } 3 { "warning" } 4 { "information" } default { "level " + $_.Name } }
        "{0} {1}" -f $_.Count, $n }) -join ", "
    $byId = ($events | Group-Object Id | Sort-Object Count -Descending | ForEach-Object { "id {0} x{1}" -f $_.Name, $_.Count }) -join ", "
    Add-Line ("{0}: {1} event(s): {2} ({3}); newest {4}" -f $q.Log, $events.Count, $byLevel, $byId, $events[0].TimeCreated.ToString("yyyy-MM-dd HH:mm:ss"))
    $txt = New-Object System.Text.StringBuilder
    foreach ($ev in $events) {
        [void]$txt.AppendLine(("[{0}] id {1}, level {2}, provider {3}" -f $ev.TimeCreated.ToString("yyyy-MM-dd HH:mm:ss"), $ev.Id, $ev.Level, $ev.ProviderName))
        [void]$txt.AppendLine(("" + $ev.Message).Trim())
        [void]$txt.AppendLine("")
        if ($ev.Level -ge 1 -and $ev.Level -le 3) { $wheaTimes += $ev.TimeCreated }
    }
    Save-Text ($q.File + ".txt") $txt.ToString()
    Save-Text ($q.File + ".xml") ("<Events>`r`n" + (($events | ForEach-Object { $_.ToXml() }) -join "`r`n") + "`r`n</Events>")
}
# Warning / error WHEA events close to a crash (10 minutes before, 2 minutes after): the most telling evidence of a
# hardware fault. Their absence proves nothing (many processor faults are not reported as WHEA events).
foreach ($ct in $crashTimes) {
    $near = @($wheaTimes | Where-Object { $_ -ge $ct.AddMinutes(-10) -and $_ -le $ct.AddMinutes(2) })
    Add-Line ("Crash at {0}: {1} WHEA warning/error event(s) within -10/+2 minutes" -f $ct.ToString("yyyy-MM-dd HH:mm:ss"), $near.Count)
}

# --------------------------------------------------------------------------------------------------------------------
# (e) Overlay, tuning and security software that may inject into games or change CPU behaviour: process names only.
# The list below is made of commonly seen process names, compiled from general knowledge; the names were not checked
# one by one against each product, so a product may be missed.
# --------------------------------------------------------------------------------------------------------------------
Add-Section "Overlay, tuning and security software running now (process names from a fixed, unverified list)"
$known = [ordered]@{
    "overlay / capture" = @("rtss", "nvidia app", "msiafterburner", "discord", "steam", "gameoverlayui", "overwolf", "nvidia overlay", "nvidia share", "nvcontainer", "gamebar", "gamebarftserver", "xboxgamebar", "obs64", "obs32", "bdcam", "bandicam", "fraps", "medal", "outplayed", "razercortex", "evga precision x1", "precisionx_x64", "epicgameslauncher", "upc", "eadesktop", "playnite.desktopapp", "radeonsoftware", "amdrsserv", "dxtory", "action_x64", "mirillis")
    "CPU / RAM tuning" = @("throttlestop", "xtu3service", "xtuservice", "perftune", "amdryzenmaster", "ryzenmaster", "processlasso", "processgovernor", "hwinfo64", "aida64", "armourycrate.service", "armourycrate.usersessionhelper", "lghub", "lghub_agent", "icue", "signalrgb", "nzxt cam", "msi.centralserver", "msi center", "gigabyte control center", "aorusengine")
    "security / anti-cheat" = @("msmpeng", "avp", "avpui", "avastsvc", "avastui", "avgsvc", "avgui", "bdagent", "bdservicehost", "vsserv", "ekrn", "egui", "nortonsecurity", "norton", "mbamservice", "mbam", "mcshield", "mfemms", "mfevtps", "sophoshealth", "sophosui", "csfalconservice", "sentinelagent", "wrsa", "cmdagent", "coreserviceshell", "fshoster64", "fshoster32", "avkservice", "psanhost", "zonealarm", "vgc", "vgtray", "easyanticheat", "easyanticheat_eos", "beservice", "faceit", "faceitservice")
}
$running = @{}
foreach ($p in Get-Process -ErrorAction SilentlyContinue) { $running[$p.ProcessName.ToLowerInvariant()] = $true }
foreach ($cat in $known.Keys) {
    $hits = @($known[$cat] | Where-Object { $running.ContainsKey($_) })
    Add-Line ("{0}: {1}" -f $cat, $(if ($hits.Count) { $hits -join ", " } else { "none" }))
}
try {
    $av = @(Get-CimInstance -Namespace "root/SecurityCenter2" -ClassName AntiVirusProduct -ErrorAction Stop | ForEach-Object { $_.displayName })
    Add-Line ("Registered antivirus products: {0}" -f $(if ($av.Count) { ($av | Sort-Object -Unique) -join ", " } else { "none" }))
} catch { Add-Line "Registered antivirus products: not readable" }

# If the game is running now: names of its loaded modules that are neither in the Windows folder nor next to the
# game (injected DLLs). Names only, no path.
if ($exeName) {
    $procName = [System.IO.Path]::GetFileNameWithoutExtension($exeName)
    $gp = @(Get-Process -Name $procName -ErrorAction SilentlyContinue)
    if ($gp.Count -eq 0) {
        Add-Line ("{0} is not running: injected modules not listed (start the game, then run this tool again to list them)." -f $exeName)
    } else {
        foreach ($p in $gp) {
            try {
                $gameDir = Split-Path -Parent $p.MainModule.FileName
                $win = $env:SystemRoot
                $foreign = @($p.Modules | Where-Object { $_.FileName -and -not $_.FileName.StartsWith($win, [StringComparison]::OrdinalIgnoreCase) -and -not $_.FileName.StartsWith($gameDir, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { $_.ModuleName } | Sort-Object -Unique)
                Add-Line ("Modules loaded in {0} from outside Windows and the game folder: {1}" -f $exeName, $(if ($foreign.Count) { $foreign -join ", " } else { "none" }))
                # Known overlay / capture hooks wherever they are loaded from (several live in System32), and the
                # graphics driver user-mode modules, with their file versions.
                $hooks = @("nvspcap64.dll", "gameoverlayrenderer64.dll", "discordhook64.dll", "rtsshooks64.dll", "graphics-hook64.dll", "ow-graphics-hook64.dll", "overlay64.dll", "mediaserver64.dll", "reshade64.dll", "bdcamvk64.dll", "medal-hook64.dll")
                $gpuPrefix = '^(nv|amd|ati|aticfx|igd|igx|igc|intelcontrol)'
                $shown = @($p.Modules | Where-Object { $hooks -contains $_.ModuleName.ToLowerInvariant() -or $_.ModuleName.ToLowerInvariant() -match $gpuPrefix } |
                    Sort-Object ModuleName -Unique | ForEach-Object {
                        $flag = if ($hooks -contains $_.ModuleName.ToLowerInvariant()) { " [overlay/capture]" } else { "" }
                        "{0} {1}{2}" -f $_.ModuleName, $_.FileVersionInfo.FileVersion, $flag })
                Add-Line ("Graphics driver and overlay modules in {0}: {1}" -f $exeName, $(if ($shown.Count) { $shown -join "; " } else { "none" }))
            } catch { Add-Line ("Modules of {0}: not readable" -f $exeName) }
        }
    }
}

# --------------------------------------------------------------------------------------------------------------------
# (f) README, summary, anonymization check of every output file, archive
# --------------------------------------------------------------------------------------------------------------------
$readme = @"
What this folder contains
=========================
Created by tools/diag/collect_diagnostics.ps1. The tool only READ the items below, wrote this folder on your
computer, and sent NOTHING anywhere (it has no network access). It changed no setting. If this folder is inside a
OneDrive folder (often the case of the Desktop), OneDrive may upload it: summary.txt says so.

- summary.txt: date, time and UTC offset of the collection (to line up crash times with event times), processor
  name, microcode revision, BIOS version and date, motherboard model, memory modules (size, maker, part number,
  rated and configured speed; no serial number), Windows version, graphics driver version, active power plan,
  names of running overlay / tuning / security programs from a fixed list, names of DLLs loaded into the game if
  it is running, the crash report list, and the result of the anonymization check.
- whea-*.txt / whea-*.xml (only if present): Windows hardware error events (WHEA) of the last $Days days,
  from the System log (provider Microsoft-Windows-WHEA-Logger) and the Microsoft-Windows-Kernel-WHEA logs.
  "Corrected" WHEA errors near a crash time point to an unstable processor, memory or BIOS setting.
- crashes/: the $MaxCrashes most recent crash reports of the game (.txt and .dmp minidumps).

Anonymization: your user name, profile folder name, computer name, domain name and the full name of a local
account are replaced by [user] / [computer] / [domain] / [name] in the text files; account SIDs (S-1-5-21-...)
by S-1-5-21-[removed]. In the minidumps they are overwritten by x characters of the same length, only where
the name stands alone; a name shorter than 4 characters only in a folder path (\Users\...) or, in UTF-16, an
environment variable or a network name (summary.txt says if this applies). Also overwritten in the minidumps: the 12
bytes of the machine part of the account SID, so that the other accounts and groups of this computer are hidden too.
A name stuck to a letter or a digit is neither masked nor detected. A final check searches every output file again
and its result is at the end of summary.txt. Please still look at the files before sharing them, and remove
anything you do not want to share.
"@

Add-Section "Anonymization"
$binCopied = @(Get-ChildItem -LiteralPath (Join-Path $out "crashes") -File | Where-Object { Test-BinaryFile $_.Name }).Count
foreach ($k in $secrets) {
    $n = $k.Value.Length
    if ($n -lt 2) {
        Add-Line ("LIMIT: the {0} name has 1 character and cannot be masked. Look at the files before sharing them." -f $k.Label)
    } elseif ($n -lt 4 -and $binCopied -gt 0) {
        Add-Line ("LIMIT: the {0} name has {1} characters: in the .dmp / .bin files it was masked only in a folder path (\Users\...) and, in UTF-16 text, after = (environment variable) or \\ (network name); elsewhere in them it cannot be told apart from ordinary bytes and was left as is. Do not share the .dmp files if this matters to you." -f $k.Label, $n)
    }
}
Add-Line "LIMIT: a name stuck to a letter or a digit (inside a longer word, or followed by a number) is neither masked nor detected by the check, in any file. Look at the files before sharing them."
if ($userSidBytes -and -not $sidPrefixBytes) { Add-Line "LIMIT: the account SID has an unusual shape: only the SID of this account was masked in the .dmp files, not the other accounts of the computer." }
if (-not ($secrets | Where-Object { $_.Label -eq "name" })) { Add-Line "Full account name: not found (only local accounts are read); it is not masked." }

Save-Text "README.txt" $readme
Save-Text "summary.txt" ($summary -join "`r`n")

# Check every output file as written, summary.txt and README.txt included, then add the verdict to summary.txt and
# check that file once more.
$leaks = @(Find-Leaks @(Get-ChildItem -LiteralPath $out -Recurse -File -Force) $out)
Add-Section "Anonymization check (every output file, after writing)"
if ($leaks.Count) {
    Add-Line ("WARNING: a name or SID of this account or computer is still present, or a file could not be checked, in: {0}" -f ($leaks -join "; "))
    Add-Line "No .zip was made. Remove or edit these files before sharing anything."
} else { Add-Line "OK: no name or SID of this account or computer found in the output files." }
Save-Text "summary.txt" ($summary -join "`r`n")
$again = @(Find-Leaks @(Get-Item -LiteralPath (Join-Path $out "summary.txt")) $out)
if ($again.Count) { Write-Host "WARNING: summary.txt failed the anonymization check after the verdict was added."; $leaks += $again }

$zipPath = ""
if (-not $NoZip -and $leaks.Count -eq 0) {
    $zipPath = $out + ".zip"
    try { Compress-Archive -Path (Join-Path $out "*") -DestinationPath $zipPath -Force -ErrorAction Stop }
    catch { Write-Host ("Could not create the .zip: {0}" -f $_.Exception.Message); $zipPath = "" }
}

Write-Host ""
Write-Host "Done. Nothing was sent anywhere."
Write-Host ("Folder: {0}" -f $out)
if ($zipPath) { Write-Host ("Archive to attach to your report: {0}" -f $zipPath) }
if (-not $NoPause -and [Environment]::UserInteractive -and $Host.Name -eq "ConsoleHost") {
    # Interactive use only: show the result in File Explorer (explorer.exe /select,<file>), since the default folder
    # is under the hidden AppData folder.
    $show = if ($zipPath) { $zipPath } else { $out }
    try { Start-Process -FilePath (Join-Path $env:SystemRoot "explorer.exe") -ArgumentList ('/select,"{0}"' -f $show) } catch { }
    Write-Host "Press Enter to close."
    [void][Console]::ReadLine()
}
if ($leaks.Count) { exit 1 }
exit 0
