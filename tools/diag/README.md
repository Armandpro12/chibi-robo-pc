# collect_diagnostics: crash diagnostics for players

One command that gathers, in a folder and a `.zip` on the Desktop, what is needed to tell an unstable machine from a
bug in the game, when a crash cannot be explained by the code (for example a general-protection fault on an
instruction that cannot fault).

```
tools\collect_diagnostics.cmd
```

Double-clicking `tools\collect_diagnostics.cmd` works too. No administrator rights, no installation: it uses Windows
PowerShell 5.1, which ships with Windows 10 and 11.

## What it reads (and nothing else)

| Item | Source |
| --- | --- |
| Processor name, cores, clocks | `Win32_Processor` (CIM) |
| Microcode revision (current and previous) | registry `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0`, values `Update Revision` / `Previous Update Revision` |
| BIOS vendor, version, date; motherboard maker and model | `Win32_BIOS`, `Win32_BaseBoard` (no serial number) |
| Memory modules: size, maker, part number, rated and configured speed (XMP/EXPO) | `Win32_PhysicalMemory` (no serial number) |
| Windows edition, version, build, last boot | `Win32_OperatingSystem`, registry `...\Windows NT\CurrentVersion` (`DisplayVersion`, `UBR`) |
| Graphics adapters: name, GPU, chipset vendor, driver version and date, hardware id (vendor, device, board, revision) | `Win32_VideoController` (`Name`, `VideoProcessor`, `AdapterCompatibility`, `DriverVersion`, `DriverDate`, `PNPDeviceID` without its instance suffix; Microsoft Learn, *Win32_VideoController class*) |
| Version of the NVIDIA capture helper `nvspcap64.dll` if installed | file version information in `System32` |
| Active power plan | `powercfg /getactivescheme` |
| Hardware error events of the last 30 days (`-Days`) | System log, provider `Microsoft-Windows-WHEA-Logger`; logs `Microsoft-Windows-Kernel-WHEA/Errors` and `/Operational` |
| The 5 most recent crash reports of the game (`-MaxCrashes`) | `%APPDATA%\<user_org>\<user_app>\crashes\` from `games/<game>/game.toml` `[diagnostics]`, plus `-CrashDir` |
| Overlay, tuning and security programs running now | process names compared with a fixed list in the script (names only) |
| Registered antivirus products | `root/SecurityCenter2` `AntiVirusProduct`, display names only |
| DLLs loaded into the game, if it is running | module names outside the Windows folder and the game folder (names only); graphics driver modules (`nv*`, `amd*`, `ati*`, `igd*`...) and known overlay / capture hooks (`nvspcap64.dll`, `GameOverlayRenderer64.dll`, `DiscordHook64.dll`, `RTSSHooks64.dll`, `graphics-hook64.dll`...) wherever they are, with file versions |

Guest memory dumps (`*-guest-ram.bin`, about 25 MiB each) are copied only with `-IncludeGuestRam`.

## What it never does

- No network access: nothing is sent anywhere. The player attaches the `.zip` to a report if they want to.
- No setting is changed. `-ExecutionPolicy Bypass` in the `.cmd` applies to that PowerShell process only and is not
  stored in the registry (Microsoft Learn, *about_Execution_Policies*, PowerShell 5.1, "Set a different policy for
  one session").
- No other file, log or registry key is read.

## Anonymization

Before writing, every output file is scrubbed of the user name (profile folder name and `USERNAME`), the computer
name and user account SIDs (`S-1-5-21-...`): `[user]` / `[computer]` in text files; same-length `x` characters in
the minidumps (UTF-16, UTF-8 and ANSI forms), so that their structure stays valid. A final check searches every
output file for these names and prints `OK` or a warning. The crash `.txt` is already scrubbed by the game's crash
handler; the `.dmp` is not.

## Reading the result

- `summary.txt`: everything above, plus, for each crash, the number of WHEA warning/error events within 10 minutes
  before and 2 minutes after it. Corrected machine-check or cache errors at the time of a crash point to the
  hardware or its settings (overclocking, undervolting, memory profile, outdated BIOS/microcode).
- No WHEA event proves nothing: many processor faults are never reported as WHEA events.
- Informational Kernel-WHEA events (for example ids 5 and 42 at each boot) are normal.
- Whether an overlay is *enabled* is not read: NVIDIA does not document where the setting is stored. What is reported
  is whether its process runs and which hook DLLs are loaded in the game. `nvspcap64.dll` ("NVIDIA Game Proxy",
  NVIDIA App) is loaded into the game on a machine with the NVIDIA App even when the game is stable, so its presence
  alone explains nothing; a version difference or other hooks may.
- For the game modules, run the tool while the game is running.
- Then: `tools/crash_inspect.py` on the `.dmp` files.

## Options

`-Game <name>` (default: the only folder in `games/`), `-OutDir <folder>` (default: Desktop), `-Days N`,
`-MaxCrashes N`, `-CrashDir <folder>[,<folder>]`, `-IncludeGuestRam`, `-NoZip`, `-NoPause`.

## Sources

- Event queries: `Get-WinEvent -LogName <log> -FilterXPath <XPath>` and the `timediff(@SystemTime)` filter
  (Microsoft Learn, *Get-WinEvent*, PowerShell 5.1, example 16). The same query by hand:
  `wevtutil qe System /q:"*[System[Provider[@Name='Microsoft-Windows-WHEA-Logger']]]" /rd:true /f:text /c:50`
  (Microsoft Learn, *wevtutil*: `qe` with `/q:<XPath>`, `/rd`, `/f`, `/c`).
- The microcode registry values were checked on Windows 11 (4-byte value); older Windows versions store 8 bytes with
  the revision in the high 32 bits, which the script also handles (not checked on such a machine).
