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
.PARAMETER OutDir      Parent folder of the output (default: the Desktop).
.PARAMETER Days        How far back to read hardware error events (default 30).
.PARAMETER MaxCrashes  How many of the most recent crash reports to copy (default 5).
.PARAMETER CrashDir    Extra folders to search for crash reports (crash-*.txt / crash-*.dmp).
.PARAMETER IncludeGuestRam  Also copy the *-guest-ram.bin dumps (about 25 MiB each).
.PARAMETER NoZip       Do not create the .zip.
.PARAMETER NoPause     Do not wait for a key at the end (for scripts).
#>
param(
    [string]$Game = "",
    [string]$OutDir = "",
    [int]$Days = 30,
    [int]$MaxCrashes = 5,
    [string[]]$CrashDir = @(),
    [switch]$IncludeGuestRam,
    [switch]$NoZip,
    [switch]$NoPause
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
# Output folder
# --------------------------------------------------------------------------------------------------------------------
if (-not $OutDir) { $OutDir = [Environment]::GetFolderPath("Desktop") }
if (-not $OutDir -or -not (Test-Path -LiteralPath $OutDir)) { $OutDir = (Get-Location).Path }
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$out = Join-Path $OutDir ("{0}-diagnostics-{1}" -f $label, $stamp)
New-Item -ItemType Directory -Path $out -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $out "crashes") -Force | Out-Null

$summary = New-Object System.Collections.Generic.List[string]
function Add-Line([string]$s) { $summary.Add($s) | Out-Null; Write-Host $s }
function Add-Section([string]$s) { Add-Line ""; Add-Line ("== " + $s + " ==") }

# --------------------------------------------------------------------------------------------------------------------
# Anonymization: user name, profile path, computer name, user SIDs. Same length replacement, so that binary files
# (minidumps) keep their structure.
# --------------------------------------------------------------------------------------------------------------------
$secretNames = @()
if ($env:USERPROFILE) { $secretNames += (Split-Path -Leaf $env:USERPROFILE) }
if ($env:USERNAME) { $secretNames += $env:USERNAME }
$secretNames = @($secretNames | Where-Object { $_ -and $_.Length -ge 2 } | Sort-Object -Unique)
$computerName = $env:COMPUTERNAME

function Hide-Text([string]$s) {
    if ($null -eq $s) { return $s }
    foreach ($n in $secretNames) {
        $e = [regex]::Escape($n)
        $s = [regex]::Replace($s, "(?i)([\\/]Users[\\/])$e(?=[\\/\s""']|$)", '$1[user]')
        $s = [regex]::Replace($s, "(?i)(?<![A-Za-z0-9])$e(?![A-Za-z0-9])", '[user]')
    }
    if ($computerName -and $computerName.Length -ge 3) {
        $s = [regex]::Replace($s, "(?i)(?<![A-Za-z0-9])" + [regex]::Escape($computerName) + "(?![A-Za-z0-9])", '[computer]')
    }
    $s = [regex]::Replace($s, 'S-1-5-21-\d+-\d+-\d+-\d+', 'S-1-5-21-<removed>')
    return $s
}

$latin1 = [System.Text.Encoding]::GetEncoding(28591)
function Hide-Binary([string]$Path) {
    # Byte-level, same length: each secret is searched as UTF-16LE, UTF-8 and ANSI bytes, case-insensitively, and
    # replaced by as many 'x' (or 'x' 0x00 pairs). Latin-1 maps every byte to one char, so the round trip is exact.
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = $latin1.GetString($bytes)
    $secrets = @($secretNames)
    if ($computerName -and $computerName.Length -ge 3) { $secrets += $computerName }
    foreach ($n in $secrets) {
        foreach ($enc in @([System.Text.Encoding]::Unicode, [System.Text.Encoding]::UTF8, [System.Text.Encoding]::Default)) {
            foreach ($variant in @($n, $n.ToUpperInvariant(), $n.ToLowerInvariant()) | Sort-Object -Unique) {
                $pat = $latin1.GetString($enc.GetBytes($variant))
                if ($pat.Length -lt 2) { continue }
                $rep = if ($enc -eq [System.Text.Encoding]::Unicode) { ("x" + [char]0) * ($pat.Length / 2) } else { "x" * $pat.Length }
                $text = [regex]::Replace($text, [regex]::Escape($pat), $rep, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
            }
        }
    }
    [System.IO.File]::WriteAllBytes($Path, $latin1.GetBytes($text))
}

function Save-Text([string]$Name, [string]$Content) {
    $p = Join-Path $out $Name
    [System.IO.File]::WriteAllText($p, (Hide-Text $Content), (New-Object System.Text.UTF8Encoding($false)))
}

function Try-Cim([string]$Class) {
    try { return @(Get-CimInstance -ClassName $Class -ErrorAction Stop) } catch { return @() }
}

Add-Line ("Diagnostics for: {0}" -f $label)
Add-Line ("Collected: {0} (local time), UTC offset {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), ([TimeZoneInfo]::Local.GetUtcOffset((Get-Date))).ToString())
Add-Line "Nothing was sent anywhere. Review the files before sharing them."

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

Add-Section "Memory modules (rated speed vs configured speed shows XMP/EXPO profiles)"
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
# Capture / overlay helper DLLs installed with graphics software (version information only). nvspcap64.dll is
# installed by the NVIDIA App / GeForce Experience and is loaded into game processes.
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
    foreach ($f in Get-ChildItem -LiteralPath $e.Value.Dir -File -Filter ($stem + "*")) {
        if ($f.Name -like "*-guest-ram.bin" -and -not $IncludeGuestRam) { continue }
        $dst = Join-Path (Join-Path $out "crashes") $f.Name
        Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
        if ($f.Extension -eq ".txt") {
            $txt = [System.IO.File]::ReadAllText($dst)
            [System.IO.File]::WriteAllText($dst, (Hide-Text $txt), (New-Object System.Text.UTF8Encoding($false)))
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
# (e) Overlay, tuning and security software known to inject into games or change CPU behaviour: process names only.
# --------------------------------------------------------------------------------------------------------------------
Add-Section "Overlay, tuning and security software running now (process names only)"
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
# (f) README, summary, self-check of the anonymization, archive
# --------------------------------------------------------------------------------------------------------------------
$readme = @"
What this folder contains
=========================
Created by tools/diag/collect_diagnostics.ps1. The tool only READ the items below, wrote this folder on your
computer, and sent NOTHING anywhere (it has no network access). It changed no setting.

- summary.txt: processor name, microcode revision, BIOS version and date, motherboard model, memory modules
  (size, maker, part number, rated and configured speed; no serial number), Windows version, graphics driver
  version, active power plan, names of running overlay / tuning / security programs from a fixed list, names of
  DLLs loaded into the game if it is running, and the crash report list.
- whea-*.txt / whea-*.xml (only if present): Windows hardware error events (WHEA) of the last $Days days,
  from the System log (provider Microsoft-Windows-WHEA-Logger) and the Microsoft-Windows-Kernel-WHEA logs.
  "Corrected" WHEA errors near a crash time point to an unstable processor, memory or BIOS setting.
- crashes/: the $MaxCrashes most recent crash reports of the game (.txt and .dmp minidumps).

Anonymization: your user name, profile folder and computer name are replaced by [user] / [computer] in the text
files, and by x characters in the minidumps; user account SIDs are removed. Please still look at the files
before sharing them, and remove anything you do not want to share.
"@
Save-Text "README.txt" $readme

# Self-check: the secrets must not appear in any output file (UTF-16 and 8-bit forms).
$leaks = @()
foreach ($f in Get-ChildItem -LiteralPath $out -Recurse -File) {
    $t = $latin1.GetString([System.IO.File]::ReadAllBytes($f.FullName))
    foreach ($n in $secretNames + @($computerName)) {
        if (-not $n -or $n.Length -lt 3) { continue }
        foreach ($enc in @([System.Text.Encoding]::Unicode, [System.Text.Encoding]::UTF8)) {
            if ($t.IndexOf($latin1.GetString($enc.GetBytes($n)), [StringComparison]::OrdinalIgnoreCase) -ge 0) { $leaks += $f.Name; break }
        }
    }
}
Add-Section "Anonymization check"
if ($leaks.Count) { Add-Line ("WARNING: a user or computer name is still present in: {0}" -f (($leaks | Sort-Object -Unique) -join ", ")) }
else { Add-Line "OK: no user name or computer name found in the output files." }

Save-Text "summary.txt" ($summary -join "`r`n")

$zipPath = ""
if (-not $NoZip) {
    $zipPath = $out + ".zip"
    try { Compress-Archive -Path (Join-Path $out "*") -DestinationPath $zipPath -Force -ErrorAction Stop }
    catch { Write-Host ("Could not create the .zip: {0}" -f $_.Exception.Message); $zipPath = "" }
}

Write-Host ""
Write-Host "Done. Nothing was sent anywhere."
Write-Host ("Folder: {0}" -f $out)
if ($zipPath) { Write-Host ("Archive to attach to your report: {0}" -f $zipPath) }
if (-not $NoPause -and [Environment]::UserInteractive -and $Host.Name -eq "ConsoleHost") {
    Write-Host "Press Enter to close."
    [void][Console]::ReadLine()
}
