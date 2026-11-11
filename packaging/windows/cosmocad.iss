; Установщик CosmoCad для Windows (Inno Setup 6).
; Собирается в .github/workflows/installer.yml из папки stage, куда уже сложены программа,
; её библиотеки и данные. Версию и папки передаёт сборка: /DAppVersion=... /DStageDir=... /DOutputDir=...

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\out"
#endif

[Setup]
AppId={{6B0E0C1B-6C1D-4B6C-9A4E-3C0C5D0A7F21}
AppName=CosmoCad
AppVersion={#AppVersion}
AppPublisher=CosmoCad
DefaultDirName={autopf}\CosmoCad
DefaultGroupName=CosmoCad
LicenseFile={#StageDir}\LICENSE.txt
OutputDir={#OutputDir}
OutputBaseFilename=CosmoCad-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\bin\cosmocad.exe

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\CosmoCad"; Filename: "{app}\bin\cosmocad.exe"; WorkingDir: "{app}\bin"
Name: "{group}\{cm:UninstallProgram,CosmoCad}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\CosmoCad"; Filename: "{app}\bin\cosmocad.exe"; WorkingDir: "{app}\bin"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\cosmocad.exe"; Description: "{cm:LaunchProgram,CosmoCad}"; Flags: nowait postinstall skipifsilent
