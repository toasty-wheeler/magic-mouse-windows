; MagicMouseNative installer (Inno Setup). Build with package.bat.
;
; Installs the app and the test-signed driver, turns on test signing if needed (the user
; chooses; BitLocker is suspended for that restart) and puts the driver on the paired Magic
; Mouse with Driver\mmdriver.exe. Uninstalling takes the driver off the mouse again.

#ifndef AppVersion
  #define AppVersion "1.0.0"
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
      Comment: "Puts the driver on the Magic Mouse again, e.g. after pairing it anew"

[Tasks]
Name: "startup"; Description: "Start with Windows"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "MagicMouseNative"; \
      ValueData: """{app}\MagicMouseNative.exe"" /tray"; Tasks: startup; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MagicMouseNative"; Flags: dontcreatekey uninsdeletekey

[Run]
Filename: "{app}\MagicMouseNative.exe"; Description: "Open MagicMouseNative"; Flags: postinstall nowait runasoriginaluser skipifsilent

[UninstallRun]
Filename: "{app}\Driver\mmdriver.exe"; Parameters: "uninstall /quiet"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveDriver"
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN MagicMouseNative /F"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveTask"

[Code]
var
  TestSigningPage: TInputOptionWizardPage;
  RestartNeeded: Boolean;

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

function InitializeSetup: Boolean;
begin
  Result := True;
  if SecureBootOn then begin
    MsgBox('Secure Boot is on.' #13#10#13#10 +
           'This driver is test-signed (not signed by Microsoft), and Windows refuses test-signed ' +
           'drivers while Secure Boot is on. Turn Secure Boot off in your PC''s UEFI/BIOS setup, ' +
           'then run setup again.' #13#10#13#10 +
           'If BitLocker is on, have your recovery key ready before you change Secure Boot.',
           mbCriticalError, MB_OK);
    Result := False;
    Exit;
  end;
  if RegKeyExists(HKLM64, 'SYSTEM\CurrentControlSet\Services\applewirelessmouse') then
    MsgBox('Apple''s Boot Camp mouse driver (applewirelessmouse) seems to be installed. Remove it ' +
           '(and Magic Utilities, if you use it) before using this driver: they attach to the same mouse.',
           mbInformation, MB_OK);
end;

procedure InitializeWizard;
begin
  if TestSigningOn then Exit;
  TestSigningPage := CreateInputOptionPage(wpSelectTasks,
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

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  // The running app would keep its exe in use (and the old driver loaded).
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im MagicMouseNative.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := '';
end;

procedure InstallDriver;
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
    3: MsgBox('No Magic Mouse (2009) is paired, so the driver couldn''t be put on it yet.' #13#10#13#10 +
              'Pair the mouse in Bluetooth settings, then run "Reinstall Magic Mouse driver" from the Start menu.',
              mbInformation, MB_OK);
  else
    MsgBox('Installing the driver failed (code ' + IntToStr(Code) + '). Run "Reinstall Magic Mouse driver" ' +
           'from the Start menu to see what went wrong.', mbError, MB_OK);
  end;
  if (TestSigningPage <> nil) and (Code <> 1) then RestartNeeded := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then InstallDriver;
end;

function NeedRestart: Boolean;
begin
  Result := RestartNeeded;
end;
