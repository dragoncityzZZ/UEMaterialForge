; MaterialForge Inno Setup 6 script
; Build: ISCC.exe installer\MaterialForge.iss
; Payload: build\installer-payload (staged by scripts\build-installer.ps1)

#define MyAppName "MaterialForge"
#define MyAppVersion "2.5.0"
#define MyAppPublisher "MaterialForge"
#define MyAppExeName "MaterialForge.exe"
#define PayloadDir "..\build\installer-payload"

[Setup]
AppId={{9C0C0E52-5F6E-4A97-9E5B-8A57D2B7C0F1}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\MaterialForge
DefaultGroupName=MaterialForge
DisableProgramGroupPage=yes
OutputDir=..\dist\installer
OutputBaseFilename=MaterialForge-{#MyAppVersion}-Setup
SetupIconFile=..\resources\MaterialForge.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
Uninstallable=yes
CreateUninstallRegKey=yes
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
VersionInfoVersion=2.5.0.0
VersionInfoDescription=MaterialForge UE5 Material Editor
VersionInfoCompany={#MyAppPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "associate"; Description: "Associate .mfproject files with MaterialForge"; GroupDescription: "File associations:"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\MaterialForge"; Filename: "{app}\{#MyAppExeName}"
Name: "{autoprograms}\Uninstall MaterialForge"; Filename: "{uninstallexe}"
Name: "{autodesktop}\MaterialForge"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.mfproject"; ValueType: string; ValueName: ""; ValueData: "MaterialForge.Project"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\MaterialForge.Project"; ValueType: string; ValueName: ""; ValueData: "MaterialForge Project"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\MaterialForge.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\MaterialForge.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Flags: uninsdeletekey; Tasks: associate

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch MaterialForge"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\.materialforge"
