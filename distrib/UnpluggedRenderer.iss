; ---------------------------------------------------------------------------
; UnpluggedRenderer.iss -- standalone installer for the Unplugged video
; renderer, for MPC-HC and MPC-BE users who do not install the Unplugged
; application.
;
; WHAT SHIPS
;   unplugged_vr.ax           the filter (friendly name "Unplugged Video
;                             Renderer") and its four property pages
;   onnxruntime.dll           Microsoft's inference engine, loaded by the ML
;                             grade
;   *.plxgb / *.onnx          the ML models. They MUST sit beside the .ax:
;                             the filter resolves them from the directory it
;                             was loaded from, with no error if they are
;                             missing.
;   Readme.txt, LICENSE.txt   instructions and the GPL v3 text
;   Reset_Settings.cmd        removes HKCU\Software\Unplugged\Renderer
;
; The payload is the same build the Unplugged application ships (the folder
; produced by distrib\make_dist.cmd). This installer does not depend on the
; application and does not install it.
;
; REGISTRATION
;   The .ax is registered with regsvr32 AFTER every file is in place, with
;   retries. Two reasons learned from the application installer:
;     - onnxruntime.dll must already be beside the .ax, or a clean install
;       fails to load the module (regsvr32 exit 3);
;     - antivirus scanning the freshly written files can hold them long
;       enough for one regsvr32 run to fail.
;   The regserver flag is deliberately NOT used: a failed registration there
;   aborts the wizard mid-install, while here it is retried and then reported
;   with actionable text.
;
; COEXISTENCE WITH THE UNPLUGGED APPLICATION
;   Both register the same CLSID. Whichever registered last wins, which is
;   harmless (same binary). On uninstall this package unregisters its copy
;   and, if C:\Program Files\Unplugged\filters\unplugged_vr.ax exists,
;   re-registers the application's copy so a separately installed Unplugged
;   keeps working.
;
; BUILD
;   distrib\build_renderer_setup.ps1 -- verifies the payload and version,
;   runs make_dist.cmd, locates Inno Setup 6 and compiles this file.
; ---------------------------------------------------------------------------

#define MyAppName      "Unplugged Video Renderer"
; The PACKAGE version, kept in lockstep with the Unplugged application release.
; build_renderer_setup.ps1 reads MyAppVersion from
; ..\..\unplugged-mpc-hc\distrib\unplugged.iss and refuses to build when the
; two disagree, so the renderer setup and Unplugged 2.2.0 are one release.
#define MyAppVersion   "2.2.0"
#define MyAppPublisher "unplugged"

; The renderer BINARY build (Include\Version.h + revision.h). It names the
; staging folder produced by make_dist.cmd and is gated against the tree, so a
; rebuilt filter cannot ship under a stale folder name. It is deliberately
; separate from MyAppVersion: the package follows the application release,
; the filter inside it is this build (shown in the filter's own properties).
#define RendererBuild  "0.10.9.2637"
#define FiltersSrc     "..\_bin\UnpluggedRenderer-" + RendererBuild

; The VC++ runtime and its check live with the application repo, which is
; where the one tested copy of that logic is kept. Both are gated by the
; build script before ISCC runs.
#define RedistSrc      "..\..\unplugged-mpc-hc\distrib\redist"
#define VcRuntimeIss   "..\..\unplugged-mpc-hc\distrib\vc_runtime.iss"

[Setup]
; A new AppId: this is a different product from Unplugged, MPC-HC and MPC-BE
; and must never upgrade or collide with any of them.
AppId={{6E0F1A46-30F6-4A4B-809B-435CA1E07664}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
; DirectShow CLSIDs are machine-wide HKCR keys; there is no honest per-user
; install of a filter.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE.txt
; A running player holds the .ax mapped. Inno asks it to close through the
; Restart Manager instead of failing halfway with a sharing violation.
CloseApplications=yes
RestartApplications=no
OutputDir=out
#ifdef FastTest
OutputBaseFilename=UnpluggedRenderer-setup-{#MyAppVersion}-TESTONLY
Compression=none
#else
OutputBaseFilename=UnpluggedRenderer-setup-{#MyAppVersion}
Compression=lzma2/max
#endif
SolidCompression=yes
SetupLogging=yes
UninstallDisplayName={#MyAppName} {#MyAppVersion}
UninstallDisplayIcon={app}\unplugged_vr.ax
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
; No install-time task: the renderer is selected in the player, not here.
; The Start menu group is created unconditionally below.

[Files]
; Dependencies first, the filter last -- the order the application installer
; proved necessary for a clean machine.
Source: "{#FiltersSrc}\onnxruntime.dll";          DestDir: "{app}"; Flags: 64bit ignoreversion restartreplace
Source: "{#FiltersSrc}\xgb_gamma_model.plxgb";    DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "{#FiltersSrc}\xgb_highlight_rolloff.plxgb"; DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "{#FiltersSrc}\xgb_shadow_recovery.plxgb"; DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "{#FiltersSrc}\yolo26n-seg_op21.onnx";    DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "{#FiltersSrc}\yolo26n-sem.onnx";         DestDir: "{app}"; Flags: ignoreversion restartreplace

Source: "{#FiltersSrc}\unplugged_vr.ax";          DestDir: "{app}"; Flags: 64bit ignoreversion restartreplace

Source: "Readme-UnpluggedRenderer.txt";           DestDir: "{app}"; DestName: "Readme.txt"; Flags: ignoreversion
Source: "..\LICENSE.txt";                         DestDir: "{app}"; Flags: ignoreversion
Source: "Reset_Settings.cmd";                     DestDir: "{app}"; Flags: ignoreversion

; The runtime belongs in the system, so it is executed from {tmp}, never
; installed as loose DLLs beside the filter.
Source: "{#RedistSrc}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\Readme"; Filename: "{app}\Readme.txt"
Name: "{group}\Reset settings"; Filename: "{app}\Reset_Settings.cmd"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; \
  StatusMsg: "Installing Microsoft Visual C++ 2015-2022 runtime..."; \
  Check: NeedVCRedist; Flags: waituntilterminated

Filename: "{app}\Readme.txt"; Description: "Open the renderer setup instructions"; \
  Flags: postinstall shellexec nowait unchecked

[Code]
// The shared VC++ runtime decision. It defines NeedVCRedist, VcDllSatisfied and
// NeedVCRedistAt; [Run] calls NeedVCRedist. Included here, not at the top:
// its functions are Pascal and must sit inside [Code].
#include VcRuntimeIss

const
  RendererDll = 'unplugged_vr.ax';

// Register with retries. Each failure is logged; the final failure is shown
// to the user with the two things that actually fix it (close the player,
// run as administrator), not a bare "registration failed" code.
function RegisterRenderer(Path: String): Boolean;
var
  rc, attempt: Integer;
begin
  Result := False;
  for attempt := 1 to 6 do
  begin
    if Exec(ExpandConstant('{sys}\regsvr32.exe'), '/s "' + Path + '"', '',
            SW_HIDE, ewWaitUntilTerminated, rc) and (rc = 0) then
    begin
      Log('registered ' + Path);
      Result := True;
      Exit;
    end;
    Log('regsvr32 attempt ' + IntToStr(attempt) + ' failed for ' + Path +
        ', rc=' + IntToStr(rc));
    Sleep(1000);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  dll: String;
begin
  if CurStep = ssPostInstall then
  begin
    dll := ExpandConstant('{app}\' + RendererDll);
    if not RegisterRenderer(dll) then
      MsgBox('Unplugged Video Renderer was installed but could not be registered.' + #13#10 + #13#10 +
             'Close any running MPC-HC / MPC-BE window and run this installer ' +
             'again. If it still fails, run this command from an elevated ' +
             'Command Prompt:' + #13#10 + #13#10 +
             '    regsvr32 "' + dll + '"' + #13#10 + #13#10 +
             'The errors are in the setup log.',
             mbError, MB_OK);
  end;
end;

// Unregister our copy, then restore the Unplugged application's registration
// if that product is installed. Without the second step, uninstalling this
// package would leave a separately installed Unplugged without a renderer.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  rc, attempt: Integer;
  ourDll, appDll: String;
begin
  if CurUninstallStep <> usUninstall then
    Exit;

  ourDll := ExpandConstant('{app}\' + RendererDll);
  for attempt := 1 to 3 do
  begin
    if Exec(ExpandConstant('{sys}\regsvr32.exe'), '/s /u "' + ourDll + '"', '',
            SW_HIDE, ewWaitUntilTerminated, rc) and (rc = 0) then
    begin
      Log('unregistered ' + ourDll);
      Break;
    end;
    Log('regsvr32 /u attempt ' + IntToStr(attempt) + ' failed, rc=' + IntToStr(rc));
    Sleep(500);
  end;

  appDll := ExpandConstant('{autopf}\Unplugged\filters\' + RendererDll);
  if FileExists(appDll) then
  begin
    if RegisterRenderer(appDll) then
      Log('restored the Unplugged application renderer registration: ' + appDll)
    else
      Log('could not restore the Unplugged application renderer: ' + appDll);
  end;
end;