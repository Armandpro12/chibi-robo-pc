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
; /TESTFAIL=extract or /TESTFAIL=copy (only with /TESTMODE=1, no effect otherwise): the extraction (before anything is
; installed), or the check of the copied image (during the installation), is reported as failed after gcextract has
; run, to test that a failed (re)install keeps the previous installation and leaves no temporary file.
;
; Safety rules:
; - a first install only goes into a new or empty folder: a folder that holds files and was not installed by this
;   installer (no InstallMarker file with this AppId) is refused, so that nothing of the player's is overwritten now
;   or deleted by the uninstaller later (it removes fixed names: game.iso, disc\, manifest.json, gcrt-data\);
; - the extraction (disc.new\, manifest.new) runs in PrepareToInstall, before anything is installed: a failure there
;   stops Setup (exit code 7) with nothing changed; the image is copied as game.iso.new, checked again, and only then
;   are game.iso, disc\ and manifest.json replaced (renames), before the executables: a failure there keeps the
;   previous files, skips the rest of the payload and makes Setup exit with code 9 (GetCustomSetupExitCode).

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
CreateUninstallRegKey=not IsTestMode

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Tools needed before the installation starts (checking the image on its wizard page): first, so that a solid
; archive does not have to decompress everything else to reach them.
Source: "{#PayloadDir}\common\gcextract.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\common\game.toml"; DestDir: "{app}"; Flags: ignoreversion
; The player's disc image, copied from their disk (not part of the installer), under a temporary name.
Source: "{code:GetImagePath}"; DestDir: "{app}"; DestName: "game.iso.new"; ExternalSize: {#ImageSize}; Flags: external ignoreversion; Check: ImageNeedsCopy
; After the copy, before the executables: check the copy, then put the new image and extraction in place. A failure
; there keeps the previous files and skips the entries below (an exception in AfterInstall does not abort Setup:
; Inno Setup reports it and goes on, seen in test).
Source: "{#PayloadDir}\BUILD-INFO.txt"; DestDir: "{app}"; Flags: ignoreversion; AfterInstall: FinishImageAndExtraction
Source: "{#PayloadDir}\common\*"; Excludes: "gcextract.exe,game.toml"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: not InstallFailed
; Executable(s) and the DLLs that differ between versions: only the files of the version of the chosen image.
#include PayloadDir + "\versions.iss"

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

[Code]
var
  ImagePage: TInputFileWizardPage;
  CheckPage: TOutputProgressWizardPage;
  GameId: String;          { game ID of the accepted image (INFO match), selects the executable }
  GcxError: String;        { message of the last ERROR line }
  GcxMatch: String;        { game ID of the last INFO match line }
  GcxInstalling: Boolean;  { True during the check of the copy (progress on the installing page) }
  FailedFlag: Boolean;  { the copy could not be checked or put in place: the rest of the payload is skipped }
  InstallDone: Boolean;    { ssPostInstall reached }
  PreparedDir: Boolean;    { PrepareToInstall wrote into the folder (temporary files, marker) }
  DirWasNew: Boolean;      { the folder was missing or empty before this run }
  PreparedApp: String;     { the folder PrepareToInstall wrote into (the wizard may be gone in DeinitializeSetup) }

{ Check of the payload entries after the image: skipped once the copy failed. }
function InstallFailed: Boolean;
begin
  Result := FailedFlag;
end;

function IsTestMode: Boolean;
begin
  Result := ExpandConstant('{param:TESTMODE|0}') = '1';
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

function TestFailure(const Stage: String): Boolean;
begin
  Result := IsTestMode and (ExpandConstant('{param:TESTFAIL|}') = Stage);
end;

{ True if Dir does not exist or holds nothing (no file, no folder). }
function IsDirEmptyOrMissing(const Dir: String): Boolean;
var
  Rec: TFindRec;
begin
  Result := True;
  if not DirExists(Dir) then
    Exit;
  if FindFirst(AddBackslash(Dir) + '*', Rec) then
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
end;

{ True if Dir is an installation made by this installer (marker file with this AppId). }
function IsOurInstall(const Dir: String): Boolean;
var
  Text: AnsiString;
begin
  Result := LoadStringFromFile(AddBackslash(Dir) + '{#InstallMarker}', Text) and (Pos('{#AppId}', String(Text)) > 0);
end;

{ '' if Setup may install into Dir, else the reason (see "Safety rules" at the top). }
function DestinationProblem(const Dir: String): String;
begin
  Result := '';
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

function IsAsciiPath(const S: String): Boolean;
var
  I: Integer;
begin
  Result := True;
  for I := 1 to Length(S) do
    if Ord(S[I]) > 126 then
      Result := False;
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

{ Runs gcextract with Params; returns its exit code (-1 if it could not be started). }
function RunGcextract(const Exe, Params: String): Integer;
var
  Code: Integer;
begin
  GcxError := '';
  GcxMatch := '';
  Log('Running: "' + Exe + '" ' + Params);
  try
    if not ExecAndLogOutput(Exe, Params, '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code, @GcxOnLog) then begin
      GcxError := SysErrorMessage(Code);
      Code := -1;
    end;
  except
    GcxError := GetExceptionMessage;
    Code := -1;
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
  WizardForm.CancelButton.Enabled := False;
  try
    Code := RunGcextract(Exe, AddQuotes(Image) + ' --verify-only --game-toml ' + AddQuotes(ExpandConstant('{tmp}\game.toml')));
  finally
    WizardForm.CancelButton.Enabled := True;
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

{ Temporary names of the files replaced at the end of the extraction: Final[I] <- New[I] (Final[I].old meanwhile). }
procedure SwapNames(var Final, New: TArrayOfString);
var
  App: String;
begin
  App := AppDir;
  SetArrayLength(Final, 3);
  SetArrayLength(New, 3);
  Final[0] := App + '\disc';          New[0] := App + '\disc.new';
  Final[1] := App + '\manifest.json'; New[1] := App + '\manifest.new';
  Final[2] := InstalledImage;         New[2] := InstalledImage + '.new';
end;

{ Before anything is installed: finish or undo an interrupted swap of an earlier run, remove its temporary files. }
procedure CleanUpTemporaryFiles;
var
  Final, New: TArrayOfString;
  I: Integer;
begin
  SwapNames(Final, New);
  for I := 0 to GetArrayLength(Final) - 1 do begin
    if not FileOrDirExists(Final[I]) and FileOrDirExists(Final[I] + '.old') then begin
      Log('Restoring ' + Final[I] + ' from an interrupted install');
      RenameFile(Final[I] + '.old', Final[I]);
    end;
    RemovePath(Final[I] + '.old');
    RemovePath(New[I]);
  end;
end;

{ Moves the new files (those that exist) into place; on any failure, puts everything back as it was. }
function SwapInNewFiles: Boolean;
var
  Final, New: TArrayOfString;
  Moved, Placed: array of Boolean;
  I, J, N: Integer;
begin
  Result := False;
  SwapNames(Final, New);
  N := GetArrayLength(Final);
  SetArrayLength(Moved, N);
  SetArrayLength(Placed, N);
  for I := 0 to N - 1 do begin
    Moved[I] := False;   { Final[I] renamed to Final[I].old }
    Placed[I] := False;  { New[I] renamed to Final[I] }
  end;
  Result := True;
  for I := 0 to N - 1 do
    if Result and FileOrDirExists(New[I]) then begin
      if FileOrDirExists(Final[I]) then begin
        Moved[I] := RenameFile(Final[I], Final[I] + '.old');
        if not Moved[I] then begin
          Log('Cannot rename ' + Final[I]);
          Result := False;
        end;
      end;
      if Result then begin
        Placed[I] := RenameFile(New[I], Final[I]);
        if not Placed[I] then begin
          Log('Cannot rename ' + New[I]);
          Result := False;
        end;
      end;
    end;
  if not Result then begin
    for J := 0 to N - 1 do begin
      if Placed[J] then
        RenameFile(Final[J], New[J]);
      if Moved[J] then
        RenameFile(Final[J] + '.old', Final[J]);
    end;
    Exit;
  end;
  for I := 0 to N - 1 do
    RemovePath(Final[I] + '.old');
end;

procedure RemoveTemporaryFiles;
begin
  RemovePath(AppDir + '\disc.new');
  RemovePath(AppDir + '\manifest.new');
  RemovePath(InstalledImage + '.new');
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

{ PrepareToInstall, before anything is installed: extracts sys\ from the chosen image (the player's file, or the copy
  already installed) into disc.new\ and manifest.new. The image is hashed again by gcextract. '' on success, else the
  message shown by Setup, which then stops (exit code 7) with the temporary files removed. }
function PrepareSystemFiles: String;
var
  App, Params: String;
  Code: Integer;
begin
  Result := '';
  App := AppDir;
  if not ForceDirectories(App) then begin
    Result := 'The folder "' + App + '" could not be created.';
    Exit;
  end;
  PreparedDir := True;
  PreparedApp := App;
  CleanUpTemporaryFiles;
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
  if Code <> 0 then begin
    { gcextract never cleans up after itself. }
    RemoveTemporaryFiles;
    Log('Extraction failed: ' + GcxError);
    Result := 'The game files could not be prepared. Nothing was changed. ' + RefusalText(Code);
    Exit;
  end;
  { From here on the folder is an installation of this installer (a later run may update or repair it). }
  if not SaveStringToFile(App + '\{#InstallMarker}',
      'Installed by the {#AppName} installer, AppId {#AppId}, version {#AppVersion}.' + #13#10 +
      'Running the installer again over this folder updates this installation.' + #13#10, False) then
    Log('Could not write the installation marker');
end;

{ AfterInstall of the entry that follows the image copy: checks the copy (hashed again by gcextract), then replaces
  game.iso, disc\ and manifest.json. On failure the previous files stay, the temporary ones are removed, the rest of
  the payload is skipped (Check: not FailedFlag) and Setup exits with code 9. }
procedure FinishImageAndExtraction;
var
  Code: Integer;
  Text: String;
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
  if (Code = 0) and not SwapInNewFiles then begin
    GcxError := 'the new files could not replace the previous ones (a file may be in use)';
    Code := 7;
  end;
  if Code <> 0 then begin
    FailedFlag := True;
    RemoveTemporaryFiles;
    Log('Installation failed: ' + GcxError);
    Text := 'The game files could not be installed. The previous files (if any) were kept. ' + RefusalText(Code);
    SuppressibleMsgBox(Text, mbError, MB_OK, IDOK);
  end;
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

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    { The game opens its files through the ANSI code page: keep the installation path ASCII. }
    if not IsAsciiPath(WizardDirValue) then begin
      SuppressibleMsgBox('Choose an installation folder whose path contains only ASCII characters (A-Z, 0-9, spaces and basic punctuation).',
        mbError, MB_OK, IDOK);
      Result := False;
    end else if DestinationProblem(WizardDirValue) <> '' then begin
      Log('Refused installation folder: ' + WizardDirValue);
      SuppressibleMsgBox(DestinationProblem(WizardDirValue), mbError, MB_OK, IDOK);
      Result := False;
    end;
  end else if CurPageID = ImagePage.ID then
    Result := VerifyImage;
end;

{ Before anything is installed: the folder is checked again (the folder page is skipped when a previous installation
  is found), then the system files are extracted (see PrepareSystemFiles). A non-empty result stops Setup. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := DestinationProblem(AppDir);
  if Result <> '' then begin
    Log('Refused installation folder: ' + AppDir);
    Exit;
  end;
  DirWasNew := IsDirEmptyOrMissing(AppDir);
  Result := PrepareSystemFiles;
  if (Result <> '') and DirWasNew then begin
    DeleteFile(AppDir + '\{#InstallMarker}');
    RemoveDir(AppDir);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    InstallDone := True;
end;

{ Setup cancelled or failed after PrepareToInstall (rollback of the [Files] entries): remove what the code wrote. }
procedure DeinitializeSetup;
begin
  if PreparedDir and not InstallDone then begin
    RemovePath(PreparedApp + '\disc.new');
    RemovePath(PreparedApp + '\manifest.new');
    RemovePath(PreparedApp + '\game.iso.new');
    if DirWasNew then begin
      DeleteFile(PreparedApp + '\{#InstallMarker}');
      RemoveDir(PreparedApp);
    end;
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
