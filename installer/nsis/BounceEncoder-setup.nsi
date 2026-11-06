; SPDX-License-Identifier: GPL-3.0-or-later
; NSIS: instalator "Bounce Encoder" (enkoder AES67 -> Icecast) z kreatorem Modern UI 2, w tym samym
; układzie i z tym samym brandingiem co BounceAES-setup.nsi. makensis działa natywnie na
; macOS/Linux/Windows.
;
; Build (z katalogu installer/nsis):  makensis [-DPAYLOAD=<bezwzględna ścieżka do plików>] BounceEncoder-setup.nsi
;         ->  BounceEncoder-setup.exe obok skryptu
;         PAYLOAD = katalog 64-bitowego buildu z kodekami (MinGW: build-win, MSVC: build\Release):
;                   BounceEncoder.exe, BounceEncoderTray.exe, libmp3lame.dll, libfdk-aac-2.dll
;                   i encoder-licenses\ (teksty licencji kodeków, kopiowane przez cmake/codecs.cmake).
;
; Kreator: Welcome -> Licencja (GPLv3) -> folder -> Instalacja -> Finish.
; Instalacja: pliki (kodeki obok programu: ładowane są tylko z jego folderu), usługa Windows
; "Bounce Encoder" (auto, restart po awarii), źródło Dziennika zdarzeń, reguła zapory dla programu,
; folder %ProgramData%\Bounce Encoder z uprawnieniami tylko dla administratorów i SYSTEM
; (config.json zawiera hasła strumieni) i podfolderem public\ do odczytu dla użytkowników
; (panel.json: adres panelu dla ikony w zasobniku), autostart ikony w zasobniku, skróty.
; Deinstalacja: zatrzymuje i usuwa usługę, usuwa regułę zapory, źródło Dziennika zdarzeń, skróty
; i pliki; konfigurację w %ProgramData%\Bounce Encoder usuwa tylko na wyraźne życzenie.
; Tylko Windows 10/11 x64 (enkoder jest wyłącznie 64-bitowy).

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinVer.nsh"

!ifndef PAYLOAD
  !define PAYLOAD "${__FILEDIR__}\..\..\build-win"
!endif
!define ROOT "${__FILEDIR__}\..\.."  ; paths are relative to this script, not to the working directory

!define APP "Bounce Encoder"
!define PUBLISHER "BOUNCE SP. Z O.O."
!define VER "1.0.0"
!define SERVICE "BounceEncoder"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\BounceEncoder"
!define EVENTLOG_KEY "SYSTEM\CurrentControlSet\Services\EventLog\Application\Bounce Encoder"
!define PANEL_URL "http://127.0.0.1:8070"
; well-known SIDs: the group names are translated (e.g. "Administratorzy" on Polish Windows)
!define SID_ADMINS "*S-1-5-32-544"
!define SID_SYSTEM "*S-1-5-18"
!define SID_USERS "*S-1-5-32-545"
; the folder's DACL: protected (P), auto-inherited (AI); full access (FA) for Administrators (BA)
; and SYSTEM (SY), inherited by subfolders and files (OICI)
!define FOLDER_SDDL "D:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)"
; the program folder outside Program Files: the same plus read & execute (FRFX) for Users (BU), so
; that no user can replace BounceEncoder.exe or a codec DLL the LocalSystem service loads
!define PROGRAM_SDDL "D:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;FRFX;;;BU)"

Name "${APP}"
OutFile "BounceEncoder-setup.exe"
InstallDir "$PROGRAMFILES64\Bounce Encoder"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
InstallColors F72868 050505

VIProductVersion "1.0.0.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP} - instalator"
VIAddVersionKey "FileVersion" "${VER}"
VIAddVersionKey "LegalCopyright" "GPL-3.0-or-later"

!define MUI_ICON "${ROOT}\tray\bounce.ico"
!define MUI_UNICON "${ROOT}\tray\bounce.ico"
!define MUI_ABORTWARNING

!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "${__FILEDIR__}\branding\header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "${__FILEDIR__}\branding\header.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${__FILEDIR__}\branding\wizard.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${__FILEDIR__}\branding\wizard.bmp"

!define MUI_WELCOMEPAGE_TITLE "$(WelcomeTitle)"
!define MUI_WELCOMEPAGE_TEXT "$(WelcomeText)"
!insertmacro MUI_PAGE_WELCOME
!define MUI_LICENSEPAGE_CHECKBOX
!define MUI_LICENSEPAGE_CHECKBOX_TEXT "$(LicCheck)"
!insertmacro MUI_PAGE_LICENSE "${ROOT}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\BounceEncoderTray.exe"
!define MUI_FINISHPAGE_RUN_TEXT "$(RunTray)"
!define MUI_FINISHPAGE_SHOWREADME "${PANEL_URL}"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "$(OpenPanel)"
!define MUI_FINISHPAGE_TEXT "$(FinishText)"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "Polish"
!insertmacro MUI_LANGUAGE "English"

LangString WelcomeTitle ${LANG_POLISH} "Instalacja Bounce Encoder"
LangString WelcomeTitle ${LANG_ENGLISH} "Bounce Encoder Setup"
LangString WelcomeText ${LANG_POLISH} "Kreator zainstaluje Bounce Encoder: usługę, która odbiera strumienie AES67, miksuje je w sygnał stacji i wysyła do serwerów Icecast (MP3, AAC, HE-AAC, FLAC), ikonę w zasobniku oraz panel WWW.$\r$\n$\r$\nKodeki: LAME (LGPL) i Fraunhofer FDK AAC. Licencja FDK AAC nie jest zgodna z GPL, a AAC jest objęty patentami: instalator z kodekiem AAC jest przeznaczony do użytku wewnętrznego (LICENCJE.md).$\r$\n$\r$\nPo instalacji otwórz panel (${PANEL_URL}) i ustaw hasło.$\r$\n$\r$\nKliknij Dalej, aby kontynuować."
LangString WelcomeText ${LANG_ENGLISH} "This wizard installs Bounce Encoder: the service that receives AES67 streams, mixes them into a station signal and sends it to Icecast servers (MP3, AAC, HE-AAC, FLAC), the tray icon and the web panel.$\r$\n$\r$\nCodecs: LAME (LGPL) and Fraunhofer FDK AAC. The FDK AAC licence is not GPL-compatible and AAC is covered by patents: an installer with the AAC codec is for internal use (LICENCJE.md).$\r$\n$\r$\nAfter installation open the panel (${PANEL_URL}) and set a password.$\r$\n$\r$\nClick Next to continue."
LangString LicCheck ${LANG_POLISH} "Zapoznałem się z licencją GPLv3 i akceptuję jej warunki."
LangString LicCheck ${LANG_ENGLISH} "I have read and accept the GPLv3 license."
LangString RunTray ${LANG_POLISH} "Uruchom ikonę Bounce Encoder w zasobniku"
LangString RunTray ${LANG_ENGLISH} "Start the Bounce Encoder tray icon"
LangString OpenPanel ${LANG_POLISH} "Otwórz panel w przeglądarce (ustaw hasło)"
LangString OpenPanel ${LANG_ENGLISH} "Open the web panel (set the password)"
LangString FinishText ${LANG_POLISH} "Bounce Encoder został zainstalowany i działa.$\r$\n$\r$\nAutostart: usługa uruchamia się razem z Windows, ikona w zasobniku przy każdym logowaniu. Skrót $\"Bounce Encoder$\" jest na pulpicie i w menu Start.$\r$\n$\r$\nW panelu wybierz kartę sieciową AoIP (Ustawienia…) i dodaj pierwszą stację."
LangString FinishText ${LANG_ENGLISH} "Bounce Encoder has been installed and is running.$\r$\n$\r$\nAutostart: the service starts with Windows, the tray icon at every logon. A $\"Bounce Encoder$\" shortcut is on the desktop and in the Start menu.$\r$\n$\r$\nIn the panel select the AoIP network card (Ustawienia…) and add the first station."
LangString Need64 ${LANG_POLISH} "Bounce Encoder wymaga 64-bitowego systemu Windows 10 lub 11."
LangString Need64 ${LANG_ENGLISH} "Bounce Encoder requires 64-bit Windows 10 or 11."
LangString PtpWarn ${LANG_POLISH} "Włączony jest klient PTP usługi czasu Windows (w32time). Zajmuje on porty 319/320 i uniemożliwi synchronizację Bounce Encoder z zegarem PTP.$\r$\n$\r$\nWyłącz go po instalacji (opis w INSTRUKCJA.md, rozdział 11)."
LangString PtpWarn ${LANG_ENGLISH} "The Windows Time service PTP client (w32time) is enabled. It occupies ports 319/320 and prevents Bounce Encoder from synchronising to the PTP clock.$\r$\n$\r$\nDisable it after installation (see INSTRUKCJA.md, section 11)."
LangString AclWarn ${LANG_POLISH} "Nie udało się ustawić uprawnień folderu $APPDATA\Bounce Encoder (kod $1). Plik config.json z hasłami strumieni może być czytelny dla innych użytkowników tego komputera: ustaw uprawnienia ręcznie (INSTRUKCJA.md, rozdział 2)."
LangString AclWarn ${LANG_ENGLISH} "Could not set the permissions of $APPDATA\Bounce Encoder (code $1). config.json with the stream passwords may be readable by other users of this computer: set the permissions by hand (INSTRUKCJA.md, section 2)."
LangString InstAclWarn ${LANG_POLISH} "Nie udało się ustawić uprawnień folderu programu $INSTDIR (kod $1). Inni użytkownicy tego komputera mogą podmienić pliki usługi Bounce Encoder: zainstaluj program w Program Files albo ustaw uprawnienia ręcznie (administratorzy i SYSTEM: pełna kontrola, Użytkownicy: odczyt i wykonanie)."
LangString InstAclWarn ${LANG_ENGLISH} "Could not set the permissions of the program folder $INSTDIR (code $1). Other users of this computer may replace the Bounce Encoder service files: install the program in Program Files or set the permissions by hand (Administrators and SYSTEM: full control, Users: read & execute)."
LangString ForeignDirWarn ${LANG_POLISH} "Folder $INSTDIR już istniał i nie jest folderem Bounce Encoder, więc jego uprawnienia nie zostały zmienione. Jeśli inni użytkownicy tego komputera mogą w nim zapisywać, mogą też podmienić pliki usługi: zainstaluj program w Program Files albo w nowym folderze."
LangString ForeignDirWarn ${LANG_ENGLISH} "The folder $INSTDIR already existed and is not a Bounce Encoder folder, so its permissions were not changed. If other users of this computer can write to it, they can also replace the service files: install the program in Program Files or in a new folder."
LangString RootDir ${LANG_POLISH} "Wybierz folder dla programu (np. $PROGRAMFILES64\Bounce Encoder), nie katalog główny dysku."
LangString RootDir ${LANG_ENGLISH} "Choose a folder for the program (e.g. $PROGRAMFILES64\Bounce Encoder), not the root of a drive."
LangString DelConfig ${LANG_POLISH} "Usunąć także konfigurację i logi Bounce Encoder ($APPDATA\Bounce Encoder)?$\r$\n$\r$\nKonfiguracja zawiera stacje, strumienie z hasłami Icecast i hasło panelu. Wybierz Nie, jeśli zamierzasz zainstalować program ponownie."
LangString DelConfig ${LANG_ENGLISH} "Also delete the Bounce Encoder configuration and logs ($APPDATA\Bounce Encoder)?$\r$\n$\r$\nThe configuration holds the stations, the streams with their Icecast passwords and the panel password. Choose No if you plan to install the program again."

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "$(Need64)"
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "$(Need64)"
    Abort
  ${EndIf}
  SetRegView 64
  ; an update goes to the folder of the installed copy (InstallDirRegKey would read the 32-bit
  ; registry view); a folder given with /D= on the command line wins
  ${If} $INSTDIR == "$PROGRAMFILES64\Bounce Encoder"
    ReadRegStr $0 HKLM "Software\Bounce\Bounce Encoder" "InstallDir"
    ${If} $0 != ""
      StrCpy $INSTDIR $0
    ${EndIf}
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

; Not the root of a drive: Next stays disabled.
Function .onVerifyInstDir
  StrLen $0 $INSTDIR
  ${If} $0 <= 3
    Abort
  ${EndIf}
FunctionEnd

; The same for /D= (silent installation), before anything is stopped.
!macro CHECK_INSTDIR
  StrLen $0 $INSTDIR
  ${If} $0 <= 3
    MessageBox MB_OK|MB_ICONSTOP "$(RootDir)" /SD IDOK
    Abort
  ${EndIf}
!macroend

; The program folder outside $PROGRAMFILES64 (which already allows Users only to read and run)
; gets the same protection: Administrators and SYSTEM full control, Users read & execute,
; protected (nothing inherited from a parent such as D:\ where Users may create and change
; files). Otherwise a local user could replace BounceEncoder.exe or a codec DLL and have the
; LocalSystem service run it. Runs before the files are written (they inherit the DACL), in the
; same order as the data folder below: Administrators take ownership, the folder gets its final
; DACL in one call, then what is inside goes back to inherited ACEs only. Only a folder that is
; ours ($5 = 1: new, or an earlier installation of Bounce Encoder): the ACL of a folder that
; existed with other contents (which could be anything, e.g. a user's profile) is never replaced,
; the user is warned instead.
!macro PROTECT_INSTDIR
  StrCpy $2 "$PROGRAMFILES64\"
  StrLen $3 $2
  StrCpy $4 $INSTDIR $3
  ${If} $4 != $2  ; case-insensitive
  ${AndIf} $5 == 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "$(ForeignDirWarn)" /SD IDOK
  ${ElseIf} $4 != $2
    StrCpy $1 0
    nsExec::ExecToLog 'takeown /F "$INSTDIR" /A'
    Pop $0
    nsExec::ExecToLog 'icacls "$INSTDIR" /setowner ${SID_ADMINS} /T /C /Q'
    Pop $0
    ${If} $0 != 0
      StrCpy $1 $0
    ${EndIf}
    nsExec::ExecToLog `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$$ErrorActionPreference = 'Stop'; $$d = '$INSTDIR'; $$a = New-Object System.Security.AccessControl.DirectorySecurity; $$a.SetSecurityDescriptorSddlForm('${PROGRAM_SDDL}', 'Access'); (Get-Item -LiteralPath $$d).SetAccessControl($$a)"`
    Pop $0
    ${If} $0 != 0
      ${If} $1 == 0
        StrCpy $1 $0
      ${EndIf}
      nsExec::ExecToLog 'icacls "$INSTDIR" /grant:r ${SID_ADMINS}:(OI)(CI)F ${SID_SYSTEM}:(OI)(CI)F ${SID_USERS}:(OI)(CI)RX'
      Pop $0
      ${If} $0 == 0
        nsExec::ExecToLog 'icacls "$INSTDIR" /inheritance:r'
        Pop $0
      ${EndIf}
    ${EndIf}
    ${If} $0 == 0
      ; an update or an existing folder: what is inside inherits the final DACL (no stray ACEs)
      ${If} ${FileExists} "$INSTDIR\*.*"
        nsExec::ExecToLog 'icacls "$INSTDIR\*" /reset /T /C /Q'
        Pop $0
      ${EndIf}
    ${ElseIf} $1 == 0
      StrCpy $1 $0
    ${EndIf}
    ${If} $1 != 0
      MessageBox MB_OK|MB_ICONEXCLAMATION "$(InstAclWarn)" /SD IDOK
    ${EndIf}
  ${EndIf}
!macroend

; Stops the components so that no file is locked (update and uninstall). net stop waits until the
; service has stopped (its stop wait hint is 10 s); a service that hangs is ended with taskkill.
; The failure actions are cleared first, so that the taskkill fallback cannot make the service
; control manager restart the service while files are replaced or the service is deleted (the
; install section sets them again).
!macro STOP_COMPONENTS
  nsExec::ExecToLog 'taskkill /F /IM BounceEncoderTray.exe'
  Pop $0
  nsExec::ExecToLog 'sc.exe failure ${SERVICE} reset= 0 actions= ""'
  Pop $0
  nsExec::ExecToLog 'net.exe stop ${SERVICE}'
  Pop $0
  nsExec::ExecToLog 'taskkill /F /IM BounceEncoder.exe'
  Pop $0
  Sleep 500
!macroend

Section "Bounce Encoder" SecMain
  SectionIn RO
  !insertmacro CHECK_INSTDIR
  !insertmacro STOP_COMPONENTS
  SetShellVarContext all
  ; $5 = 1: the program folder is ours to protect (new, or an earlier installation)
  StrCpy $5 1
  ${If} ${FileExists} "$INSTDIR\*.*"
  ${AndIfNot} ${FileExists} "$INSTDIR\BounceEncoder.exe"
    StrCpy $5 0
  ${EndIf}
  SetOutPath "$INSTDIR"
  !insertmacro PROTECT_INSTDIR

  File "${PAYLOAD}\BounceEncoder.exe"
  File "${PAYLOAD}\BounceEncoderTray.exe"
  ; run-time loaded codecs: only ever loaded from the program's own folder
  File "${PAYLOAD}\libmp3lame.dll"
  File "${PAYLOAD}\libfdk-aac-2.dll"
  File "/oname=LICENSE.txt" "${ROOT}\LICENSE"
  File "/oname=LICENCJE.md" "${ROOT}\LICENSING.md"
  File "/oname=INSTRUKCJA.md" "${ROOT}\docs\ENCODER-INSTALL.md"
  File "/oname=TESTY.md" "${ROOT}\docs\ENCODER-TESTING.md"
  File "/oname=przyklad-orban.sdp" "${ROOT}\tests\data\orban.sdp"
  File "${ROOT}\tray\bounce.ico"
  SetOutPath "$INSTDIR\licencje"
  File "${PAYLOAD}\encoder-licenses\LAME-COPYING.txt"
  File "${PAYLOAD}\encoder-licenses\fdk-aac-NOTICE.txt"
  File "${PAYLOAD}\encoder-licenses\libogg-COPYING.txt"
  File "${PAYLOAD}\encoder-licenses\flac-COPYING.Xiph.txt"
  File "${PAYLOAD}\encoder-licenses\libsamplerate-COPYING.txt"
  SetOutPath "$INSTDIR"

  ; --- %ProgramData%\Bounce Encoder: administrators and SYSTEM only (config.json holds the stream
  ;     passwords and the panel password hash), inherited by everything the service writes there;
  ;     public\ adds read access for Users (panel.json for the tray, replaced by a rename at every
  ;     service start: a file created in public\ inherits the folder's ACEs, so the rename keeps them).
  ;     The folder may exist already, even created by another user (who would own it, with an
  ;     implicit right to change its ACL, and may have added ACEs of their own). Order:
  ;       1. Administrators take ownership of the folder and everything in it;
  ;       2. the folder gets its final DACL in one call: protected (nothing inherited from
  ;          %ProgramData%), Administrators and SYSTEM full control, inheritable. At no moment does
  ;          the folder or a file in it inherit the Users access of %ProgramData% (a /reset of the
  ;          folder itself would, until the next steps ran);
  ;       3. everything inside goes back to inherited ACEs only (explicit ACEs removed), and so
  ;          inherits exactly step 2 - only after step 2 succeeded;
  ;       4. Users may read public\.
  ;     Step 2 uses Windows PowerShell: a DirectorySecurity built from the SDDL holds only the DACL,
  ;     so SetAccessControl writes only the DACL (Set-Acl would also rewrite owner, group and SACL,
  ;     which can fail on an update for no reason); if that fails, icacls grants first
  ;     and then cuts inheritance, which never widens the access either. $1 keeps the first error
  ;     code for the warning. ---
  CreateDirectory "$APPDATA\Bounce Encoder"
  CreateDirectory "$APPDATA\Bounce Encoder\public"
  StrCpy $1 0
  nsExec::ExecToLog 'takeown /F "$APPDATA\Bounce Encoder" /A'
  Pop $0
  nsExec::ExecToLog 'icacls "$APPDATA\Bounce Encoder" /setowner ${SID_ADMINS} /T /C /Q'
  Pop $0
  ${If} $0 != 0
    StrCpy $1 $0
  ${EndIf}
  nsExec::ExecToLog `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$$ErrorActionPreference = 'Stop'; $$d = '$APPDATA\Bounce Encoder'; $$a = New-Object System.Security.AccessControl.DirectorySecurity; $$a.SetSecurityDescriptorSddlForm('${FOLDER_SDDL}', 'Access'); (Get-Item -LiteralPath $$d).SetAccessControl($$a)"`
  Pop $0
  ${If} $0 != 0
    ${If} $1 == 0
      StrCpy $1 $0
    ${EndIf}
    nsExec::ExecToLog 'icacls "$APPDATA\Bounce Encoder" /grant:r ${SID_ADMINS}:(OI)(CI)F ${SID_SYSTEM}:(OI)(CI)F'
    Pop $0
    ${If} $0 == 0
      nsExec::ExecToLog 'icacls "$APPDATA\Bounce Encoder" /inheritance:r'
      Pop $0
    ${EndIf}
  ${EndIf}
  ${If} $0 == 0
    ; the folder's DACL is final: its contents may now inherit it
    nsExec::ExecToLog 'icacls "$APPDATA\Bounce Encoder\*" /reset /T /C /Q'
    Pop $0
    ${If} $0 != 0
    ${AndIf} $1 == 0
      StrCpy $1 $0
    ${EndIf}
    nsExec::ExecToLog 'icacls "$APPDATA\Bounce Encoder\public" /grant:r ${SID_USERS}:(OI)(CI)RX'
    Pop $0
    ${If} $0 != 0
    ${AndIf} $1 == 0
      StrCpy $1 $0
    ${EndIf}
  ${ElseIf} $1 == 0
    StrCpy $1 $0
  ${EndIf}
  ${If} $1 != 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "$(AclWarn)" /SD IDOK
  ${EndIf}

  ; --- usługa: start automatyczny, restart po awarii po 5 s (trzykrotnie) ---
  nsExec::ExecToLog 'sc.exe create ${SERVICE} binPath= "\"$INSTDIR\BounceEncoder.exe\"" start= auto DisplayName= "Bounce Encoder"'
  Pop $0
  nsExec::ExecToLog 'sc.exe config ${SERVICE} binPath= "\"$INSTDIR\BounceEncoder.exe\"" start= auto DisplayName= "Bounce Encoder"'
  Pop $0
  nsExec::ExecToLog 'sc.exe description ${SERVICE} "Odbiór strumieni AES67, miks stacji i wysyłanie do serwerów Icecast (Bounce Encoder), panel WWW."'
  Pop $0
  nsExec::ExecToLog 'sc.exe failure ${SERVICE} reset= 86400 actions= restart/5000/restart/5000/restart/5000'
  Pop $0

  ; --- Dziennik zdarzeń: źródło "Bounce Encoder" (usługa je tylko otwiera) ---
  WriteRegExpandStr HKLM "${EVENTLOG_KEY}" "EventMessageFile" "%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\EventLogMessages.dll"
  WriteRegDWORD HKLM "${EVENTLOG_KEY}" "TypesSupported" 7

  ; --- zapora: odbiór AES67 (RTP), PTP 319/320, panel 8070 (reguła na program, nie na porty) ---
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Bounce Encoder"'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Bounce Encoder" dir=in action=allow program="$INSTDIR\BounceEncoder.exe" enable=yes profile=any'
  Pop $0

  ; --- ikona w zasobniku przy logowaniu (bez --panel: logowanie nie otwiera okna) ---
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "Bounce Encoder" '"$INSTDIR\BounceEncoderTray.exe"'

  ; --- skrót na pulpicie (wszyscy użytkownicy): ikona w zasobniku + panel ---
  CreateShortcut "$DESKTOP\Bounce Encoder.lnk" "$INSTDIR\BounceEncoderTray.exe" "--panel" "$INSTDIR\bounce.ico" 0 SW_SHOWNORMAL "" "Panel i stan Bounce Encoder"

  ; --- menu Start ---
  CreateDirectory "$SMPROGRAMS\Bounce Encoder"
  CreateShortcut "$SMPROGRAMS\Bounce Encoder\Bounce Encoder.lnk" "$INSTDIR\BounceEncoderTray.exe" "--panel" "$INSTDIR\bounce.ico"
  ; the panel at the address the service wrote to public\panel.json (fallback 127.0.0.1:8070)
  Delete "$SMPROGRAMS\Bounce Encoder\Panel Bounce Encoder.url"
  CreateShortcut "$SMPROGRAMS\Bounce Encoder\Panel Bounce Encoder.lnk" "$INSTDIR\BounceEncoderTray.exe" "--open-panel" "$INSTDIR\bounce.ico"
  CreateShortcut "$SMPROGRAMS\Bounce Encoder\Instrukcja.lnk" "$INSTDIR\INSTRUKCJA.md"
  CreateShortcut "$SMPROGRAMS\Bounce Encoder\Odinstaluj Bounce Encoder.lnk" "$INSTDIR\uninstall.exe"

  ; --- rejestr: ścieżka + wpis w Aplikacje / Panel sterowania ---
  WriteRegStr HKLM "Software\Bounce\Bounce Encoder" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\bounce.ico"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VER}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://github.com/bounceradiopl/aes"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" 7500
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; --- klient PTP Windows zająłby porty 319/320 ---
  ReadRegDWORD $0 HKLM "SYSTEM\CurrentControlSet\Services\W32Time\TimeProviders\PtpClient" "Enabled"
  ${If} $0 == 1
    MessageBox MB_OK|MB_ICONEXCLAMATION "$(PtpWarn)" /SD IDOK
  ${EndIf}

  nsExec::ExecToLog 'sc.exe start ${SERVICE}'
  Pop $0
SectionEnd

Section "Uninstall"
  !insertmacro STOP_COMPONENTS
  SetShellVarContext all
  nsExec::ExecToLog 'sc.exe delete ${SERVICE}'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Bounce Encoder"'
  Pop $0
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "Bounce Encoder"
  DeleteRegKey HKLM "${EVENTLOG_KEY}"
  Delete "$DESKTOP\Bounce Encoder.lnk"
  RMDir /r "$SMPROGRAMS\Bounce Encoder"

  ; tylko własne pliki, nigdy RMDir /r na $INSTDIR (użytkownik mógł wskazać istniejący folder)
  Delete "$INSTDIR\BounceEncoder.exe"
  Delete "$INSTDIR\BounceEncoderTray.exe"
  Delete "$INSTDIR\libmp3lame.dll"
  Delete "$INSTDIR\libfdk-aac-2.dll"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\LICENCJE.md"
  Delete "$INSTDIR\INSTRUKCJA.md"
  Delete "$INSTDIR\TESTY.md"
  Delete "$INSTDIR\przyklad-orban.sdp"
  Delete "$INSTDIR\bounce.ico"
  Delete "$INSTDIR\licencje\LAME-COPYING.txt"
  Delete "$INSTDIR\licencje\fdk-aac-NOTICE.txt"
  Delete "$INSTDIR\licencje\libogg-COPYING.txt"
  Delete "$INSTDIR\licencje\flac-COPYING.Xiph.txt"
  Delete "$INSTDIR\licencje\libsamplerate-COPYING.txt"
  RMDir "$INSTDIR\licencje"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "Software\Bounce\Bounce Encoder"
  DeleteRegKey HKLM "${UNINST_KEY}"

  ; konfiguracja (stacje, hasła) i logi zostają, chyba że użytkownik wybierze inaczej;
  ; deinstalacja cicha (/S) zawsze je zostawia
  MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "$(DelConfig)" /SD IDNO IDNO keep_config
    RMDir /r "$APPDATA\Bounce Encoder"
  keep_config:
SectionEnd
