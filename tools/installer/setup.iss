; Player installer for a recompiled GameCube game (Inno Setup 6, https://jrsoftware.org/isinfo.php).
;
; Do not compile this file directly: tools/installer/build.py assembles the payload (game executable and its DLLs,
; gcextract.exe, the game description, licence notices; NO game data) and runs ISCC with the defines below.
;
; What the installed copy contains: the executables, a COPY of the player's own disc image ({app}\game.iso, read by
; the game's native DVD layer), {app}\disc\sys\ (extracted by gcextract --system-only) and {app}\manifest.json
; ("image": "game.iso", relative to the manifest's folder). Saves and settings live in the user folder (%APPDATA%),
; never in {app}: the uninstaller never touches them.
;
; Silent install (tests, automation):
;   setup.exe /SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR="C:\Games\X" /IMAGE="D:\my.iso"
; (from Git Bash, set MSYS_NO_PATHCONV=1 first: otherwise "/VERYSILENT" is rewritten into a path and the wizard opens).
; The /IMAGE= value fills the "disc image" page; an image that is not recognised makes Setup exit before installing.
; /TESTMODE=1 (automated tests on a developer machine): no Programs-list entry (registry) and no shortcuts, so that
; nothing is written outside the installation folder; uninstall by running {app}\unins000.exe.
; /TESTFAIL=<stage> (only with /TESTMODE=1, no effect otherwise) simulates a failure, to test that a failed (re)install
; keeps the previous installation, or leaves nothing behind in a new folder, and leaves no temporary file:
;   extract  the extraction (before anything is installed) is reported as failed after gcextract has run (exit code 7);
;   copy     the check of the copied image (during the installation, before the swap) is reported as failed (exit 9);
;   raise    an unexpected error (exception) right after the swap of the image files (exit 9);
;   exe      an Abort/Retry/Ignore file error after the executables (a missing source file), answered "Abort" by
;            /SUPPRESSMSGBOXES: Inno Setup rolls the installation back, as for Cancel (exit code 5);
;   dirpage  the folder check of the folder page is skipped, as when that page is not shown (reinstall found in the
;            registry): only the check made again in PrepareToInstall applies;
;   prepare  an unexpected error (exception) in PrepareToInstall, after the extraction (exit code 7);
;   space    the free disk space is reported as 0 (exit code 7, before anything is written);
;   missing  manifest.new is deleted just before the swap: the swap is refused, nothing replaced (exit code 9);
;   undo     the swap is reported as failed after moving every file, and putting back the previous manifest.json is
;            reported as failed (exit code 9): manifest.json.old must be kept, and the next run puts it back.
;
; Safety rules:
; - a first install only goes into a new or empty folder whose path is ASCII: a folder that holds files and was not
;   installed by this installer (no InstallMarker file with this AppId) is refused, so that nothing of the player's is
;   overwritten now or deleted by the uninstaller later (it removes fixed names: game.iso, disc\, manifest.json,
;   gcrt-data\); the marker is written first, so that an interrupted run leaves a folder that a later run may reuse;
; - the extraction (disc.new\, manifest.new) runs in PrepareToInstall, before anything is installed: a failure there
;   stops Setup (exit code 7) with nothing changed; the image is copied as game.iso.new, checked again, and only then
;   are game.iso, disc\ and manifest.json replaced (renames: the previous ones are kept as *.old until the end of the
;   installation), before the executables: a failure there puts the previous files back, skips the rest of the payload
;   and makes Setup exit with code 9 (GetCustomSetupExitCode);
; - Inno Setup's rollback does not restore a file it has overwritten: so, before a file of the payload (executables,
;   DLLs, BUILD-INFO.txt...) is written over a previous one, the previous one becomes <name>.old (BackUpExisting);
;   whenever the installation does not complete (exit code 9, cancel, error), those are put back too, so that the
;   executable always matches the disc image files (a US executable never runs with a PAL image, or the reverse); they
;   are deleted once the installation is complete;
; - Setup cancelled or stopped by an error after the swap (Inno Setup then rolls back the files it installed): the
;   previous game.iso, disc\, manifest.json and payload files are put back (DeinitializeSetup);
; - a failed (re)install does not create or update the Programs-list entry (CreateUninstallRegKey);
; - free disk space is checked before anything is written: the new copy of the image, the payload and a margin (the
;   previous image is kept until the end, so an update needs as much free space as a first install);
; - whenever a first install into a new folder does not complete (exit code 7, 9, cancel, error), everything this run
;   wrote is removed, the folder included, and the parent folders it created: no orphan disc image, no "shell"
;   installation, no uninstaller entry.

; ---------------------------------------------------------------------------------------- defines from build.py
#ifndef AppName
  #error Run tools/installer/build.py instead of compiling this file directly.
#endif
; AppName        name shown to the player (wizard, Start menu, Programs list)
; GameName       name of the game, for messages
; AppSlug        ASCII short name (default folder, output file)
; AppId          stable GUID (derived from the slug by build.py): reinstalling updates the same install
; AppVersion     installer version, e.g. "2026.10.19+e9c72deec287" (the commit of the game executable)
; ExeCommits     commit(s) of the game executable(s), shown on the disc image page
; ExeName        file name of the game executable
; PayloadDir     folder assembled by build.py (common\, <game ID>\, versions.iss)
; OutputDir      where the installer is written
; OutputName     installer file name without .exe
; ImageSize      size of a full disc image in bytes (disk space estimate for the copy)
; UserDataDir    where the game keeps saves and settings, e.g. "{userappdata}\Org\App"
; SupportedList  human-readable list of the recognised versions
; PassDiscArg    1: shortcuts pass --disc "{app}\disc" (executables that do not find disc\ next to themselves)
; payload.iss (written by build.py into PayloadDir) defines:
;   PayloadNames  "|"-terminated list of the payload files installed into {app} (relative names)
;   PayloadBytes  total size of the payload in bytes (free disk space check)
#include PayloadDir + "\payload.iss"

; Quotes doubled: the value is used inside quoted [Icons]/[Run] parameters.
#define DiscArg PassDiscArg == "1" ? '--disc ""{app}\disc""' : ""
; Written into {app} after a successful install (contains the AppId): marks the folder as an installation of this
; installer, which a later run may update.
#define InstallMarker "installed-by-setup.txt"

[Setup]
AppId={{{#AppId}}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName} contributors
VersionInfoVersion=1.0.0.0
VersionInfoProductTextVersion={#AppVersion}
DefaultDirName={autopf}\{#AppSlug}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Per-user install (%LOCALAPPDATA%\Programs\...): no administrator rights, no UAC prompt; the game writes its
; GPU pipeline cache next to the executable (gcrt-data\), which must therefore stay writable.
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
UninstallDisplayIcon={app}\{#ExeName}
UninstallDisplayName={#AppName}
UsePreviousAppDir=yes
CloseApplications=yes
; No Programs-list entry in test mode, nor for an install whose image files could not be put in place (exit code 9):
; a failed first install is removed again by DeinitializeSetup, and a failed reinstall keeps the entry of the
; installation that is still there (with its version) instead of writing the new version into it. Inno Setup evaluates
; this when it saves the uninstall information, after [Files] (seen in the log); Uninstallable is evaluated before
; [Files], so the uninstaller files of such a failed first install are written, then removed with the rest
; (RemoveFirstInstall).
CreateUninstallRegKey=UninstallEntryWanted

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Tools needed before the installation starts (checking the image on its wizard page): first, so that a solid
; archive does not have to decompress everything else to reach them.
; Every payload entry: BeforeInstall: BackUpExisting (the previous file becomes <name>.old, see "Safety rules").
Source: "{#PayloadDir}\common\gcextract.exe"; DestDir: "{app}"; Flags: ignoreversion; BeforeInstall: BackUpExisting
Source: "{#PayloadDir}\common\game.toml"; DestDir: "{app}"; Flags: ignoreversion; BeforeInstall: BackUpExisting
; The player's disc image, copied from their disk (not part of the installer), under a temporary name.
Source: "{code:GetImagePath}"; DestDir: "{app}"; DestName: "game.iso.new"; ExternalSize: {#ImageSize}; Flags: external ignoreversion; Check: ImageNeedsCopy
; After the copy, before the executables: check the copy, then put the new image and extraction in place. A failure
; there keeps the previous files and skips the entries below (an exception in AfterInstall does not abort Setup:
; Inno Setup reports it and goes on, seen in test; FinishImageAndExtraction catches every exception itself).
Source: "{#PayloadDir}\BUILD-INFO.txt"; DestDir: "{app}"; Flags: ignoreversion; BeforeInstall: BackUpExisting; AfterInstall: FinishImageAndExtraction
Source: "{#PayloadDir}\common\*"; Excludes: "gcextract.exe,game.toml"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: not InstallFailed; BeforeInstall: BackUpExisting
; Executable(s) and the DLLs that differ between versions: only the files of the version of the chosen image.
#include PayloadDir + "\versions.iss"
; Test only (/TESTMODE=1 /TESTFAIL=exe): a source file that does not exist, after the executables, so that Setup stops
; with a file error after the swap (see the top of this file).
Source: "{code:TestFailSource}"; DestDir: "{app}"; DestName: "testfail-exe.bin"; ExternalSize: 1; Flags: external ignoreversion; Check: TestFailure('exe')

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#ExeName}"; Parameters: "{#DiscArg}"; WorkingDir: "{app}"; Check: not IsTestMode and not InstallFailed
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#ExeName}"; Parameters: "{#DiscArg}"; WorkingDir: "{app}"; Tasks: desktopicon; Check: not IsTestMode and not InstallFailed

[Run]
Filename: "{app}\{#ExeName}"; Parameters: "{#DiscArg}"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent; Check: not InstallFailed

[UninstallDelete]
; Not in the uninstall log under these names (renamed from their temporary names, written by gcextract or by the
; game: GPU caches): removed with the game. They are ours: a first install only goes into a new or empty folder.
; The user folder (saves, settings, crash reports) is elsewhere and is never removed.
Type: files; Name: "{app}\game.iso"
Type: filesandordirs; Name: "{app}\disc"
Type: files; Name: "{app}\manifest.json"
Type: filesandordirs; Name: "{app}\gcrt-data"
Type: files; Name: "{app}\{#InstallMarker}"
; Leftovers of an interrupted install, if any.
Type: files; Name: "{app}\game.iso.new"
Type: filesandordirs; Name: "{app}\disc.new"
Type: files; Name: "{app}\manifest.new"
Type: files; Name: "{app}\game.iso.old"
Type: filesandordirs; Name: "{app}\disc.old"
Type: files; Name: "{app}\manifest.json.old"
; Previous payload files (<name>.old, see BackUpExisting) left by an interrupted install, if any.
#include PayloadDir + "\uninstall-old.iss"

[Code]
const
  { RefusalText code of an unexpected error (exception) while putting the image files in place; gcextract's own exit
    codes are 0 to 7. }
  UnexpectedError = 100;

var
  ImagePage: TInputFileWizardPage;
  CheckPage: TOutputProgressWizardPage;
  GameId: String;          { game ID of the accepted image (INFO match), selects the executable }
  GcxError: String;        { message of the last ERROR line }
  GcxMatch: String;        { game ID of the last INFO match line }
  GcxInstalling: Boolean;  { True during the check of the copy (progress on the installing page) }
  GcxBusy: Boolean;        { gcextract is running: Cancel is disabled and refused (CancelButtonClick) }
  FailedFlag: Boolean;     { the copy could not be checked or put in place: the rest of the payload is skipped }
  InstallDone: Boolean;    { ssPostInstall reached }
  PreparedDir: Boolean;    { PrepareToInstall wrote into the folder (marker, temporary files) }
  DirWasNew: Boolean;      { the folder was missing or empty before this run }
  PreparedApp: String;     { the folder PrepareToInstall wrote into (the wizard may be gone in DeinitializeSetup) }
  SwapDone: Boolean;       { SwapInNewFiles moved files: the previous ones are *.old until ssPostInstall or UndoSwap }
  SwapMoved: array of Boolean;   { per SwapNames entry: Final renamed to Final.old (and not put back yet) }
  SwapPlaced: array of Boolean;  { per SwapNames entry: New renamed to Final (and not removed yet) }
  Backups: TArrayOfString;       { payload files kept as <name>.old by BackUpExisting ('' once put back) }
  BackupFailed: Boolean;         { a payload file could not be kept: it cannot be put back }
  CreatedDirs: TArrayOfString;   { folders created by PrepareSystemFiles (the install folder and its parents), deepest
                                   first: removed again, if empty, when a first install does not complete }

{ Check of the payload entries after the image: skipped once the copy failed. }
function InstallFailed: Boolean;
begin
  Result := FailedFlag;
end;


function IsTestMode: Boolean;
begin
  Result := ExpandConstant('{param:TESTMODE|0}') = '1';
end;

{ CreateUninstallRegKey: no entry in test mode, nor for a failed install: a failed first install is removed again by
  DeinitializeSetup; after a failed reinstall, the entry of the installation still in place is left as it is (Inno
  Setup does not create or update it, see CreateUninstallRegKey in its help). Logged, to show when Inno Setup evaluates
  it (the failure is logged first, so that the test mode shows it too). }
function UninstallEntryWanted: Boolean;
begin
  Result := not IsTestMode and not FailedFlag;
  if FailedFlag and DirWasNew then
    Log('Programs-list entry: no (failed first install)')
  else if FailedFlag then
    Log('Programs-list entry: not updated (failed reinstall)')
  else if IsTestMode then
    Log('Programs-list entry: no (/TESTMODE=1)')
  else
    Log('Programs-list entry: yes');
end;

function TestFailure(const Stage: String): Boolean;
begin
  Result := IsTestMode and (ExpandConstant('{param:TESTFAIL|}') = Stage);
end;

{ Source of the /TESTFAIL=exe entry: a file that does not exist. }
function TestFailSource(Param: String): String;
begin
  Result := ExpandConstant('{tmp}\missing-file-for-testfail-exe.bin');
end;

{ The single place that reads the image field: every user of the path sees the same trimmed, unquoted value. }
function GetImagePath(Param: String): String;
begin
  Result := RemoveQuotes(Trim(ImagePage.Values[0]));
end;

{ Installation folder; WizardDirValue is valid from the folder page on (PrepareToInstall included). }
function AppDir: String;
begin
  Result := RemoveBackslashUnlessRoot(WizardDirValue);
end;

function InstalledImage: String;
begin
  Result := AppDir + '\game.iso';
end;

function ImageNeedsCopy: Boolean;
begin
  { Reinstalling from the copy already in the install folder: nothing to copy. }
  Result := CompareText(ExpandFileName(GetImagePath('')), ExpandFileName(InstalledImage)) <> 0;
end;

{ True if Dir does not exist or holds nothing (no file, no folder). A folder that exists but cannot be listed (access
  denied, for example) counts as NOT empty: what it holds is unknown, so it is refused. So does an empty drive root
  (no "." entry: FindFirst finds nothing), which is not an installation folder anyway. }
function IsDirEmptyOrMissing(const Dir: String): Boolean;
var
  Rec: TFindRec;
begin
  Result := True;
  if not DirExists(Dir) then
    Exit;
  if FindFirst(AddBackslash(Dir) + '*', Rec) then begin
    try
      repeat
        if (Rec.Name <> '.') and (Rec.Name <> '..') then begin
          Result := False;
          Exit;
        end;
      until not FindNext(Rec);
    finally
      FindClose(Rec);
    end;
  end else begin
    Log('Cannot list the folder ' + Dir + ': counted as not empty');
    Result := False;
  end;
end;

{ True if Dir is an installation made by this installer (marker file with this AppId). }
function IsOurInstall(const Dir: String): Boolean;
var
  Text: AnsiString;
begin
  Result := LoadStringFromFile(AddBackslash(Dir) + '{#InstallMarker}', Text) and (Pos('{#AppId}', String(Text)) > 0);
end;

function IsAsciiPath(const S: String): Boolean;
var
  I: Integer;
begin
  Result := True;
  for I := 1 to Length(S) do
    if Ord(S[I]) > 126 then
      Result := False;
end;

{ '' if Setup may install into Dir, else the reason (see "Safety rules" at the top). Used by the folder page and again
  by PrepareToInstall (the folder page is skipped when a previous installation is found). }
function DestinationProblem(const Dir: String): String;
begin
  Result := '';
  { The game opens its files through the ANSI code page: keep the installation path ASCII. }
  if not IsAsciiPath(Dir) then begin
    Result := 'The folder "' + Dir + '" cannot be used: choose an installation folder whose path contains only ASCII ' +
      'characters (A-Z, 0-9, spaces and basic punctuation).';
    Exit;
  end;
  if IsDirEmptyOrMissing(Dir) or IsOurInstall(Dir) then
    Exit;
  Result := 'The folder "' + Dir + '" already contains files that were not installed by this installer.' + #13#10#13#10 +
    'To protect them, {#AppName} can only be installed into a new or empty folder (or over a previous installation ' +
    'of {#AppName}). Choose another folder, for example a new "{#AppSlug}" folder inside it.';
end;

function IsGameVersion(Id: String): Boolean;
begin
  Result := (GameId = Id) and not FailedFlag;
end;

{ First space-separated token of S, removed from S. }
function NextToken(var S: String): String;
var
  P: Integer;
begin
  S := Trim(S);
  P := Pos(' ', S);
  if P = 0 then begin
    Result := S;
    S := '';
  end else begin
    Result := Copy(S, 1, P - 1);
    S := Trim(Copy(S, P + 1, Length(S)));
  end;
end;

{ One line of gcextract's machine-readable output (tools/gcextract/gcextract.cpp: INFO / PROGRESS / DONE / ERROR). }
procedure GcxOnLog(const S: String; const Error, FirstLine: Boolean);
var
  Rest, Kind, Stage, Text: String;
  Done, Total: Int64;
  Percent: Integer;
begin
  if Error then begin
    GcxError := S;
    Log('gcextract output error: ' + S);
    Exit;
  end;
  Log('gcextract: ' + S);
  Rest := S;
  Kind := NextToken(Rest);
  if Kind = 'PROGRESS' then begin
    Stage := NextToken(Rest);
    Done := StrToInt64Def(NextToken(Rest), 0);
    Total := StrToInt64Def(NextToken(Rest), 0);
    if Total > 0 then
      Percent := Integer(Done * 100 div Total)
    else
      Percent := 100;
    if Stage = 'verify' then
      Text := 'Checking the disc image'
    else
      Text := 'Extracting the system files';
    if GcxInstalling then begin
      WizardForm.StatusLabel.Caption := Text + '... ' + IntToStr(Percent) + '%';
      WizardForm.StatusLabel.Update;
    end else if CheckPage <> nil then begin
      CheckPage.SetText(Text + '... ' + IntToStr(Percent) + '%', GetImagePath(''));
      CheckPage.SetProgress(Percent, 100);
    end;
  end else if Kind = 'INFO' then begin
    if NextToken(Rest) = 'match' then
      GcxMatch := NextToken(Rest);
  end else if Kind = 'ERROR' then begin
    NextToken(Rest);  { exit code, also returned by the process }
    GcxError := Rest;
  end;
end;

{ Runs gcextract with Params; returns its exit code (-1 if it could not be started). Cancel is disabled meanwhile (on
  the image page, the preparing page and the installing page alike): gcextract is not stopped halfway, and the
  check of the copy is not cut between the check and the swap. }
function RunGcextract(const Exe, Params: String): Integer;
var
  Code: Integer;
  CancelWasEnabled: Boolean;
begin
  GcxError := '';
  GcxMatch := '';
  Log('Running: "' + Exe + '" ' + Params);
  CancelWasEnabled := WizardForm.CancelButton.Enabled;
  WizardForm.CancelButton.Enabled := False;
  GcxBusy := True;
  try
    try
      if not ExecAndLogOutput(Exe, Params, '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code, @GcxOnLog) then begin
        GcxError := SysErrorMessage(Code);
        Code := -1;
      end;
    except
      GcxError := GetExceptionMessage;
      Code := -1;
    end;
  finally
    GcxBusy := False;
    WizardForm.CancelButton.Enabled := CancelWasEnabled;
  end;
  Log('gcextract exit code: ' + IntToStr(Code));
  Result := Code;
end;

{ Text shown to the player for a gcextract exit code (codes: tools/gcextract/gcextract.cpp, enum Exit). }
function RefusalText(Code: Integer): String;
begin
  case Code of
    3: Result := 'The file could not be read. Check that it still exists, that it is not open in another program, and that the drive is connected.';
    4: Result := 'This file is not an uncompressed GameCube disc image (.iso or .gcm).' + #13#10#13#10 +
         'Compressed or converted formats (RVZ, WIA, GCZ, CISO, NKit), archives (ZIP, 7z, RAR) and Wii images are not supported. ' +
         'Convert your own dump back to a plain ISO first (for example with the "Convert File" feature of Dolphin), then choose that file.';
    5: Result := 'This disc image is not a recognised copy of {#GameName}.' + #13#10#13#10 +
         'Supported: {#SupportedList}. A modified, trimmed (NKit), patched or damaged dump is refused: use a complete, unmodified dump of your own disc.';
    6: Result := 'The disc image is damaged (its file table points outside the image). Make a new dump of your disc.';
    7: Result := 'The game files could not be written (disk full, or folder not writable).';
    UnexpectedError: Result := 'An unexpected error stopped the installation.';
  else
    Result := 'The disc image could not be checked.';
  end;
  if GcxError <> '' then
    Result := Result + #13#10#13#10 + 'Details: ' + GcxError;
end;

{ Checks the chosen image before anything is installed. }
function VerifyImage: Boolean;
var
  Image, Exe: String;
  Code: Integer;
begin
  Result := False;
  Image := Trim(GetImagePath(''));
  if Image = '' then begin
    SuppressibleMsgBox('Choose the disc image of your own copy of {#GameName} (.iso file).', mbError, MB_OK, IDOK);
    Exit;
  end;
  if not FileExists(Image) then begin
    SuppressibleMsgBox('The file "' + Image + '" does not exist.', mbError, MB_OK, IDOK);
    Exit;
  end;
  ExtractTemporaryFile('gcextract.exe');
  ExtractTemporaryFile('game.toml');
  Exe := ExpandConstant('{tmp}\gcextract.exe');
  CheckPage.SetText('Checking the disc image...', Image);
  CheckPage.SetProgress(0, 100);
  CheckPage.Show;
  try
    Code := RunGcextract(Exe, AddQuotes(Image) + ' --verify-only --game-toml ' + AddQuotes(ExpandConstant('{tmp}\game.toml')));
  finally
    CheckPage.Hide;
  end;
  if (Code = 0) and (GcxMatch <> '') then begin
    GameId := GcxMatch;
    Log('Accepted disc image: ' + Image + ' (' + GameId + ')');
    Result := True;
  end else begin
    Log('Refused disc image: ' + Image + ' (exit code ' + IntToStr(Code) + ')');
    SuppressibleMsgBox(RefusalText(Code), mbError, MB_OK, IDOK);
  end;
end;

{ gcextract writes absolute paths (of the image it read, the player's own file, and of its output folder, disc.new); the
  installed copy must keep working if the folder is moved: "image" becomes "game.iso" and "disc_root" "disc",
  relative to the manifest's folder (= the working directory of the shortcuts, so executables that open the image
  relative to the current directory work too). }
function MakeManifestRelative(const Manifest: String): Boolean;
var
  Lines: TArrayOfString;
  I: Integer;
  Found: Boolean;
begin
  Result := False;
  Found := False;
  if not LoadStringsFromFile(Manifest, Lines) then
    Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
    if Pos('"image"', Trim(Lines[I])) = 1 then begin
      Lines[I] := '  "image": "game.iso",';
      Found := True;
    end else if Pos('"disc_root"', Trim(Lines[I])) = 1 then
      Lines[I] := '  "disc_root": "disc",';
  Result := Found and SaveStringsToUTF8FileWithoutBOM(Manifest, Lines, False);
end;

procedure RemovePath(const Path: String);
begin
  if DirExists(Path) then
    DelTree(Path, True, True, True)
  else if FileExists(Path) then
    DeleteFile(Path);
end;

{ Names of the files replaced at the end of the extraction, in folder App: Final[I] <- New[I] (Final[I].old
  meanwhile). }
procedure SwapNames(const App: String; var Final, New: TArrayOfString);
begin
  SetArrayLength(Final, 3);
  SetArrayLength(New, 3);
  Final[0] := App + '\disc';          New[0] := App + '\disc.new';
  Final[1] := App + '\manifest.json'; New[1] := App + '\manifest.new';
  Final[2] := App + '\game.iso';      New[2] := App + '\game.iso.new';
end;

procedure RemoveTemporaryFiles(const App: String);
var
  Final, New: TArrayOfString;
  I: Integer;
begin
  SwapNames(App, Final, New);
  for I := 0 to GetArrayLength(New) - 1 do
    RemovePath(New[I]);
end;

{ Adds the names of the "|"-terminated list Names, relative to App, to List. }
procedure AddNames(const App: String; Names: String; var List: TArrayOfString);
var
  P, N: Integer;
begin
  while Names <> '' do begin
    P := Pos('|', Names);
    N := GetArrayLength(List);
    SetArrayLength(List, N + 1);
    List[N] := App + '\' + Copy(Names, 1, P - 1);
    Names := Copy(Names, P + 1, Length(Names));
  end;
end;

{ Before anything is installed: finish or undo an interrupted run (power loss, crash): the <name>.old of a name that is
  missing is the previous file (image files: SwapInNewFiles; payload files: BackUpExisting), put back; a <name>.old
  whose name exists is no longer needed and is deleted, but only then: if it cannot be put back, it stays (it is the
  only copy of the previous file) and False is returned (Setup stops: run it again once the file is free). Then the
  temporary files are removed. }
function CleanUpTemporaryFiles(const App: String): Boolean;
var
  Final, New, Names: TArrayOfString;
  I: Integer;
begin
  Result := True;
  SwapNames(App, Final, New);
  AddNames(App, 'disc|manifest.json|game.iso|{#PayloadNames}', Names);
  for I := 0 to GetArrayLength(Names) - 1 do
    if FileOrDirExists(Names[I] + '.old') then begin
      if not FileOrDirExists(Names[I]) then begin
        if RenameFile(Names[I] + '.old', Names[I]) then
          Log('Restored ' + Names[I] + ' from an interrupted install')
        else
          Log('Cannot restore ' + Names[I] + ' from an interrupted install: ' + Names[I] + '.old kept');
      end;
      if FileOrDirExists(Names[I]) then
        RemovePath(Names[I] + '.old')
      else
        Result := False;
    end;
  for I := 0 to GetArrayLength(New) - 1 do
    RemovePath(New[I]);
end;

{ Puts the previous files back after a swap (SwapInNewFiles, partial or complete): the new files are deleted, the
  *.old renamed back. True only if everything is back. The state of each name is kept (SwapPlaced, SwapMoved, and
  SwapDone stays True), so that a step that failed is tried again by the next call, and the result is never True while
  something is not back. A name that cannot be restored keeps its *.old, which the next run puts back
  (CleanUpTemporaryFiles) if the name is then missing. }
function UndoSwap(const App: String): Boolean;
var
  Final, New: TArrayOfString;
  I: Integer;
begin
  Result := True;
  if not SwapDone then
    Exit;
  SwapNames(App, Final, New);
  for I := GetArrayLength(Final) - 1 downto 0 do begin
    if SwapPlaced[I] then begin
      RemovePath(Final[I]);
      if FileOrDirExists(Final[I]) then begin
        Log('Cannot remove the new ' + Final[I]);
        Result := False;
      end else
        SwapPlaced[I] := False;
    end;
    if SwapMoved[I] then begin
      if SwapPlaced[I] then
        Result := False  { the new one is still there: the previous one cannot take its name }
      else if TestFailure('undo') and (I = 1) then begin
        Log('Simulated failure to restore the previous ' + Final[I] + ' (/TESTFAIL=undo)');
        Result := False;
      end else if RenameFile(Final[I] + '.old', Final[I]) then begin
        Log('Restored the previous ' + Final[I]);
        SwapMoved[I] := False;
      end else begin
        Log('Cannot restore the previous ' + Final[I]);
        Result := False;
      end;
    end;
  end;
  SwapDone := not Result;
end;

{ Moves the new files into place, the previous ones becoming *.old (kept until ssPostInstall, so that a later failure
  can put them back: UndoSwap). disc.new and manifest.new must exist, and game.iso.new too when the image was copied:
  if one is missing, nothing is moved and the swap fails. On any failure here, puts everything back as it was (as far
  as possible: the result of UndoSwap is logged, and FinishImageAndExtraction asks it again). }
function SwapInNewFiles: Boolean;
var
  Final, New: TArrayOfString;
  Wanted: array of Boolean;
  I, N: Integer;
begin
  SwapNames(AppDir, Final, New);
  N := GetArrayLength(Final);
  SetArrayLength(SwapMoved, N);
  SetArrayLength(SwapPlaced, N);
  SetArrayLength(Wanted, N);
  Result := True;
  for I := 0 to N - 1 do begin
    SwapMoved[I] := False;
    SwapPlaced[I] := False;
    { New[2] is game.iso.new: there is none when the installed copy is reused. }
    Wanted[I] := (I <> 2) or ImageNeedsCopy;
    if Wanted[I] and not FileOrDirExists(New[I]) then begin
      Log('Missing ' + New[I] + ': the swap is not made');
      Result := False;
    end;
  end;
  if not Result then
    Exit;
  { Set before the first rename: UndoSwap must see a partial swap too. }
  SwapDone := True;
  for I := 0 to N - 1 do
    if Result and Wanted[I] then begin
      if FileOrDirExists(Final[I]) then begin
        SwapMoved[I] := RenameFile(Final[I], Final[I] + '.old');
        if not SwapMoved[I] then begin
          Log('Cannot rename ' + Final[I]);
          Result := False;
        end;
      end;
      if Result then begin
        SwapPlaced[I] := RenameFile(New[I], Final[I]);
        if not SwapPlaced[I] then begin
          Log('Cannot rename ' + New[I]);
          Result := False;
        end;
      end;
    end;
  if Result and TestFailure('undo') then begin
    Log('Simulated failure of the swap after every rename (/TESTFAIL=undo)');
    Result := False;
  end;
  if Result then
    Log('Swap done: the new disc image files are in place')
  else if UndoSwap(AppDir) then
    Log('Swap failed: the previous files are back')
  else
    Log('Swap failed, and some previous files could not be put back');
end;

{ BeforeInstall of every payload entry (called for each file of a wildcard entry): Inno Setup's rollback does not
  restore a file that it has overwritten, so the previous file, if any, becomes <name>.old first (renamed; copied if it
  cannot be renamed, Inno Setup then writing over the original). Put back by RestoreBackups when the installation does
  not complete, deleted by RemoveBackups when it does. }
procedure BackUpExisting;
var
  F: String;
  N: Integer;
begin
  F := ExpandConstant(CurrentFileName);
  if not FileExists(F) then
    Exit;
  { A stale <name>.old was already handled by CleanUpTemporaryFiles; one left now is not a previous version. }
  if FileExists(F + '.old') then
    DeleteFile(F + '.old');
  if RenameFile(F, F + '.old') or CopyFile(F, F + '.old', True) then begin
    N := GetArrayLength(Backups);
    SetArrayLength(Backups, N + 1);
    Backups[N] := F;
    Log('Kept the previous ' + F + ' as .old');
  end else begin
    BackupFailed := True;
    Log('Cannot keep the previous ' + F + ': it cannot be put back if the installation fails');
  end;
end;

{ Puts the previous payload files back (see BackUpExisting): the new file, if Inno Setup's rollback left it, is deleted
  and <name>.old renamed back. True only if every previous file is back (and none could not be kept). A file that
  cannot be put back keeps its <name>.old, which the next run puts back if the name is then missing. }
function RestoreBackups: Boolean;
var
  I: Integer;
  F: String;
begin
  Result := not BackupFailed;
  for I := GetArrayLength(Backups) - 1 downto 0 do begin
    F := Backups[I];
    if F <> '' then begin
      if FileExists(F) then
        DeleteFile(F);
      if FileExists(F) then begin
        Log('Cannot remove the new ' + F);
        Result := False;
      end else if RenameFile(F + '.old', F) then begin
        Log('Restored the previous ' + F);
        Backups[I] := '';
      end else begin
        Log('Cannot restore the previous ' + F);
        Result := False;
      end;
    end;
  end;
end;

{ The installation is complete: the previous payload files are no longer needed. }
procedure RemoveBackups;
var
  I: Integer;
begin
  for I := 0 to GetArrayLength(Backups) - 1 do
    if Backups[I] <> '' then
      DeleteFile(Backups[I] + '.old');
end;

{ Removes the previous files kept by the swap, once the installation is complete. }
procedure RemoveOldFiles(const App: String);
var
  Final, New: TArrayOfString;
  I: Integer;
begin
  SwapNames(App, Final, New);
  for I := 0 to GetArrayLength(Final) - 1 do
    RemovePath(Final[I] + '.old');
end;

{ A first install into a folder that was missing or empty did not complete: removes what this run put there (every
  name the installer writes; Inno Setup's rollback has already removed the payload files it installed, or they were
  skipped), then the folder itself if it is empty, then the parent folders this run created, if they are empty. }
procedure RemoveFirstInstall(const App: String);
var
  Names: TArrayOfString;
  I: Integer;
begin
  AddNames(App, 'game.iso|disc|manifest.json|game.iso.new|disc.new|manifest.new|game.iso.old|disc.old|' +
           'manifest.json.old|{#InstallMarker}|gcextract.exe|game.toml|BUILD-INFO.txt|unins000.exe|unins000.dat|' +
           'unins000.msg|', Names);
  for I := 0 to GetArrayLength(Names) - 1 do
    RemovePath(Names[I]);
  if RemoveDir(App) then
    Log('Removed the new installation folder: ' + App)
  else
    Log('The installation folder is not empty, kept: ' + App);
  { CreatedDirs[0] is App itself (when this run created it). }
  for I := 1 to GetArrayLength(CreatedDirs) - 1 do
    if RemoveDir(CreatedDirs[I]) then
      Log('Removed the new parent folder: ' + CreatedDirs[I]);
end;

{ The folders that ForceDirectories(Dir) creates, deepest first (Dir itself first, if missing), in List; returns the
  deepest folder of Dir's path that exists (for the free space check), '' if none. }
function MissingFolders(const Dir: String; var List: TArrayOfString): String;
var
  D, Parent: String;
  N: Integer;
begin
  SetArrayLength(List, 0);
  Result := '';
  D := Dir;
  while D <> '' do begin
    if DirExists(D) then begin
      Result := D;
      Exit;
    end;
    N := GetArrayLength(List);
    SetArrayLength(List, N + 1);
    List[N] := D;
    Parent := ExtractFileDir(D);
    if (Parent = D) or (Parent = '') then
      Exit;
    D := Parent;
  end;
end;

{ '' if there is enough free disk space for this installation, else the message (Setup stops before anything is
  written). Needed: the new copy of the image (none when the installed copy is reused), the payload, and a margin for
  disc\sys\ and the uninstaller. The previous image is kept until the new one is in place (and the previous payload
  files as <name>.old), so an update needs as much as a first install. }
function SpaceProblem(const App: String): String;
var
  Folders: TArrayOfString;
  Existing: String;
  Need, Size, Free, Total: Int64;
begin
  Result := '';
  Need := {#PayloadBytes};
  Need := Need + 64 * 1024 * 1024;
  if ImageNeedsCopy then begin
    if not FileSize64(GetImagePath(''), Size) then
      Size := {#ImageSize};
    Need := Need + Size;
  end;
  Existing := MissingFolders(App, Folders);
  if TestFailure('space') then begin
    Free := 0;
    Log('Free disk space reported as 0 (/TESTFAIL=space)');
  end else if (Existing = '') or not GetSpaceOnDisk64(Existing, Free, Total) then begin
    Log('Free disk space unknown for ' + App + ': not checked');
    Exit;
  end;
  Log('Disk space: ' + IntToStr(Need div (1024 * 1024)) + ' MB needed, ' + IntToStr(Free div (1024 * 1024)) +
      ' MB free');
  if Free < Need then
    Result := 'There is not enough free disk space in "' + App + '": ' + IntToStr(Need div (1024 * 1024)) +
      ' MB are needed, ' + IntToStr(Free div (1024 * 1024)) + ' MB are free. Nothing was changed.' + #13#10#13#10 +
      'When an installation is updated, its previous disc image is kept until the new copy is in place, so an update ' +
      'needs as much free space as a first installation. Free some space, or choose another drive.';
end;

{ Runs gcextract with the progress shown on CheckPage (wizard pages before the installation). }
function RunGcextractWithPage(const Exe, Params, Text: String): Integer;
begin
  CheckPage.SetText(Text, GetImagePath(''));
  CheckPage.SetProgress(0, 100);
  CheckPage.Show;
  try
    Result := RunGcextract(Exe, Params);
  finally
    CheckPage.Hide;
  end;
end;

{ PrepareToInstall, before anything is installed: writes the marker, then extracts sys\ from the chosen image (the
  player's file, or the copy already installed) into disc.new\ and manifest.new. The image is hashed again by
  gcextract. '' on success, else the message shown by Setup, which then stops (exit code 7); DeinitializeSetup
  removes the temporary files (and the marker and folder of a first install). }
function PrepareSystemFiles: String;
var
  App, Params: String;
  Code: Integer;
begin
  Result := '';
  App := AppDir;
  { Recorded before they are created: a first install that does not complete removes them again. }
  MissingFolders(App, CreatedDirs);
  PreparedDir := True;
  PreparedApp := App;
  if not ForceDirectories(App) then begin
    Result := 'The folder "' + App + '" could not be created.';
    Exit;
  end;
  { From here on the folder belongs to this installer: a later run may update, repair or remove it. }
  { Written only if missing: a failed reinstall leaves the folder as it was (the installed version is in
    BUILD-INFO.txt, which a failed reinstall puts back). }
  { Nested ifs: no reliance on short-circuit evaluation. }
  if not IsOurInstall(App) then
    if not SaveStringToFile(App + '\{#InstallMarker}',
        'Installed by the {#AppName} installer, AppId {#AppId}.' + #13#10 +
        'Running the installer again over this folder updates this installation.' + #13#10, False) then begin
      Result := 'The folder "' + App + '" is not writable. ' + RefusalText(7);
      Exit;
    end;
  if not CleanUpTemporaryFiles(App) then begin
    Result := 'A previous file of the installation in "' + App + '" could not be put back (it may be open in another ' +
      'program). Nothing was changed: close the game and run Setup again.';
    Exit;
  end;
  ExtractTemporaryFile('gcextract.exe');
  ExtractTemporaryFile('game.toml');
  Params := AddQuotes(GetImagePath('')) + ' ' + AddQuotes(App + '\disc.new') + ' --system-only --game-toml ' +
            AddQuotes(ExpandConstant('{tmp}\game.toml')) + ' --manifest ' + AddQuotes(App + '\manifest.new');
  Code := RunGcextractWithPage(ExpandConstant('{tmp}\gcextract.exe'), Params, 'Extracting the system files...');
  if (Code = 0) and TestFailure('extract') then begin
    GcxError := 'simulated failure (/TESTFAIL=extract)';
    Code := 7;
  end;
  if (Code = 0) and (GcxMatch = GameId) and not MakeManifestRelative(App + '\manifest.new') then begin
    GcxError := 'manifest.json could not be updated';
    Code := 7;
  end;
  if (Code = 0) and (GcxMatch <> GameId) then begin
    GcxError := 'the image is now ' + GcxMatch + ', expected ' + GameId;
    Code := 6;
  end;
  if (Code = 0) and TestFailure('prepare') then
    RaiseException('simulated unexpected error in PrepareToInstall (/TESTFAIL=prepare)');
  if Code <> 0 then begin
    { gcextract never cleans up after itself. }
    RemoveTemporaryFiles(App);
    Log('Extraction failed: ' + GcxError);
    Result := 'The game files could not be prepared. Nothing was changed. ' + RefusalText(Code);
  end;
end;

{ Checks the copy (hashed again by gcextract), then swaps the image files in. Returns 0, or the RefusalText code. }
function CheckCopyAndSwap: Integer;
var
  Code: Integer;
begin
  Code := 0;
  if ImageNeedsCopy then begin
    WizardForm.StatusLabel.Caption := 'Checking the copy of the disc image...';
    GcxInstalling := True;
    try
      Code := RunGcextract(AppDir + '\gcextract.exe', AddQuotes(InstalledImage + '.new') + ' --verify-only --game-toml ' +
                           AddQuotes(AppDir + '\game.toml'));
    finally
      GcxInstalling := False;
    end;
    if (Code = 0) and (GcxMatch <> GameId) then begin
      GcxError := 'the copy is ' + GcxMatch + ', expected ' + GameId;
      Code := 6;
    end;
  end;
  if (Code = 0) and TestFailure('copy') then begin
    GcxError := 'simulated failure (/TESTFAIL=copy)';
    Code := 7;
  end;
  if (Code = 0) and TestFailure('missing') then begin
    Log('Deleting manifest.new (/TESTFAIL=missing)');
    DeleteFile(AppDir + '\manifest.new');
  end;
  if (Code = 0) and not SwapInNewFiles then begin
    GcxError := 'the new files could not replace the previous ones (a file may be in use)';
    Code := 7;
  end;
  if (Code = 0) and TestFailure('raise') then
    RaiseException('simulated unexpected error after the swap (/TESTFAIL=raise)');
  Result := Code;
end;

{ AfterInstall of the entry that follows the image copy. On failure, or on any exception (Inno Setup would report it
  and go on), the previous files are put back (image files, and the payload files installed so far: gcextract.exe,
  game.toml, BUILD-INFO.txt), the temporary ones removed, the rest of the payload is skipped (Check: not
  InstallFailed), Setup exits with code 9, and a first install is removed again (DeinitializeSetup). }
procedure FinishImageAndExtraction;
var
  Code: Integer;
  Restored: Boolean;
  Text: String;
begin
  try
    Code := CheckCopyAndSwap;
  except
    GcxError := GetExceptionMessage;
    Code := UnexpectedError;
  end;
  if Code = 0 then
    Exit;
  FailedFlag := True;
  Restored := False;
  try
    { Both are always run (no short-circuit): each puts back what it can. }
    Restored := UndoSwap(AppDir);
    if not RestoreBackups then
      Restored := False;
    RemoveTemporaryFiles(AppDir);
  except
    Restored := False;
    Log('Clean-up after the failure: ' + GetExceptionMessage);
  end;
  if Restored then
    Log('The previous files are back')
  else
    Log('Some previous files could not be put back');
  Log('Installation failed: ' + GcxError);
  if DirWasNew then
    Text := 'The game could not be installed; nothing is left in the installation folder. '
  else if Restored then
    Text := 'The game could not be updated. The previous game files were kept. '
  else
    Text := 'The game could not be updated, and some previous files could not be put back: run Setup again. ';
  SuppressibleMsgBox(Text + RefusalText(Code), mbError, MB_OK, IDOK);
end;

function GetCustomSetupExitCode: Integer;
begin
  if FailedFlag then
    Result := 9
  else
    Result := 0;
end;

procedure InitializeWizard;
begin
  ImagePage := CreateInputFilePage(wpSelectDir, 'Disc image',
    'Choose the disc image of your own copy of {#GameName}.',
    'This installer contains no game data. Choose an unmodified dump of your own disc (.iso or .gcm). ' +
    'It is checked, then copied into the installation folder.' + #13#10#13#10 +
    'Supported: {#SupportedList}.' + #13#10 + 'Game executable: {#ExeCommits}.');
  ImagePage.Add('Disc image:', 'GameCube disc images (*.iso;*.gcm)|*.iso;*.gcm|All files (*.*)|*.*', '.iso');
  ImagePage.Values[0] := ExpandConstant('{param:IMAGE|}');
  CheckPage := CreateOutputProgressPage('Disc image', 'Checking that the disc image is a supported, unmodified copy.');
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  { Reinstall / update: offer the copy already installed. }
  if (CurPageID = ImagePage.ID) and (Trim(ImagePage.Values[0]) = '') and FileExists(WizardDirValue + '\game.iso') then
    ImagePage.Values[0] := WizardDirValue + '\game.iso';
end;

{ Cancel (button or window close) while gcextract runs: refused, the button is disabled anyway (RunGcextract). At any
  other time: Inno Setup's own handling (before the installation: nothing installed; during it: rollback, then
  DeinitializeSetup). }
procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  if GcxBusy then begin
    Log('Cancel refused while gcextract is running');
    Cancel := False;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    if TestFailure('dirpage') then
      Log('Folder page check skipped (/TESTFAIL=dirpage)')
    else if DestinationProblem(WizardDirValue) <> '' then begin
      Log('Refused installation folder: ' + WizardDirValue);
      SuppressibleMsgBox(DestinationProblem(WizardDirValue), mbError, MB_OK, IDOK);
      Result := False;
    end;
  end else if CurPageID = ImagePage.ID then
    Result := VerifyImage;
end;

{ Before anything is installed: the folder is checked again (the folder page is skipped when a previous installation
  is found), then the system files are extracted (see PrepareSystemFiles). A non-empty result stops Setup. }
{ What an earlier, failed call of PrepareToInstall wrote (the wizard can show the Preparing page again, in another
  folder too) is removed, so that each call starts from the folder as it was: temporary files, and for a first
  install the folder, its marker and the parent folders created. }
procedure AbandonPrepared;
begin
  if not PreparedDir then
    Exit;
  try
    RemoveTemporaryFiles(PreparedApp);
    if DirWasNew then
      RemoveFirstInstall(PreparedApp);
  except
    Log('Clean-up after the failed preparation: ' + GetExceptionMessage);
  end;
  PreparedDir := False;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := DestinationProblem(AppDir);
  if Result <> '' then begin
    Log('Refused installation folder: ' + AppDir);
    Exit;
  end;
  { Only for the first preparation of this folder: once this run has written its marker, the folder is no longer
    empty, but it still was before this run (AbandonPrepared resets this after a failure). }
  if not PreparedDir then
    DirWasNew := IsDirEmptyOrMissing(AppDir);
  Result := SpaceProblem(AppDir);
  if Result <> '' then begin
    Log('Refused: ' + Result);
    Exit;
  end;
  try
    Result := PrepareSystemFiles;
  except
    { Inno Setup would report the exception and go on with the installation: stop it here instead. }
    Log('PrepareToInstall: ' + GetExceptionMessage);
    { No line may start with "#" here: ISPP would read it as a directive. }
    Result := 'The game files could not be prepared. Nothing was changed. ' + RefusalText(UnexpectedError) + #13#10#13#10 +
      'Details: ' + GetExceptionMessage;
  end;
  if Result <> '' then
    AbandonPrepared;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    InstallDone := True;
    { The installation is complete (past Inno Setup's rollback): the previous image and payload files are no longer
      needed. }
    if not FailedFlag then begin
      if SwapDone then
        RemoveOldFiles(AppDir);
      RemoveBackups;
    end;
  end;
end;

{ Called just before Setup terminates, whatever happened. After a cancel or an error during the installation (Inno
  Setup has rolled back the files it installed), the previous image files are put back and the temporary ones
  removed; a first install that did not complete (that, or exit code 7 or 9) is removed. }
procedure DeinitializeSetup;
begin
  if not PreparedDir then
    Exit;
  try
    if not InstallDone then begin
      if SwapDone then begin
        Log('Setup did not complete after the swap: putting the previous files back');
        if not UndoSwap(PreparedApp) then
          Log('Some previous files could not be put back (the next run of Setup restores them)');
      end;
      { After Inno Setup's rollback, which has deleted the payload files it installed under names that were free
        (BackUpExisting had renamed the previous ones). }
      if GetArrayLength(Backups) > 0 then begin
        Log('Setup did not complete: putting the previous payload files back');
        if not RestoreBackups then
          Log('Some previous payload files could not be put back (the next run of Setup restores them)');
      end;
      RemoveTemporaryFiles(PreparedApp);
    end;
    if DirWasNew and (FailedFlag or not InstallDone) then
      RemoveFirstInstall(PreparedApp);
  except
    Log('Clean-up at exit: ' + GetExceptionMessage);
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine + 'Disc image:' + NewLine + Space + GetImagePath('') + ' (' + GameId + ')';
  if not ImageNeedsCopy then
    Result := Result + NewLine + Space + '(already installed, not copied again)';
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + NewLine + MemoTasksInfo;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then begin
    { PrepareToInstall creates the folder before Inno Setup does, so it is not in the uninstall log: remove it here
      (RemoveDir only removes an empty folder). }
    RemoveDir(ExpandConstant('{app}'));
    SuppressibleMsgBox('{#AppName} was removed. Your saves and settings were kept in:' + #13#10 +
      ExpandConstant('{#UserDataDir}'), mbInformation, MB_OK, IDOK);
  end;
end;
