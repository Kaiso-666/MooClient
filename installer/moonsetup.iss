; MooClient installer (Inno Setup 6).
;
; Per-user by design: the app updates itself by running this installer
; silently from inside the app, which cannot raise a UAC prompt. Everything
; therefore lives under %LOCALAPPDATA%\Programs\MooClient and the uninstall
; entry goes to HKCU, so no administrator rights are ever needed.

#define AppName "MooClient"
#define AppExe "MooClient.exe"
#define AppPublisher "MooClient"
#define AppRepo "Kaiso-666/MooClient"
#define AppUrl "https://github.com/" + AppRepo

; The exe is the single source of truth for the version (res/version.h.in ->
; build/generated/version.h -> VERSIONINFO).
#define AppVersion GetStringFileInfo(SourcePath + "..\build\bin\" + AppExe, "ProductVersion")
#if AppVersion == ""
  #error MooClient.exe is missing or has no version resource. Build first.
#endif

[Setup]
AppId={{6F1B7A52-9C3E-4A4B-9F1D-2E5A7C8B0D31}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} Setup
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}

DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UsePreviousAppDir=yes

; Never elevate. The updater runs this file with /SILENT from a non-elevated
; process; an admin prompt there would look like a failure to the user.
PrivilegesRequired=lowest

OutputDir=..\dist
OutputBaseFilename=MooClientSetup-{#AppVersion}
SetupIconFile=..\res\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Lets a silent update replace the running app: Setup sees the mutex, asks
; (or obeys /CLOSEAPPLICATIONS) and waits for the process to exit.
CloseApplications=yes
RestartApplications=no
AppMutex=Local\MooClient.SingleInstance

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
Source: "..\build\bin\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "moomoo.io desktop client"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "moomoo.io desktop client"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Regenerable data only. The WebView2 profile holds the login session, so it
; survives an uninstall/reinstall cycle on purpose.
Type: filesandordirs; Name: "{localappdata}\MooClient\cache"
Type: filesandordirs; Name: "{localappdata}\MooClient\logs"

[Code]
const
  WebView2Key = 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';

function WebView2Installed(): Boolean;
var
  Value: String;
begin
  Result :=
    RegQueryStringValue(HKLM, WebView2Key, 'pv', Value) or
    RegQueryStringValue(HKLM64, WebView2Key, 'pv', Value) or
    RegQueryStringValue(HKCU, WebView2Key, 'pv', Value);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if WebView2Installed() then
    Exit;

  case MsgBox('MooClient needs the Microsoft Edge WebView2 runtime, which was not found ' +
              'on this computer.' + #13#10#13#10 +
              'Windows Update usually installs it automatically. Continue anyway?',
              mbConfirmation, MB_YESNO) of
    IDYES: Result := True;
  else
    Result := False;
  end;
end;
