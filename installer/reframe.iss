; Inno Setup script for reframe++.
;
; Build it with:
;   cmake --build --preset vs2022 --target installer
; or directly:
;   "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" installer\reframe.iss
;
; The application is a single self-contained executable - icons and fonts are
; compiled into it - so the installer's whole job is placement, shortcuts,
; optional autostart and a clean uninstall.

#define AppName        "reframe++"
#define AppVersion     "1.0.0"
#define AppPublisher   "shadowyohan"
#define AppExe         "Reframe.exe"

#ifndef BuildDir
  #define BuildDir "..\build\vs2022\bin"
#endif

[Setup]
AppId={{7B3C1F42-9E4D-4A61-B0E5-6D2A8F31C7A4}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
OutputDir=..\build\installer
OutputBaseFilename=reframe++-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin

; 64-bit only: the capture and encode paths are x64 D3D11/NVENC.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Windows.Graphics.Capture, DirectComposition and the hardware encoders this
; depends on need Windows 10 1809 or newer.
MinVersion=10.0.17763

; The application holds this mutex while running (see wWinMain). Inno uses it
; to notice a running copy and ask the user to close it, instead of failing to
; replace a locked executable halfway through.
AppMutex=Local\ReframePlusPlusSingleton

LicenseFile=..\LICENSE

; Setup.exe's own icon, and the icon Windows shows for the entry in
; "Apps & features". The shortcuts take theirs from the executable, which now
; carries the same file as a resource (see apps/shell/app.rc).
SetupIconFile=..\icon.ico

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
; Spelled out rather than {cm:CreateDesktopIcon} so the wording reads the same
; whichever language the wizard is in, and checked by default: the app is
; normally driven by hotkeys, but the first launch still needs somewhere
; obvious to click.
Name: "desktopicon"; Description: "Создать ярлык на рабочем столе"; \
    GroupDescription: "Ярлыки:"
Name: "autostart"; Description: "Запускать вместе с Windows"; \
    GroupDescription: "Дополнительно:"

[Files]
Source: "{#BuildDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
; The game-capture hook. It has to be a real file for a game to LoadLibrary it,
; so this one thing lives beside the executable rather than inside it, and must
; stay in the same folder - GameCapture resolves it from the running module.
Source: "{#BuildDir}\reframe-hook64.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
; Inno has no constant for the Videos folder, and the app's default output is
; %USERPROFILE%\Videos\Reframe.
Name: "{group}\Папка с записями"; Filename: "{%USERPROFILE}\Videos\Reframe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Per-user autostart. HKCU rather than the machine-wide key on purpose: a
; recorder belongs to the person who installed it, and this needs no rights to
; remove later.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExe}"""; \
    Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; \
    Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The replay spool can be gigabytes; leaving it behind after an uninstall
; would be rude. Settings and logs go too - they are worthless without the app.
Type: filesandordirs; Name: "{localappdata}\Reframe\temp"
Type: filesandordirs; Name: "{localappdata}\Reframe\logs"

[Code]
// Recordings are the user's own files and are never touched automatically -
// only offered, and only if the folder actually has something in it.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  VideoDir: String;
  FindRec: TFindRec;
  HasFiles: Boolean;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;

  VideoDir := ExpandConstant('{%USERPROFILE}\Videos\Reframe');
  HasFiles := False;
  if FindFirst(VideoDir + '\*.mp4', FindRec) then
  begin
    HasFiles := True;
    FindClose(FindRec);
  end;

  if not HasFiles then
    Exit;

  if MsgBox('Удалить сохранённые записи из ' + VideoDir + '?' + #13#10 +
            'Файлы будут удалены безвозвратно.',
            mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    DelTree(VideoDir, True, True, True);
end;
