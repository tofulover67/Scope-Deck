; Inno Setup script for the Scope Deck installer: the app, and the ScopeTap
; OFX plugin placed where Resolve scans for it.
;
; Built by .github/workflows/release.yml. To build it by hand after a release
; build:
;
;   ISCC.exe /DAppVersion=1.2.3 /DNumericVersion=1.2.3 ^
;            /DBuildDir=<build dir> /DOutputDir=<dir> installer\ScopeDeck.iss
;
; BuildDir must hold a build with the CUDA ScopeTap in it - the release
; workflow checks that before it gets here.

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef NumericVersion
  #define NumericVersion "0.0.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\build-release"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
; Never change AppId: it is how an upgrade finds the install it replaces.
AppId={{4D090D11-83A6-4565-B036-8178902CA670}
AppName=Scope Deck
AppVersion={#AppVersion}
VersionInfoVersion={#NumericVersion}
; Fixed, not chosen by the user: the plugin's "Open Scope Deck" button
; launches scopedeck.exe from exactly this folder.
DefaultDirName={autopf}\Scope Deck
DisableDirPage=yes
DisableProgramGroupPage=yes
; Admin because the plugin has to go under Program Files\Common Files\OFX,
; the one folder Resolve scans on Windows.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=ScopeDeck-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; The installer exe itself. Shortcuts and the uninstall entry need nothing:
; they point at scopedeck.exe, which carries the icon as a resource.
SetupIconFile=..\app\icon\scopedeck.ico
UninstallDisplayIcon={app}\scopedeck.exe
UninstallDisplayName=Scope Deck

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#BuildDir}\scopedeck.exe";           DestDir: "{app}"; Flags: ignoreversion
; The timecode and subtitle bridges look for these next to the exe.
Source: "{#BuildDir}\timecode_poll_worker.py"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\subtitle_poll_worker.py"; DestDir: "{app}"; Flags: ignoreversion
; The bridges run those scripts with this Python first, so the machine needs
; none of its own (release.ps1 downloads and checks it).
Source: "{#BuildDir}\python\*"; DestDir: "{app}\python"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\bundle\ScopeTap.ofx.bundle\*"; DestDir: "{commoncf64}\OFX\Plugins\ScopeTap.ofx.bundle"; Flags: ignoreversion recursesubdirs createallsubdirs
; Premiere's Transmit plugin, into the MediaCore folder Premiere registers for
; third-party plugins - only where Premiere is actually installed.
Source: "{#BuildDir}\ScopeTransmit.prm"; DestDir: "{code:MediaCoreDir}"; Check: HasPremiere; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Scope Deck"; Filename: "{app}\scopedeck.exe"
Name: "{autodesktop}\Scope Deck";  Filename: "{app}\scopedeck.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\scopedeck.exe"; Description: "{cm:LaunchProgram,Scope Deck}"; Flags: nowait postinstall skipifsilent

[Messages]
FinishedLabel=Setup has finished installing Scope Deck.%n%nDaVinci Resolve: add Scope Tap (OpenFX, Scope Deck group) to a node.%n%nPremiere Pro: tick Scope Deck under Preferences > Playback > Video Device.%n%nThen pick the host in Scope Deck's Input menu.

[Code]
// Premiere's third-party plugin folder, as Premiere itself registers it
// (HKLM\SOFTWARE\Adobe\Premiere Pro\<version>\CommonPluginInstallPath), or ''
// when no Premiere is installed. Every version shares one MediaCore folder in
// practice; the last one listed wins.
function PremierePluginDir(): String;
var
  Versions: TArrayOfString;
  I: Integer;
  Path: String;
begin
  Result := '';
  if RegGetSubkeyNames(HKLM, 'SOFTWARE\Adobe\Premiere Pro', Versions) then
    for I := 0 to GetArrayLength(Versions) - 1 do
      if RegQueryStringValue(HKLM, 'SOFTWARE\Adobe\Premiere Pro\' + Versions[I],
                             'CommonPluginInstallPath', Path) and (Path <> '') then
        Result := RemoveBackslashUnlessRoot(Path);
end;

function HasPremiere(): Boolean;
begin
  Result := PremierePluginDir() <> '';
end;

function MediaCoreDir(Param: String): String;
begin
  Result := PremierePluginDir();
end;

function IsProcessRunning(ExeName: String): Boolean;
var
  Locator, Service, Procs: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Procs := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''' + ExeName + '''');
    Result := Procs.Count > 0;
  except
    Result := False;
  end;
end;

// Resolve holds the loaded ScopeTap.ofx open and only scans for plugins at
// startup - a Resolve sitting on the Project Manager still counts - and
// Premiere does the same with ScopeTransmit.prm. So install and uninstall
// wait for both to be quit rather than failing half-way through replacing a
// plugin.
function WaitForHostsClosed(): Boolean;
var
  Host: String;
begin
  Result := True;
  while True do
  begin
    if IsProcessRunning('Resolve.exe') then
      Host := 'DaVinci Resolve'
    else if IsProcessRunning('Adobe Premiere Pro.exe') then
      Host := 'Adobe Premiere Pro'
    else
      Exit;

    if SuppressibleMsgBox(Host + ' is running.' + #13#10#13#10 +
        'Quit it fully (in Resolve, the Project Manager window counts too), then click Retry. ' +
        'It keeps the Scope Deck plugin open while it runs.',
        mbError, MB_RETRYCANCEL, IDCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not WaitForHostsClosed() then
    Result := 'DaVinci Resolve or Premiere Pro is still running. Quit it and run Setup again.';
end;

function InitializeUninstall(): Boolean;
begin
  Result := WaitForHostsClosed();
end;
