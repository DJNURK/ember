; ===========================================================================
;  Ember - Windows installer (Inno Setup 6)
;
;  Builds Ember-<version>-Windows.exe, which installs:
;    * Ember.vst3  ->  {commoncf64}\VST3\        (C:\Program Files\Common Files\VST3)
;    * Ember.exe   ->  {autopf}\EmberAudio\Ember\ + Start Menu shortcut  (optional)
;
;  Compile from the repository root:
;
;    iscc packaging\windows\ember.iss ^
;         /DEmberVersion=1.0.0 ^
;         /DEmberSourceDir=C:\path\to\build\windows-release\Ember_artefacts\Release ^
;         /DEmberOutputDir=C:\path\to\build\packages
;
;  All three defines are optional; the defaults below assume the layout produced
;  by `cmake --build --preset windows-release`. Prefer absolute paths for
;  /DEmberSourceDir - a relative one is resolved against this script's folder.
;
;  Requires Inno Setup 6 (https://jrsoftware.org/isdl.php). 64-bit only.
; ===========================================================================

#if VER < EncodeVer(6,0,0)
  #error "This script requires Inno Setup 6 or newer (https://jrsoftware.org/isdl.php)."
#endif

#ifndef EmberVersion
  #define EmberVersion "1.0.0"
#endif

#ifndef EmberSourceDir
  #define EmberSourceDir SourcePath + "..\..\build\windows-release\Ember_artefacts\Release"
#endif

#ifndef EmberOutputDir
  #define EmberOutputDir SourcePath + "..\..\build\packages"
#endif

#ifndef EmberLicenseFile
  #define EmberLicenseFile SourcePath + "..\..\LICENSE"
#endif

#define EmberName      "Ember"
#define EmberPublisher "EmberAudio"
#define EmberUrl       "https://example.invalid/ember"

; A relative /DEmberSourceDir is resolved against the folder holding this script,
; so that the [Files] entries and the existence checks below agree.
#if DirExists(EmberSourceDir)
  #define EmberSrc EmberSourceDir
#else
  #define EmberSrc SourcePath + EmberSourceDir
#endif

#define EmberVst3Dir       EmberSrc + "\VST3\Ember.vst3"
#define EmberStandaloneExe EmberSrc + "\Standalone\Ember.exe"

#if !DirExists(EmberVst3Dir)
  #error "Ember.vst3 was not found. Build the plug-in first (cmake --build --preset windows-release) or pass /DEmberSourceDir=<absolute path to Ember_artefacts\Release>."
#endif

; The standalone is optional: a plug-in-only installer is a legitimate build.
#if FileExists(EmberStandaloneExe)
  #define EmberHaveStandalone
#else
  #pragma message "Ember.exe was not found under <EmberSourceDir>\Standalone: building a VST3-only installer."
#endif

[Setup]
; Keep this GUID stable forever: it is how Windows recognises an existing
; Ember installation and how upgrades replace rather than duplicate it.
AppId={{9F2C4A61-5E3D-4B77-9C1A-0D8E6F2B71A4}
AppName={#EmberName}
AppVersion={#EmberVersion}
AppVerName={#EmberName} {#EmberVersion}
AppPublisher={#EmberPublisher}
AppPublisherURL={#EmberUrl}
AppSupportURL={#EmberUrl}
AppUpdatesURL={#EmberUrl}
VersionInfoVersion={#EmberVersion}
VersionInfoCompany={#EmberPublisher}
VersionInfoProductName={#EmberName}
VersionInfoDescription={#EmberName} multiband analog saturation and distortion
VersionInfoCopyright=Copyright (C) {#EmberPublisher}

DefaultDirName={autopf}\{#EmberPublisher}\{#EmberName}
DefaultGroupName={#EmberPublisher}
DisableProgramGroupPage=yes
DisableWelcomePage=no
#ifndef EmberHaveStandalone
; {app} is only used by the standalone, so do not ask for a folder without it.
DisableDirPage=yes
#endif
LicenseFile={#EmberLicenseFile}

OutputDir={#EmberOutputDir}
OutputBaseFilename={#EmberName}-{#EmberVersion}-Windows
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; 64-bit only. x64compatible also covers ARM64 machines running x64 code;
; it needs Inno Setup 6.3, so fall back to the older identifier on 6.0-6.2.
#if VER >= EncodeVer(6,3,0)
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#else
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
#endif

; Writing to Common Files\VST3 and Program Files needs elevation.
PrivilegesRequired=admin
MinVersion=10.0
Uninstallable=yes
UninstallDisplayName={#EmberName} {#EmberVersion}
#ifdef EmberHaveStandalone
UninstallDisplayIcon={app}\Ember.exe
#endif
CloseApplications=yes
CloseApplicationsFilter=*.exe,*.vst3
RestartApplications=no
ChangesAssociations=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full";   Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3";       Description: "VST3 plug-in (64-bit)"; Types: full custom; Flags: fixed
#ifdef EmberHaveStandalone
Name: "standalone"; Description: "Standalone application"; Types: full
#endif

[Tasks]
#ifdef EmberHaveStandalone
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Components: standalone; Flags: unchecked
#endif

[Files]
; A .vst3 on Windows is a folder (bundle), so copy it recursively.
Source: "{#EmberVst3Dir}\*"; DestDir: "{commoncf64}\VST3\Ember.vst3"; \
    Components: vst3; \
    Flags: ignoreversion recursesubdirs createallsubdirs uninsremovereadonly

#ifdef EmberHaveStandalone
Source: "{#EmberStandaloneExe}"; DestDir: "{app}"; \
    Components: standalone; Flags: ignoreversion
#endif

[Icons]
#ifdef EmberHaveStandalone
Name: "{autoprograms}\{#EmberName}"; Filename: "{app}\Ember.exe"; Components: standalone
Name: "{autodesktop}\{#EmberName}";  Filename: "{app}\Ember.exe"; Components: standalone; Tasks: desktopicon
#endif

[Run]
#ifdef EmberHaveStandalone
Filename: "{app}\Ember.exe"; Description: "{cm:LaunchProgram,{#EmberName}}"; \
    Components: standalone; Flags: nowait postinstall skipifsilent
#endif

[UninstallDelete]
; Remove the plug-in bundle itself plus any empty folders we created, so an
; uninstall leaves nothing of Ember behind.
Type: filesandordirs; Name: "{commoncf64}\VST3\Ember.vst3"
Type: dirifempty;     Name: "{autopf}\{#EmberPublisher}\{#EmberName}"
Type: dirifempty;     Name: "{autopf}\{#EmberPublisher}"
