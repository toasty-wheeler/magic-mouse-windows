; MagicMouseNative installer (Inno Setup). Build with package.bat.
;
; Installs the app and lets the user choose how the Magic Mouse is driven:
;   - MagicMouseNative's own driver (test-signed): turns on test signing if needed (BitLocker
;     is suspended for that restart);
;   - Boot Camp mode: Apple's signed Boot Camp driver (not included: a copy already on the PC
;     is used, or the user points to AppleWirelessMouse.inf), full Bluetooth logging, and the
;     app started as administrator;
;   - the app only.
; Driver\mmdriver.exe does the work. Uninstalling undoes either driver choice, including Boot
; Camp mode's Bluetooth logging setting.

#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif

[Setup]
AppId={{8F3C2A51-6B7E-4E0D-9C1A-5D2E7F4A9B10}
AppName=MagicMouseNative
AppVersion={#AppVersion}
AppPublisher=toasty-wheeler
AppPublisherURL=https://github.com/toasty-wheeler/magic-mouse-windows
AppSupportURL=https://github.com/toasty-wheeler/magic-mouse-windows/issues
DefaultDirName={autopf}\MagicMouseNative
DefaultGroupName=MagicMouseNative
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.15063
OutputDir=..\dist
OutputBaseFilename=MagicMouseNative-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\MagicMouseNative.exe
UninstallDisplayName=MagicMouseNative (Magic Mouse driver and app)
CloseApplications=no
UsedUserAreasWarning=no

[Files]
Source: "..\app\MagicMouseNative.exe";      DestDir: "{app}";        Flags: ignoreversion
Source: "mmdriver.exe";                     DestDir: "{app}\Driver"; Flags: ignoreversion
Source: "..\driver\out\mmfilter.sys";       DestDir: "{app}\Driver"; Flags: ignoreversion
Source: "..\driver\out\mmfilter.inf";       DestDir: "{app}\Driver"; Flags: ignoreversion
Source: "..\driver\out\mmfilter.cat";       DestDir: "{app}\Driver"; Flags: ignoreversion
Source: "..\driver\out\mmfilter_test.cer";  DestDir: "{app}\Driver"; Flags: ignoreversion

[Icons]
Name: "{group}\MagicMouseNative"; Filename: "{app}\MagicMouseNative.exe"
Name: "{group}\Reinstall Magic Mouse driver"; Filename: "{app}\Driver\mmdriver.exe"; Parameters: "install"; \
      Comment: "Puts MagicMouseNative's driver on the Magic Mouse again, e.g. after pairing it anew"; Check: not BootCampChosen
Name: "{group}\Reinstall Boot Camp mode"; Filename: "{app}\Driver\mmdriver.exe"; Parameters: "bootcamp"; \
      Comment: "Puts Apple's Boot Camp driver on the Magic Mouse again, e.g. after pairing it anew"; Check: BootCampChosen

[Tasks]
Name: "startup"; Description: "Start with Windows"

[Registry]
; Boot Camp mode starts the app as administrator through a logon task instead (see InstallDriver).
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "MagicMouseNative"; \
      ValueData: """{app}\MagicMouseNative.exe"" /tray"; Tasks: startup; Check: not BootCampChosen; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MagicMouseNative"; Flags: dontcreatekey uninsdeletekey

[Run]
Filename: "{app}\MagicMouseNative.exe"; Description: "Open MagicMouseNative"; Flags: postinstall nowait runasoriginaluser skipifsilent; \
      Check: not BootCampChosen
; Boot Camp mode: as administrator (reading the Bluetooth log needs it).
Filename: "{app}\MagicMouseNative.exe"; Description: "Open MagicMouseNative"; Flags: postinstall nowait skipifsilent; \
      Check: BootCampChosen

[UninstallRun]
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN MagicMouseNative /F"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveTask"

[Code]
const
  ModeDriver = 0;
  ModeBootCamp = 1;
  ModeAppOnly = 2;

var
  ModePage: TInputOptionWizardPage;
  TestSigningPage: TInputOptionWizardPage;
  BootCampPage: TInputFileWizardPage;
  StoredAppleDriver: Boolean;           // Apple's driver already in Windows' driver store
  RestartNeeded, UninstallRestart: Boolean;

function SecureBootOn: Boolean;
var
  V: Cardinal;
begin
  Result := RegQueryDWordValue(HKLM64, 'SYSTEM\CurrentControlSet\Control\SecureBoot\State', 'UEFISecureBootEnabled', V) and (V = 1);
end;

function TestSigningOn: Boolean;
var
  S: String;
begin
  Result := RegQueryStringValue(HKLM64, 'SYSTEM\CurrentControlSet\Control', 'SystemStartOptions', S) and
            (Pos('TESTSIGNING', Uppercase(S)) > 0);
end;

// Apple's driver in the driver store: an %windir%\INF\oemNN.inf whose catalog is Apple's.
function AppleDriverStored: Boolean;
var
  F: TFindRec;
  Dir: String;
begin
  Result := False;
  Dir := ExpandConstant('{win}\INF\');
  if FindFirst(Dir + 'oem*.inf', F) then begin
    try
      repeat
        if CompareText(GetIniString('Version', 'CatalogFile', '', Dir + F.Name), 'applewirelessmouse.cat') = 0 then
          Result := True;
      until Result or not FindNext(F);
    finally
      FindClose(F);
    end;
  end;
end;

function Mode: Integer;
begin
  if ModePage = nil then
    Result := ModeDriver
  else if ModePage.Values[ModeBootCamp] then
    Result := ModeBootCamp
  else if ModePage.Values[ModeAppOnly] then
    Result := ModeAppOnly
  else
    Result := ModeDriver;
end;

function BootCampChosen: Boolean;
begin
  Result := Mode = ModeBootCamp;
end;

procedure InitializeWizard;
var
  Found: String;
begin
  StoredAppleDriver := AppleDriverStored;

  ModePage := CreateInputOptionPage(wpSelectTasks,
    'Magic Mouse driver', 'How should the Magic Mouse work?',
    'MagicMouseNative''s own driver is test-signed: Windows only loads it with test signing on, which ' +
    'needs Secure Boot off.' #13#10#13#10 +
    'Boot Camp mode uses Apple''s own signed driver instead, so nothing about Windows'' security ' +
    'settings changes. Apple''s driver isn''t included: a copy already on this PC is used, or you ' +
    'point to it on the next page.',
    True, False);
  ModePage.Add('MagicMouseNative driver (test signing; Secure Boot must be off)');
  ModePage.Add('Boot Camp mode: Apple''s signed driver (the app runs as administrator)');
  ModePage.Add('The app only; leave the mouse as it is');
  if SecureBootOn or RegKeyExists(HKLM64, 'SYSTEM\CurrentControlSet\Services\applewirelessmouse') then
    ModePage.SelectedValueIndex := ModeBootCamp
  else
    ModePage.SelectedValueIndex := ModeDriver;

  if not TestSigningOn then begin
    TestSigningPage := CreateInputOptionPage(ModePage.ID,
      'Test signing', 'Windows needs test signing on to load this driver',
      'The driver is test-signed, not signed by Microsoft, so Windows only loads it with test ' +
      'signing on. Windows then shows "Test Mode" in the corner of the desktop.' #13#10#13#10 +
      'Turning it on takes effect after a restart; until then the Magic Mouse doesn''t work, so ' +
      'restart with the keyboard if you need to. If BitLocker is on, it''s suspended for that one ' +
      'restart, so you won''t be asked for the recovery key.' #13#10#13#10 +
      'Without test signing, setup installs only the app and leaves the mouse as it is.',
      False, False);
    TestSigningPage.Add('Turn on test signing and install the driver (restart needed)');
    TestSigningPage.Values[0] := True;
  end;

  if StoredAppleDriver then
    Found := 'Apple''s driver is already on this PC and will be used; leave the box empty. To use a ' +
             'different copy, choose its AppleWirelessMouse.inf.'
  else
    Found := 'Apple''s driver isn''t on this PC. Choose AppleWirelessMouse.inf from Apple''s Boot Camp ' +
             'drivers (the AppleWirelessMouse64 package); its .sys and .cat must be next to it.';
  BootCampPage := CreateInputFilePage(ModePage.ID,
    'Boot Camp mode', 'Apple''s driver and Bluetooth logging',
    'Apple''s driver switches the mouse into multi-touch mode, and MagicMouseNative reads the touches ' +
    'from Windows'' Bluetooth packet log. That needs Windows'' full Bluetooth logging turned on. It''s ' +
    'one setting for all Bluetooth devices: while an administrator program records that log, it can ' +
    'see what Bluetooth devices send, keyboards included. MagicMouseNative keeps only the mouse''s ' +
    'touches, in memory, and drops everything else at once. Uninstalling turns the setting back to ' +
    'how it was. A restart is needed for it to apply.' #13#10#13#10 + Found);
  BootCampPage.Add('Apple''s driver (AppleWirelessMouse.inf):', 'Driver setup files|*.inf', '.inf');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if (TestSigningPage <> nil) and (PageID = TestSigningPage.ID) then Result := Mode <> ModeDriver;
  if PageID = BootCampPage.ID then Result := Mode <> ModeBootCamp;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = ModePage.ID) and (Mode = ModeDriver) and SecureBootOn then begin
    MsgBox('Secure Boot is on, and Windows refuses test-signed drivers while it is. Choose Boot Camp ' +
           'mode, or turn Secure Boot off in your PC''s UEFI/BIOS setup and run setup again.' #13#10#13#10 +
           'If BitLocker is on, have your recovery key ready before you change Secure Boot.',
           mbError, MB_OK);
    Result := False;
  end;
  if (CurPageID = ModePage.ID) and (Mode = ModeDriver) and
     RegKeyExists(HKLM64, 'SYSTEM\CurrentControlSet\Services\applewirelessmouse') then
    MsgBox('Apple''s Boot Camp mouse driver is installed. MagicMouseNative''s driver replaces it on the ' +
           'Magic Mouse (remove Magic Utilities too, if you use it).', mbInformation, MB_OK);
  if (CurPageID = BootCampPage.ID) and not StoredAppleDriver and (Trim(BootCampPage.Values[0]) = '') then begin
    MsgBox('Choose AppleWirelessMouse.inf from Apple''s Boot Camp drivers, or go back and pick another option.',
           mbError, MB_OK);
    Result := False;
  end;
  if (CurPageID = BootCampPage.ID) and (Trim(BootCampPage.Values[0]) <> '') and
     not FileExists(Trim(BootCampPage.Values[0])) then begin
    MsgBox('That file doesn''t exist.', mbError, MB_OK);
    Result := False;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  // The running app would keep its exe in use (and the old driver loaded).
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im MagicMouseNative.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := '';
end;

procedure InstallMmfilter;
var
  Params: String;
  Code: Integer;
begin
  Params := 'install /quiet';
  if TestSigningPage <> nil then begin
    if not TestSigningPage.Values[0] then begin
      MsgBox('The driver wasn''t installed, because test signing is off. To install it later, turn ' +
             'test signing on and use "Reinstall Magic Mouse driver" in the Start menu.', mbInformation, MB_OK);
      Exit;
    end;
    Params := Params + ' /testsigning';
  end;
  WizardForm.StatusLabel.Caption := 'Installing the driver on the Magic Mouse...';
  if not Exec(ExpandConstant('{app}\Driver\mmdriver.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := 1;
  case Code of
    0: ;
    10: RestartNeeded := True;
    3: MsgBox('No Magic Mouse is paired, so the driver couldn''t be put on it yet.' #13#10#13#10 +
              'Pair the mouse in Bluetooth settings, then run "Reinstall Magic Mouse driver" from the Start menu.',
              mbInformation, MB_OK);
  else
    MsgBox('Installing the driver failed (code ' + IntToStr(Code) + '). Run "Reinstall Magic Mouse driver" ' +
           'from the Start menu to see what went wrong.', mbError, MB_OK);
  end;
  if (TestSigningPage <> nil) and (Code <> 1) then RestartNeeded := True;
end;

procedure InstallBootCamp;
var
  Params: String;
  Code: Integer;
begin
  Params := 'bootcamp /quiet';
  if Trim(BootCampPage.Values[0]) <> '' then Params := 'bootcamp "' + Trim(BootCampPage.Values[0]) + '" /quiet';
  WizardForm.StatusLabel.Caption := 'Putting Apple''s Boot Camp driver on the Magic Mouse...';
  if not Exec(ExpandConstant('{app}\Driver\mmdriver.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := 1;
  case Code of
    0, 10: RestartNeeded := True;       // the Bluetooth logging setting applies after a restart
    3: MsgBox('No Magic Mouse that Apple''s driver supports (2009 or Magic Mouse 2) is paired.' #13#10#13#10 +
              'Pair the mouse in Bluetooth settings, then run "Reinstall Boot Camp mode" from the Start menu.',
              mbInformation, MB_OK);
    4: MsgBox('Apple''s driver wasn''t found. Run "Reinstall Boot Camp mode" from the Start menu once you ' +
              'have it.', mbError, MB_OK);
  else
    MsgBox('Setting up Boot Camp mode failed (code ' + IntToStr(Code) + '). Run "Reinstall Boot Camp mode" ' +
           'from the Start menu to see what went wrong.', mbError, MB_OK);
  end;
  // Boot Camp mode needs the app elevated: "Start with Windows" becomes the administrator logon task.
  if WizardIsTaskSelected('startup') then
    if not Exec(ExpandConstant('{app}\MagicMouseNative.exe'), '/registerstartup', '', SW_HIDE, ewWaitUntilTerminated, Code)
       or (Code <> 0) then
      MsgBox('Couldn''t set MagicMouseNative to start with Windows as administrator. Turn on "Start with ' +
             'Windows" in its window while it runs as administrator.', mbInformation, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep <> ssPostInstall then Exit;
  case Mode of
    ModeDriver: InstallMmfilter;
    ModeBootCamp: InstallBootCamp;
  end;
end;

function NeedRestart: Boolean;
begin
  Result := RestartNeeded;
end;

// Takes either driver off the mouse and undoes Boot Camp mode's Bluetooth logging setting.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  if CurUninstallStep <> usUninstall then Exit;
  if Exec(ExpandConstant('{app}\Driver\mmdriver.exe'), 'uninstall /quiet', '', SW_HIDE, ewWaitUntilTerminated, Code) and
     (Code = 10) then
    UninstallRestart := True;
end;

function UninstallNeedRestart: Boolean;
begin
  Result := UninstallRestart;
end;
