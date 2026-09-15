#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\release\RelWithDebInfo\gd-program-draw"
#endif
#ifndef OutputDir
  #define OutputDir "..\release"
#endif

[Setup]
AppName=GD Program Draw
AppVersion={#AppVersion}
AppVerName=GD Program Draw {#AppVersion}
AppPublisher=Ron Planken
AppPublisherURL=https://github.com/ronplanken/gd-program-draw
AppSupportURL=https://github.com/ronplanken/gd-program-draw/issues
AppUpdatesURL=https://github.com/ronplanken/gd-program-draw/releases
AppCopyright=Copyright (C) Ron Planken
VersionInfoCompany=Ron Planken
VersionInfoDescription=GD Program Draw Setup
VersionInfoProductName=GD Program Draw
VersionInfoVersion={#AppVersion}
VersionInfoCopyright=Copyright (C) Ron Planken
DefaultDirName={commonappdata}\obs-studio\plugins\gd-program-draw
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=gd-program-draw-{#AppVersion}-windows-x64-installer
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayName=GD Program Draw
UninstallFilesDir={app}\uninstall
WizardStyle=modern

[Files]
Source: "{#SourceDir}\bin\64bit\*"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion recursesubdirs
Source: "{#SourceDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs

[Messages]
WelcomeLabel2=This installs [name/ver] into the OBS Studio plugin folder for all users.%n%nClose OBS Studio before continuing.
