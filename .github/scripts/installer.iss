; Windows installer of one plugin module, built by Package-Windows.ps1:
;   ISCC /DModule=ultimate-frame /DVersion=0.1.0 "/DDisplayName=Ultimate Frame" /DSourceDir=<install>\ultimate-frame
;        /DRepoRoot=<repo> /O<repo>\release installer.iss
; Saved as UTF-8 with BOM for the Russian strings.

#ifndef Module
  #error Pass /DModule=<module>
#endif
#ifndef Version
  #error Pass /DVersion=<x.y.z>
#endif
#ifndef DisplayName
  #error Pass /DDisplayName=<name>
#endif
#ifndef SourceDir
  #error Pass /DSourceDir=<installed module folder with bin\64bit and data>
#endif
#ifndef RepoRoot
  #error Pass /DRepoRoot=<repository root>
#endif

; the uninstall entries are keyed by these: never change them
#if Module == "ultimate-frame"
  #define AppGuid "F382A226-1D91-4BE8-B1DA-F4DAA487CDA1"
#elif Module == "browser-resize"
  #define AppGuid "882821D9-690C-4E84-87E6-28DE640F76EF"
#else
  #error Unknown module
#endif

[Setup]
; {code:} AppId: one uninstall entry per target (installed OBS or each portable folder), known only on the Ready page
AppId={code:GetAppId}
AppName={#DisplayName}
AppVersion={#Version}
AppPublisher=Vakovsky
AppPublisherURL=https://github.com/VakovskyGT/vakovsky-obs-plugins
AppSupportURL=https://github.com/VakovskyGT/vakovsky-obs-plugins/issues
AppUpdatesURL=https://github.com/VakovskyGT/vakovsky-obs-plugins/releases
VersionInfoVersion={#Version}
VersionInfoProductName={#DisplayName}
VersionInfoDescription={#DisplayName} {#Version} Setup
; {app} is the OBS folder; the files go where GetDllDir and GetDataDir say
DefaultDirName={code:GetDefaultDir}
AppendDefaultDirName=no
DirExistsWarning=no
DisableWelcomePage=no
DisableDirPage=no
DisableProgramGroupPage=yes
DisableReadyPage=no
UsePreviousAppDir=no
UsePreviousGroup=no
UsePreviousLanguage=no
UsePreviousPrivileges=no
UsePreviousSetupType=no
UsePreviousTasks=no
UsePreviousUserInfo=no
; outside every plugin layout: cleaning the other layout on reinstall must not delete the uninstall log
UninstallFilesDir={commonappdata}\vakovsky-obs-plugins\uninstall\{#Module}\{code:GetTargetId}
UninstallDisplayName={code:GetUninstallName}
UninstallDisplayIcon={uninstallexe}
InfoBeforeFile={#RepoRoot}\LICENSE
SetupIconFile={#RepoRoot}\.github\scripts\icons\{#Module}.ico
OutputBaseFilename={#Module}-{#Version}-windows-x64-installer
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; we ask to close OBS ourselves, only the OBS we install into
CloseApplications=no
SetupMutex=vakovsky-{#Module}-setup
ShowLanguageDialog=auto
WizardStyle=modern
Compression=lzma2
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
english.LicenseTitle=License
english.LicenseDescription={#DisplayName} is free software under the GNU General Public License, version 2 or later.
english.LicenseClick=You don't need to accept it to use the plugin. Click Next to continue.
english.TargetTitle=Where to install
english.TargetDescription=Which OBS Studio should get {#DisplayName}?
english.TargetSubCaption=The plugin goes where this OBS loads plugins from, in the layout for its version.
english.TargetInstalled=Installed OBS Studio (%1, version %2)
english.TargetInstalledNotFound=Installed OBS Studio (not found in the registry: you choose its folder next)
english.TargetPortable=Portable OBS Studio (you choose its folder next)
english.ObsDirTitle=OBS Studio folder
english.ObsDirDescription=The folder that has bin\64bit\obs64.exe.
english.ObsDirLabelPortable=Choose the folder of the portable OBS Studio.
english.ObsDirLabelInstalled=Choose the folder OBS Studio is installed in.
english.ErrNoObs=OBS Studio is not in this folder: there is no bin\64bit\obs64.exe.%n%nChoose the OBS Studio folder.
english.ErrNoVersion=Could not read the version of %1.
english.ErrOldObs={#DisplayName} needs OBS Studio 31 or newer. This one is %1.
english.AskInstalledFolder=This is the installed OBS Studio.%n%nInstall into it (the plugin goes to C:\ProgramData\obs-studio\plugins)?
english.AskNoPortableMark=There is no portable_mode.txt next to bin in this folder, so OBS here is portable only when started with --portable.%n%nContinue?
english.AskPortableMark=This OBS has portable_mode.txt and looks portable. Plugins in ProgramData only load in the installed OBS.%n%nContinue?
english.ObsRunning=OBS Studio is running:%n%1%n%nClose it and click Retry.
english.ReadyObs=OBS Studio:
english.ReadyLayoutNew=layout of OBS 33 and newer
english.ReadyLayoutLegacy=layout of OBS 31 and 32 (legacy)
english.ReadyFiles=Files:
english.UninstallInstalled=%1 %2 (OBS Studio)
english.UninstallPortable=%1 %2 (portable OBS %3)

russian.LicenseTitle=Лицензия
russian.LicenseDescription={#DisplayName} — свободная программа под GNU General Public License версии 2 или новее.
russian.LicenseClick=Принимать её для работы с плагином не нужно. Нажмите «Далее».
russian.TargetTitle=Куда установить
russian.TargetDescription=В какой OBS Studio поставить {#DisplayName}?
russian.TargetSubCaption=Плагин ляжет туда, откуда этот OBS загружает плагины, в раскладке для его версии.
russian.TargetInstalled=Установленный OBS Studio (%1, версия %2)
russian.TargetInstalledNotFound=Установленный OBS Studio (в реестре не найден: папку выберете на следующем шаге)
russian.TargetPortable=Портативный OBS Studio (папку выберете на следующем шаге)
russian.ObsDirTitle=Папка OBS Studio
russian.ObsDirDescription=Папка, в которой есть bin\64bit\obs64.exe.
russian.ObsDirLabelPortable=Выберите папку портативного OBS Studio.
russian.ObsDirLabelInstalled=Выберите папку, в которую установлен OBS Studio.
russian.ErrNoObs=В этой папке нет OBS Studio: нет файла bin\64bit\obs64.exe.%n%nВыберите папку OBS Studio.
russian.ErrNoVersion=Не удалось прочитать версию %1.
russian.ErrOldObs={#DisplayName} работает в OBS Studio 31 и новее. Здесь версия %1.
russian.AskInstalledFolder=Это установленный OBS Studio.%n%nПоставить плагин в него (в C:\ProgramData\obs-studio\plugins)?
russian.AskNoPortableMark=В этой папке рядом с bin нет portable_mode.txt: OBS здесь портативный, только если запускать его с --portable.%n%nПродолжить?
russian.AskPortableMark=В этом OBS есть portable_mode.txt, он похож на портативный. Плагины из ProgramData загружает только установленный OBS.%n%nПродолжить?
russian.ObsRunning=OBS Studio запущен:%n%1%n%nЗакройте его и нажмите «Повторить».
russian.ReadyObs=OBS Studio:
russian.ReadyLayoutNew=раскладка OBS 33 и новее
russian.ReadyLayoutLegacy=раскладка OBS 31 и 32 (legacy)
russian.ReadyFiles=Файлы:
russian.UninstallInstalled=%1 %2 (OBS Studio)
russian.UninstallPortable=%1 %2 (портативный OBS %3)

[Files]
Source: "{#SourceDir}\bin\64bit\*"; DestDir: "{code:GetDllDir}"; Flags: ignoreversion
Source: "{#SourceDir}\data\*"; DestDir: "{code:GetDataDir}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
const
  ModuleName = '{#Module}';

var
  TargetPage: TInputOptionWizardPage;
  InstalledDir: String;
  InstalledFound: Boolean;
  PortableDir: String;
  ObsMajor: Integer;
  ObsVersionText: String;
  TargetReady: Boolean;

function ObsExe(Dir: String): String;
begin
  Result := AddBackslash(Dir) + 'bin\64bit\obs64.exe';
end;

function SameDir(A, B: String): Boolean;
begin
  Result := CompareText(RemoveBackslashUnlessRoot(A), RemoveBackslashUnlessRoot(B)) = 0;
end;

function IsPortable: Boolean;
begin
  Result := TargetPage.Values[1];
end;

function ObsDir: String;
begin
  Result := RemoveBackslashUnlessRoot(WizardDirValue);
end;

{ the installed OBS from the key its installer writes, 64-bit view first }
procedure FindInstalledObs;
var
  Dir: String;
begin
  InstalledFound := False;
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Dir) then
    if not RegQueryStringValue(HKLM32, 'SOFTWARE\OBS Studio', '', Dir) then
      Dir := '';
  if (Dir <> '') and FileExists(ObsExe(Dir)) then begin
    InstalledDir := RemoveBackslashUnlessRoot(Dir);
    InstalledFound := True;
  end else
    InstalledDir := ExpandConstant('{commonpf64}\obs-studio');
end;

{ reads the version of obs64.exe into ObsMajor and ObsVersionText }
function ReadObsVersion(Dir: String): Boolean;
var
  MS, LS: Cardinal;
begin
  Result := GetVersionNumbers(ObsExe(Dir), MS, LS);
  if Result then begin
    ObsMajor := Integer(MS shr 16);
    ObsVersionText := IntToStr(ObsMajor) + '.' + IntToStr(Integer(MS and $FFFF)) + '.' + IntToStr(Integer(LS shr 16));
  end;
end;

{ the checks shared by both pages: OBS is there and new enough, the portable mark matches the choice }
function CheckObs(Dir: String; Portable: Boolean): Boolean;
var
  Mark: Boolean;
begin
  Result := False;
  if not FileExists(ObsExe(Dir)) then begin
    SuppressibleMsgBox(CustomMessage('ErrNoObs'), mbError, MB_OK, IDOK);
    exit;
  end;
  if not ReadObsVersion(Dir) then begin
    SuppressibleMsgBox(FmtMessage(CustomMessage('ErrNoVersion'), [ObsExe(Dir)]), mbError, MB_OK, IDOK);
    exit;
  end;
  if ObsMajor < 31 then begin
    SuppressibleMsgBox(FmtMessage(CustomMessage('ErrOldObs'), [ObsVersionText]), mbError, MB_OK, IDOK);
    exit;
  end;
  Mark := FileExists(AddBackslash(Dir) + 'portable_mode.txt');
  if Portable and not Mark then
    if SuppressibleMsgBox(CustomMessage('AskNoPortableMark'), mbConfirmation, MB_YESNO, IDYES) <> IDYES then
      exit;
  if not Portable and Mark then
    if SuppressibleMsgBox(CustomMessage('AskPortableMark'), mbConfirmation, MB_YESNO, IDYES) <> IDYES then
      exit;
  Result := True;
end;

{ an obs64.exe started from this folder; other OBS instances (--multi) do not hold our files }
function IsObsRunning(Dir: String): Boolean;
var
  Locator, Service, Processes, Process: Variant;
  Path: String;
  I: Integer;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Processes := Service.ExecQuery('SELECT ExecutablePath FROM Win32_Process WHERE Name = ''obs64.exe''');
    for I := 0 to Processes.Count - 1 do begin
      Process := Processes.ItemIndex(I);
      try
        Path := Process.ExecutablePath;
      except
        Path := '';
      end;
      if CompareText(Path, ObsExe(Dir)) = 0 then
        Result := True;
    end;
  except
    Log('Could not list the running processes: ' + GetExceptionMessage);
  end;
end;

function WaitForObsClosed(Dir: String): Boolean;
begin
  Result := True;
  while IsObsRunning(Dir) do
    if SuppressibleMsgBox(FmtMessage(CustomMessage('ObsRunning'), [ObsExe(Dir)]), mbError, MB_RETRYCANCEL, IDCANCEL) <> IDRETRY then begin
      Result := False;
      exit;
    end;
end;

function ProgramDataModuleDir: String;
begin
  Result := ExpandConstant('{commonappdata}\obs-studio\plugins\') + ModuleName;
end;

function GetDefaultDir(Param: String): String;
begin
  Result := InstalledDir;
end;

function GetTargetId(Param: String): String;
begin
  if IsPortable then
    Result := Copy(GetSHA1OfString(Lowercase(ObsDir)), 1, 12)
  else
    Result := 'ProgramData';
end;

{ empty until the Ready page: Inno only needs the real AppId when the installation starts }
function GetAppId(Param: String): String;
begin
  if TargetReady then
    Result := '{' + '{#AppGuid}' + '}_' + GetTargetId('')
  else
    Result := '';
end;

function GetUninstallName(Param: String): String;
begin
  if IsPortable then
    Result := FmtMessage(CustomMessage('UninstallPortable'), ['{#DisplayName}', '{#Version}', ObsDir])
  else
    Result := FmtMessage(CustomMessage('UninstallInstalled'), ['{#DisplayName}', '{#Version}']);
end;

{ where OBS looks for the module: the portable or installed OBS, 33+ or the legacy layout of 31-32 }
function GetDllDir(Param: String): String;
begin
  if IsPortable then begin
    if ObsMajor >= 33 then
      Result := ObsDir + '\plugins\' + ModuleName
    else
      Result := ObsDir + '\obs-plugins\64bit';
  end else begin
    if ObsMajor >= 33 then
      Result := ProgramDataModuleDir
    else
      Result := ProgramDataModuleDir + '\bin\64bit';
  end;
end;

function GetDataDir(Param: String): String;
begin
  if IsPortable then begin
    if ObsMajor >= 33 then
      Result := ObsDir + '\plugins\' + ModuleName + '\data'
    else
      Result := ObsDir + '\data\obs-plugins\' + ModuleName;
  end else
    Result := ProgramDataModuleDir + '\data';
end;

procedure DeleteModuleFiles(Dir: String);
begin
  DeleteFile(Dir + '\' + ModuleName + '.dll');
  DeleteFile(Dir + '\' + ModuleName + '.pdb');
end;

{ removes the module from every layout of this OBS, so the new files are its only copy }
procedure RemoveOldCopies;
begin
  if not IsPortable then
    DelTree(ProgramDataModuleDir, True, True, True)
  else
    DelTree(ObsDir + '\plugins\' + ModuleName, True, True, True);
  DeleteModuleFiles(ObsDir + '\obs-plugins\64bit');
  DelTree(ObsDir + '\data\obs-plugins\' + ModuleName, True, True, True);
end;

{ before the wizard: DefaultDirName needs InstalledDir }
function InitializeSetup: Boolean;
begin
  FindInstalledObs;
  Result := True;
end;

procedure InitializeWizard;
begin
  TargetPage := CreateInputOptionPage(wpInfoBefore, CustomMessage('TargetTitle'), CustomMessage('TargetDescription'),
    CustomMessage('TargetSubCaption'), True, False);
  if InstalledFound and ReadObsVersion(InstalledDir) then
    TargetPage.Add(FmtMessage(CustomMessage('TargetInstalled'), [InstalledDir, ObsVersionText]))
  else
    TargetPage.Add(CustomMessage('TargetInstalledNotFound'));
  TargetPage.Add(CustomMessage('TargetPortable'));
  TargetPage.Values[0] := True;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpInfoBefore then begin
    WizardForm.PageNameLabel.Caption := CustomMessage('LicenseTitle');
    WizardForm.PageDescriptionLabel.Caption := CustomMessage('LicenseDescription');
    WizardForm.InfoBeforeClickLabel.Caption := CustomMessage('LicenseClick');
  end else if CurPageID = wpSelectDir then begin
    WizardForm.PageNameLabel.Caption := CustomMessage('ObsDirTitle');
    WizardForm.PageDescriptionLabel.Caption := CustomMessage('ObsDirDescription');
    if IsPortable then
      WizardForm.SelectDirLabel.Caption := CustomMessage('ObsDirLabelPortable')
    else
      WizardForm.SelectDirLabel.Caption := CustomMessage('ObsDirLabelInstalled');
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = wpSelectDir) and not IsPortable and InstalledFound;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = TargetPage.ID then begin
    if IsPortable then begin
      if SameDir(WizardForm.DirEdit.Text, InstalledDir) then
        WizardForm.DirEdit.Text := PortableDir;
    end else begin
      if not SameDir(WizardForm.DirEdit.Text, InstalledDir) then
        PortableDir := WizardForm.DirEdit.Text;
      WizardForm.DirEdit.Text := InstalledDir;
      { the installed OBS from the registry is checked here, its folder page is skipped }
      if InstalledFound then
        Result := CheckObs(InstalledDir, False);
    end;
  end else if CurPageID = wpSelectDir then begin
    if IsPortable and InstalledFound and SameDir(ObsDir, InstalledDir) then begin
      if SuppressibleMsgBox(CustomMessage('AskInstalledFolder'), mbConfirmation, MB_YESNO, IDNO) <> IDYES then begin
        Result := False;
        exit;
      end;
      TargetPage.Values[0] := True;
    end;
    Result := CheckObs(ObsDir, IsPortable);
  end else if CurPageID = wpReady then begin
    Result := WaitForObsClosed(ObsDir);
    TargetReady := Result;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
var
  Layout: String;
begin
  if ObsMajor >= 33 then
    Layout := CustomMessage('ReadyLayoutNew')
  else
    Layout := CustomMessage('ReadyLayoutLegacy');
  Result := CustomMessage('ReadyObs') + NewLine + Space + ObsDir + NewLine + Space + ObsVersionText + ', ' + Layout
    + NewLine + NewLine + CustomMessage('ReadyFiles') + NewLine + Space + GetDllDir('') + NewLine + Space
    + GetDataDir('');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    RemoveOldCopies;
end;

function InitializeUninstall: Boolean;
begin
  Result := WaitForObsClosed(RemoveBackslashUnlessRoot(ExpandConstant('{app}')));
end;
