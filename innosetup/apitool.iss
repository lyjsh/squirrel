; Inno Setup 6 — 64 位安装包，打包 apitool.exe
; 若链接了 OpenSSL 动态库，请将对应 libssl / libcrypto DLL 一并加入 [Files]。
; 先编译 Release：build\Release\apitool.exe
; 再在本文件上右键「Compile」或用 ISCC.exe 编译本脚本。
;
; 若 exe 路径不同，请修改下面 Source 行。

#define MyAppName "apitool"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "apitool"
#define MyAppExeName "apitool.exe"

[Setup]
AppId={{B8A3E2F1-4C5D-4E6F-9A0B-1C2D3E4F5A6B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=apitool-{#MyAppVersion}-win64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
PrivilegesRequired=lowest

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\Release\apitool.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
