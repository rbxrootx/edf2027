#define RepoRoot GetEnv("EDF2027_PRIVATE_REPO_ROOT")
#define GameRoot GetEnv("EDF2027_PRIVATE_GAME_ROOT")
#define BuildBin GetEnv("EDF2027_PRIVATE_BIN_ROOT")
#define PayloadData GetEnv("EDF2027_PRIVATE_DATA_ROOT")
#define PrivateRoot GetEnv("EDF2027_PRIVATE_SOURCE_ROOT")
#define PackageRoot GetEnv("EDF2027_PRIVATE_PACKAGE_ROOT")
#define AnalysisExcludes "*.id0,*.id1,*.nam,*.til,edf_menu_init_report.txt"
#define PrivatePayloadExcludes "*.log,*.dmp,identity.edfkey"

#if RepoRoot == ""
  #error Set EDF2027_PRIVATE_REPO_ROOT before compiling this private installer.
#endif
#if GameRoot == ""
  #error Set EDF2027_PRIVATE_GAME_ROOT before compiling this private installer.
#endif
#if BuildBin == ""
  #error Set EDF2027_PRIVATE_BIN_ROOT before compiling this private installer.
#endif
#if PayloadData == ""
  #error Set EDF2027_PRIVATE_DATA_ROOT before compiling this private installer.
#endif
#if PrivateRoot == ""
  #error Set EDF2027_PRIVATE_SOURCE_ROOT before compiling this private installer.
#endif
#if PackageRoot == ""
  #error Set EDF2027_PRIVATE_PACKAGE_ROOT before compiling this private installer.
#endif

[Setup]
AppId={{D35F1D4F-6BE3-4B73-ACCE-B92412B5C4C8}
AppName=EDF2027 Private Test
AppVersion=0.3.38-beta
AppVerName=EDF2027 Private Test 0.3.38-beta
AppPublisher=Private personal test build
UninstallDisplayName=EDF2027 Private Test
DefaultDirName={localappdata}\Programs\EDF2027 Private Test
DefaultGroupName=EDF2027 Private Test
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
UsePreviousAppDir=no
UsePreviousGroup=no
DisableProgramGroupPage=yes
WizardStyle=modern dynamic
WizardImageFile={#PrivateRoot}\assets\wizard-gameplay.png
WizardImageBackColor=#091311
WizardSmallImageFile={#PrivateRoot}\assets\wizard-menu.png
WizardSmallImageBackColor=#091311
WizardImageOpacity=255
WizardSizePercent=100
InfoAfterFile={#PrivateRoot}\quick-start.txt
OutputDir={#PackageRoot}
OutputBaseFilename=EDF2027-private-test-0.3.38-beta
Compression=lzma2/fast
SolidCompression=yes
Uninstallable=yes
VersionInfoVersion=0.3.38.0
VersionInfoProductName=EDF2027 Private Test
VersionInfoDescription=Private personal EDF2027 test installer

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Dirs]
Name: "{app}\server-data\scripts"

[Files]
; These are the client, both standalone servers, the guest-backed host, controller mappings, the app-local
; D3D compiler, and runtime DLLs gathered by build-private.ps1.
Source: "{#BuildBin}\*"; DestDir: "{app}"; Excludes: {#PrivatePayloadExcludes}; Flags: ignoreversion

; Explicit runtime game payload. This deliberately does not recurse the extracted-game
; root, so REA/Ghidra sidecars and analysis reports cannot enter the installer.
Source: "{#GameRoot}\Bat\*"; DestDir: "{app}\game\Bat"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Effect\*"; DestDir: "{app}\game\Effect"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\MainSequence\*"; DestDir: "{app}\game\MainSequence"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Map01\*"; DestDir: "{app}\game\Map01"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Map02\*"; DestDir: "{app}\game\Map02"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Map03\*"; DestDir: "{app}\game\Map03"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Map04\*"; DestDir: "{app}\game\Map04"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Map05\*"; DestDir: "{app}\game\Map05"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Map07\*"; DestDir: "{app}\game\Map07"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Map08\*"; DestDir: "{app}\game\Map08"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\MenuBg\*"; DestDir: "{app}\game\MenuBg"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Mission\*"; DestDir: "{app}\game\Mission"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Object\*"; DestDir: "{app}\game\Object"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Shader\*"; DestDir: "{app}\game\Shader"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Sound\*"; DestDir: "{app}\game\Sound"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\System\*"; DestDir: "{app}\game\System"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\Weapon\*"; DestDir: "{app}\game\Weapon"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\XUI\*"; DestDir: "{app}\game\XUI"; Excludes: {#AnalysisExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GameRoot}\default.xex"; DestDir: "{app}\game"; Flags: ignoreversion
Source: "{#GameRoot}\default_decrypted.xex"; DestDir: "{app}\game"; Flags: ignoreversion

; Server script/data, samples, multiplayer docs, project/third-party notices, and the
; post-install F5 quick-start page.
Source: "{#PayloadData}\*"; DestDir: "{app}"; Excludes: {#PrivatePayloadExcludes}; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PrivateRoot}\quick-start.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\EDF2027 Private Test\EDF2027"; Filename: "{app}\edf2027.exe"; Parameters: "--game_data_root=""{app}\game"""; WorkingDir: "{app}"
Name: "{autoprograms}\EDF2027 Private Test\Dedicated Server"; Filename: "{app}\edf2027_server.exe"; Parameters: "--name ""EDF Host"" --mission ""Mission 1"" --data-dir ""{app}\server-data"""; WorkingDir: "{app}"
Name: "{autoprograms}\EDF2027 Private Test\Directory Server"; Filename: "{app}\edf2027_master.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\EDF2027 Private Test\Experimental Guest-Backed Host"; Filename: "{app}\edf2027_authoritative_host.exe"; Parameters: "--game-root ""{app}\game"" --user-root ""{localappdata}\EDF2027 Private Test\server-user-data"" --input-script ""{app}\tools\native-headless-mission-input.txt"" --mission-actor-reconciliation --trace-input-consumers --scripts ""{app}\server-data\scripts"" --name ""EDF Guest Host"" --mission ""Mission 1"""; WorkingDir: "{app}"
Name: "{autodesktop}\EDF2027 Private Test"; Filename: "{app}\edf2027.exe"; Parameters: "--game_data_root=""{app}\game"""; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\edf2027.exe"; Description: "Launch EDF2027 Private Test"; Parameters: "--game_data_root=""{app}\game"""; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent
