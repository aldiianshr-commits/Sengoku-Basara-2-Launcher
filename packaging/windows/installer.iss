; Inno Setup script for Sengoku Basara 2 Heroes Recompiled.
; Build: iscc /DAppVersion=0.2.0 /DSourceDir=..\..\dist\SB2HeroesRecompiled installer.iss
#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\SB2HeroesRecompiled"
#endif

[Setup]
AppId={{6C1F3E2A-5B7D-4E39-9A51-2B8E5D7C1F44}
AppName=Sengoku Basara 2 Heroes Recompiled
AppVersion={#AppVersion}
AppPublisher=SB2H Recompiled project
DefaultDirName={autopf}\SB2HeroesRecompiled
DefaultGroupName=SB2 Heroes Recompiled
DisableProgramGroupPage=yes
OutputDir=..\..\dist
OutputBaseFilename=SB2HeroesRecompiled-{#AppVersion}-win64-setup
SetupIconFile=app.ico
UninstallDisplayIcon={app}\SB2Launcher.exe
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\SB2 Heroes Recompiled"; Filename: "{app}\SB2Launcher.exe"
Name: "{group}\{cm:UninstallProgram,SB2 Heroes Recompiled}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\SB2 Heroes Recompiled"; Filename: "{app}\SB2Launcher.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\SB2Launcher.exe"; Description: "{cm:LaunchProgram,SB2 Heroes Recompiled}"; Flags: nowait postinstall skipifsilent
