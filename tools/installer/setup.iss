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

[InstallDelete]
; Files written by gcextract are not in the uninstall log: remove the previous extraction before a new one
; (reinstall / update), gcextract never overwrites.
Type: filesandordirs; Name: "{app}\disc"
Type: files; Name: "{app}\manifest.json"

[Files]
; Tools needed before the installation starts (checking the image on its wizard page): first, so that a solid
; archive does not have to decompress everything else to reach them.
Source: "{#PayloadDir}\common\gcextract.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\common\game.toml"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\common\*"; Excludes: "gcextract.exe,game.toml"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs
; Executable(s) and the DLLs that differ between versions: only the files of the version of the chosen image.
#include PayloadDir + "\versions.iss"
; The player's disc image, copied from their disk (not part of the installer).
Source: "{code:GetImagePath}"; DestDir: "{app}"; DestName: "game.iso"; ExternalSize: {#ImageSize}; Flags: external ignoreversion; Check: ImageNeedsCopy
; Last entry: the extraction runs once everything above is in place (an exception there aborts and rolls back).
Source: "{#PayloadDir}\BUILD-INFO.txt"; DestDir: "{app}"; Flags: ignoreversion; AfterInstall: ExtractSystemFiles

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#ExeName}"; Parameters: "{#DiscArg}"; WorkingDir: "{app}"; Check: not IsTestMode
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#ExeName}"; Parameters: "{#DiscArg}"; WorkingDir: "{app}"; Tasks: desktopicon; Check: not IsTestMode

[Run]
Filename: "{app}\{#ExeName}"; Parameters: "{#DiscArg}"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Created after the installation (extraction, GPU caches of the game): removed with the game. The user folder
; (saves, settings, crash reports) is elsewhere and is never removed.
Type: filesandordirs; Name: "{app}\disc"
Type: files; Name: "{app}\manifest.json"
Type: filesandordirs; Name: "{app}\gcrt-data"

[Code]
var
  ImagePage: TInputFileWizardPage;
  CheckPage: TOutputProgressWizardPage;
  GameId: String;          { game ID of the accepted image (INFO match), selects the executable }
  GcxError: String;        { message of the last ERROR line }
  GcxMatch: String;        { game ID of the last INFO match line }
  GcxInstalling: Boolean;  { True during the extraction (progress on the installing page) }

function IsTestMode: Boolean;
begin
  Result := ExpandConstant('{param:TESTMODE|0}') = '1';
end;

function GetImagePath(Param: String): String;
begin
  Result := ImagePage.Values[0];
end;

function InstalledImage: String;
begin
  Result := ExpandConstant('{app}\game.iso');
end;

function ImageNeedsCopy: Boolean;
begin
  { Reinstalling from the copy already in the install folder: nothing to copy. }
  Result := CompareText(ExpandFileName(GetImagePath('')), ExpandFileName(InstalledImage)) <> 0;
end;

function IsGameVersion(Id: String): Boolean;
begin
  Result := GameId = Id;
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

{ gcextract writes the absolute path of the image; the installed copy must keep working if the folder is moved:
  "image" becomes "game.iso", relative to the manifest's folder (= the working directory of the shortcuts, so
  executables that open it relative to the current directory work too). }
function MakeManifestImageRelative(const Manifest: String): Boolean;
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
    end;
  Result := Found and SaveStringsToUTF8FileWithoutBOM(Manifest, Lines, False);
end;

{ AfterInstall of the last [Files] entry: extracts sys\ from the installed copy and writes manifest.json. The copy is
  hashed again by gcextract, so a damaged copy is caught here. On failure: clean up and abort (rollback). }
procedure ExtractSystemFiles;
var
  App, Params: String;
  Code: Integer;
begin
  App := ExpandConstant('{app}');
  if DirExists(App + '\disc') then
    DelTree(App + '\disc', True, True, True);
  DeleteFile(App + '\manifest.json');
  Params := AddQuotes(InstalledImage) + ' ' + AddQuotes(App + '\disc') + ' --system-only --game-toml ' +
            AddQuotes(App + '\game.toml') + ' --manifest ' + AddQuotes(App + '\manifest.json');
  WizardForm.StatusLabel.Caption := 'Checking the disc image...';
  GcxInstalling := True;
  try
    Code := RunGcextract(App + '\gcextract.exe', Params);
  finally
    GcxInstalling := False;
  end;
  if (Code = 0) and (GcxMatch = GameId) and not MakeManifestImageRelative(App + '\manifest.json') then begin
    GcxError := 'manifest.json could not be updated';
    Code := 7;
  end;
  if (Code <> 0) or (GcxMatch <> GameId) then begin
    { gcextract never cleans up after itself: remove the partial output before reporting. }
    DelTree(App + '\disc', True, True, True);
    DeleteFile(App + '\manifest.json');
    if (Code = 0) then
      GcxError := 'the installed copy is ' + GcxMatch + ', expected ' + GameId;
    RaiseException('The game files could not be prepared. ' + RefusalText(Code));
  end;
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
    end;
  end else if CurPageID = ImagePage.ID then
    Result := VerifyImage;
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
  if CurUninstallStep = usPostUninstall then
    SuppressibleMsgBox('{#AppName} was removed. Your saves and settings were kept in:' + #13#10 +
      ExpandConstant('{#UserDataDir}'), mbInformation, MB_OK, IDOK);
end;
