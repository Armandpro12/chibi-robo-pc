# collect_diagnostics: crash diagnostics for players

One command that gathers, in a folder and a `.zip`, what is needed to tell an unstable machine from a
bug in the game, when a crash cannot be explained by the code (for example a general-protection fault on an
instruction that cannot fault).

```
tools\collect_diagnostics.cmd
```

Double-clicking `tools\collect_diagnostics.cmd` works too. No administrator rights, no installation: it uses Windows
PowerShell 5.1, which ships with Windows 10 and 11. When it is run by hand, File Explorer opens on the result at the end.

The output goes by default to `%LOCALAPPDATA%\<user_org>\<user_app>\diagnostics\` (from `games/<game>/game.toml`
`[diagnostics]`; `%LOCALAPPDATA%\game-diagnostics\` without it), not to the Desktop: the Desktop is often
synchronised by OneDrive, which would upload the files before the player has read them. `-OutDir` chooses another
folder; if that folder is inside a OneDrive folder, `summary.txt` says so.

Without the `games/` folder (for example in an installed copy of the game), the crash folder is not known: pass it
with `-CrashDir "%APPDATA%\<user_org>\<user_app>\crashes"`.

## What it reads (and nothing else)

| Item | Source |
| --- | --- |
| Date, time and UTC offset of the collection (to line up crash times with event times) | `Get-Date`, `TimeZoneInfo.Local` |
| Processor name, cores, clocks | `Win32_Processor` (CIM) |
| Microcode revision (current and previous) | registry `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0`, values `Update Revision` / `Previous Update Revision` |
| BIOS vendor, version, date; motherboard maker and model | `Win32_BIOS`, `Win32_BaseBoard` (no serial number) |
| Memory modules: size, maker, part number, reported and configured speed (to compare with the JEDEC speed of the part number: higher means an XMP/EXPO profile) | `Win32_PhysicalMemory` (no serial number) |
| Windows edition, version, build, last boot | `Win32_OperatingSystem`, registry `...\Windows NT\CurrentVersion` (`DisplayVersion`, `UBR`) |
| Graphics adapters: name, GPU, chipset vendor, driver version and date, hardware id (vendor, device, board, revision) | `Win32_VideoController` (`Name`, `VideoProcessor`, `AdapterCompatibility`, `DriverVersion`, `DriverDate`, `PNPDeviceID` without its instance suffix; Microsoft Learn, *Win32_VideoController class*) |
| Version of the NVIDIA capture helper `nvspcap64.dll` if installed | file version information in `System32` |
| Active power plan | `powercfg /getactivescheme` |
| Hardware error events of the last 30 days (`-Days`) | System log, provider `Microsoft-Windows-WHEA-Logger`; logs `Microsoft-Windows-Kernel-WHEA/Errors` and `/Operational` |
| The 5 most recent crash reports of the game (`-MaxCrashes`) | `%APPDATA%\<user_org>\<user_app>\crashes\` from `games/<game>/game.toml` `[diagnostics]`, plus `-CrashDir` |
| Overlay, tuning and security programs running now | process names compared with a fixed list in the script (names only; common names compiled from general knowledge, not checked one by one against each product) |
| Registered antivirus products | `root/SecurityCenter2` `AntiVirusProduct`, display names only |
| Names to mask (never written): profile folder name, `USERNAME`, `COMPUTERNAME`, `USERDOMAIN`, `USERDNSDOMAIN`, the full name of a local account, the current user's SID | environment variables; `Win32_UserAccount` (`LocalAccount=True`, `FullName`); `WindowsIdentity.GetCurrent().User` |
| DLLs loaded into the game, if it is running | module names outside the Windows folder and the game folder (names only); graphics driver modules (`nv*`, `amd*`, `ati*`, `igd*`...) and known overlay / capture hooks (`nvspcap64.dll`, `GameOverlayRenderer64.dll`, `DiscordHook64.dll`, `RTSSHooks64.dll`, `graphics-hook64.dll`...) wherever they are, with file versions |

Guest memory dumps (`*-guest-ram.bin`, about 25 MiB each) are copied only with `-IncludeGuestRam`.

## What it never does

- No network access: nothing is sent anywhere. The player attaches the `.zip` to a report if they want to. (A folder
  synchronised by OneDrive would be uploaded by OneDrive itself, hence the default output folder above.)
- No setting is changed. `-ExecutionPolicy Bypass` in the `.cmd` applies to that PowerShell process only and is not
  stored in the registry (Microsoft Learn, *about_Execution_Policies*, PowerShell 5.1, "Set a different policy for
  one session").
- No other file, log or registry key is read.

## Anonymization

Secrets: the profile folder name, `USERNAME`, the computer name, `USERDOMAIN` / `USERDNSDOMAIN`, the full name of a
local account (and each of its words of 3 characters or more), account SIDs (`S-1-5-21-...`, string forms, and the
binary form of the current user's SID).

- **Text files** (`summary.txt`, `README.txt`, `whea-*`, crash `.txt`): every name of 2 characters or more is
  replaced by `[user]`, `[computer]`, `[domain]` or `[name]` where it stands alone (no ASCII letter or digit just
  before or after it), case-insensitively; SIDs become `S-1-5-21-[removed]` (no `<...>`: the `.xml` files stay
  well-formed). The crash `.txt` is read as UTF-8, or in the ANSI code page if it is not valid UTF-8.
- **Binary files** (`.dmp`, and `*-guest-ram.bin` with `-IncludeGuestRam`): same-length replacement by `x` (or `x`
  0x00 pairs), so that offsets, sizes and structure stay valid; forms searched: UTF-16LE, UTF-8 and ANSI, as written,
  upper and lower case. A name is replaced only where it stands alone, so that ordinary bytes are not altered (an
  account named `user` leaves `user32.dll` intact). A name of **2 or 3 characters** is replaced only in a context:
  after `\Users\` or `/Users/` (UTF-16 and 8-bit), after `=` (environment block) or `\\` (network name) in UTF-16.
  SIDs: the digits after `S-1-5-21-` (UTF-16 and 8-bit), and the exact bytes of the current user's binary SID.
- **Check**: after every file is written (`summary.txt` and `README.txt` included), the same rules are run again on
  each output file (text files: every name of 2+ characters in UTF-16, UTF-8 and ANSI form, the `[...]` marks being
  ignored). The verdict (`OK`, or `WARNING` with the file names) is printed and added to the end of `summary.txt`; on a
  warning no `.zip` is made and the exit code is 1. `-CheckOnly <folder>` runs only this check on an existing folder.

Limits (written in `summary.txt` when they apply):

- A name of 2 or 3 characters outside these contexts in a minidump cannot be told apart from ordinary bytes and is
  left as is; a name of 1 character is not masked at all. The check has the same blind spot.
- In a minidump, a name of 4 characters or more is replaced wherever it stands alone, including, rarely, in bytes that
  only happen to spell it.
- The full name is read only for a local account (a Microsoft account signs in through a local account); for a domain
  account it is not masked.
- Other personal data that the game or a third-party DLL may have put in memory (an e-mail address, a file name chosen
  by the player) is not searched: look at the files before sharing them.

## Reading the result

- `summary.txt`: everything above, plus, for each crash, the number of WHEA warning/error events within 10 minutes
  before and 2 minutes after it. Corrected machine-check or cache errors at the time of a crash point to the
  hardware or its settings (overclocking, undervolting, memory profile, outdated BIOS/microcode).
- No WHEA event proves nothing: many processor faults are never reported as WHEA events.
- Informational Kernel-WHEA events (for example ids 5 and 42 at each boot) are normal.
- Whether an overlay is *enabled* is not read: we found no documented place where NVIDIA stores this setting. What is
  reported is whether its process runs and which hook DLLs are loaded in the game. `nvspcap64.dll` ("NVIDIA Game
  Proxy", NVIDIA App) was seen loaded into the game on one development machine with the NVIDIA App where the game does
  not crash, so its presence alone does not explain a crash; a version difference or other hooks may.
- For the game modules, run the tool while the game is running.
- Then: `tools/crash_inspect.py` on the `.dmp` files.

## Options

`-Game <name>` (default: the only folder in `games/`), `-OutDir <folder>` (default: see above), `-Days N`,
`-MaxCrashes N`, `-CrashDir <folder>[,<folder>]`, `-IncludeGuestRam`, `-NoZip`, `-NoPause`, `-CheckOnly <folder>`.
Exit code: 0 done, 1 the anonymization check found a name or SID (no `.zip`), 2 `-CheckOnly` on a missing folder.

The tool does not depend on the game version (US or PAL): it reads the machine and whatever crash reports it finds.

## Sources

- Event queries: `Get-WinEvent -LogName <log> -FilterXPath <XPath>` and the `timediff(@SystemTime)` filter
  (Microsoft Learn, *Get-WinEvent*, PowerShell 5.1, example 16). The same query by hand:
  `wevtutil qe System /q:"*[System[Provider[@Name='Microsoft-Windows-WHEA-Logger']]]" /rd:true /f:text /c:50`
  (Microsoft Learn, *wevtutil*: `qe` with `/q:<XPath>`, `/rd`, `/f`, `/c`).
- The microcode registry values were checked on Windows 11 (4-byte value); older Windows versions store 8 bytes with
  the revision in the high 32 bits, which the script also handles (not checked on such a machine).
