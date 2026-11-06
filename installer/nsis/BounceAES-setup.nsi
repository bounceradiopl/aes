; SPDX-License-Identifier: GPL-3.0-or-later
; NSIS: instalator "BounceAES Master" z kreatorem Modern UI 2 (ten sam układ i branding co
; instalator aplikacji bounce). makensis działa natywnie na macOS/Linux/Windows.
;
; Build (z katalogu installer/nsis):  makensis [-DPAYLOAD=<bezwzględna ścieżka do plików>] BounceAES-setup.nsi
;         ->  BounceAES-Master-setup.exe obok skryptu
;         PAYLOAD = katalog z BounceAESService.exe, BounceAESAsio.dll, BounceAESTray.exe, BounceAESAudioBridge.exe,
;                   bounceaes-test.exe, ptp-sim.exe (build MinGW albo MSVC).
;         PAYLOAD32 = katalog z 32-bitowym BounceAESAsio.dll (sterownik ASIO dla programów 32-bitowych,
;                   np. 32-bitowego mAirList); instalowany jako BounceAESAsio32.dll.
;
; Kreator: Welcome -> Licencja (GPLv3) -> folder -> Instalacja -> Finish.
; Instalacja: pliki, usługa Windows (auto, restart po awarii), rejestracja sterownika ASIO,
; reguła zapory, Dziennik zdarzeń, autostart (usługa + ikona w zasobniku), skróty na pulpicie i w menu Start.
; Pasek zadań: Windows nie pozwala instalatorom przypinać programów; ikona w zasobniku sama oznacza
; się jako zawsze widoczna (Windows 11), a skrót z pulpitu można przypiąć jednym kliknięciem.
; Deinstalacja: zatrzymuje i usuwa usługę, wyrejestrowuje ASIO, usuwa pliki; konfiguracja
; w %ProgramData%\BounceAES Master zostaje.

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinVer.nsh"

!ifndef PAYLOAD
  !define PAYLOAD "${__FILEDIR__}\..\..\build-win"
!endif
!ifndef PAYLOAD32
  !define PAYLOAD32 "${__FILEDIR__}\..\..\build-win32"
!endif
!define ROOT "${__FILEDIR__}\..\.."  ; paths are relative to this script, not to the working directory

!define APP "BounceAES Master"
!define PUBLISHER "BOUNCE SP. Z O.O."
!define VER "1.2.4"
!define SERVICE "BounceAESMaster"
!define CLSID "{6B1F3C2A-8E4D-4B7A-9C51-2A7E0D4B3F10}"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\BounceAESMaster"
!define EVENTLOG_KEY "SYSTEM\CurrentControlSet\Services\EventLog\Application\BounceAES Master"
!define PANEL_URL "http://127.0.0.1:8067"

Name "${APP}"
OutFile "BounceAES-Master-setup.exe"
InstallDir "$PROGRAMFILES64\BounceAES Master"
InstallDirRegKey HKLM "Software\Bounce\BounceAES Master" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
InstallColors F72868 050505

VIProductVersion "1.2.4.0"
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
!define MUI_FINISHPAGE_RUN "$INSTDIR\BounceAESTray.exe"
!define MUI_FINISHPAGE_RUN_TEXT "$(RunTray)"
!define MUI_FINISHPAGE_SHOWREADME "${PANEL_URL}"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "$(OpenPanel)"
!define MUI_FINISHPAGE_TEXT "$(FinishText)"
!define MUI_FINISHPAGE_LINK "$(CableLink)"
!define MUI_FINISHPAGE_LINK_LOCATION "https://vb-audio.com/Cable/"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "Polish"
!insertmacro MUI_LANGUAGE "English"

LangString WelcomeTitle ${LANG_POLISH} "Instalacja BounceAES Master"
LangString WelcomeTitle ${LANG_ENGLISH} "BounceAES Master Setup"
LangString WelcomeText ${LANG_POLISH} "Kreator zainstaluje sterownik ASIO AES67 BounceAES Master: usługę audio, sterownik ASIO dla mAirList i innych aplikacji, ikonę w zasobniku oraz panel WWW.$\r$\n$\r$\nPo instalacji otwórz panel (${PANEL_URL}) i ustaw hasło.$\r$\n$\r$\nKliknij Dalej, aby kontynuować."
LangString WelcomeText ${LANG_ENGLISH} "This wizard installs the BounceAES Master AES67 ASIO driver: the audio service, the ASIO driver for mAirList and other applications, the tray icon and the web panel.$\r$\n$\r$\nAfter installation open the panel (${PANEL_URL}) and set a password.$\r$\n$\r$\nClick Next to continue."
LangString LicCheck ${LANG_POLISH} "Zapoznałem się z licencją GPLv3 i akceptuję jej warunki."
LangString LicCheck ${LANG_ENGLISH} "I have read and accept the GPLv3 license."
LangString RunTray ${LANG_POLISH} "Uruchom ikonę BounceAES Master w zasobniku"
LangString RunTray ${LANG_ENGLISH} "Start the BounceAES Master tray icon"
LangString OpenPanel ${LANG_POLISH} "Otwórz panel w przeglądarce (ustaw hasło)"
LangString OpenPanel ${LANG_ENGLISH} "Open the web panel (set the password)"
LangString FinishText ${LANG_POLISH} "BounceAES Master został zainstalowany i działa.$\r$\n$\r$\nAutostart: usługa ze sterownikiem uruchamia się razem z Windows, ikona w zasobniku przy każdym logowaniu. Skrót $\"BounceAES Master$\" jest na pulpicie i w menu Start.$\r$\n$\r$\nW mAirList wybierz sterownik ASIO $\"BounceAES Master$\" (48 kHz, bufor 128)."
LangString FinishText ${LANG_ENGLISH} "BounceAES Master has been installed and is running.$\r$\n$\r$\nAutostart: the driver service starts with Windows, the tray icon at every logon. A $\"BounceAES Master$\" shortcut is on the desktop and in the Start menu.$\r$\n$\r$\nIn mAirList select the ASIO driver $\"BounceAES Master$\" (48 kHz, buffer 128)."
LangString CableLink ${LANG_POLISH} "Darmowa wirtualna karta VB-CABLE (dla aplikacji bez ASIO)"
LangString CableLink ${LANG_ENGLISH} "Free VB-CABLE virtual sound card (for applications without ASIO)"
LangString Need64 ${LANG_POLISH} "BounceAES Master wymaga 64-bitowego systemu Windows 10 lub 11."
LangString Need64 ${LANG_ENGLISH} "BounceAES Master requires 64-bit Windows 10 or 11."
LangString PtpWarn ${LANG_POLISH} "Włączony jest klient PTP usługi czasu Windows (w32time). Zajmuje on porty 319/320 i uniemożliwi synchronizację BounceAES Master.$\r$\n$\r$\nWyłącz go po instalacji (opis w INSTRUKCJA.md, rozdział 8)."
LangString PtpWarn ${LANG_ENGLISH} "The Windows Time service PTP client (w32time) is enabled. It occupies ports 319/320 and prevents BounceAES Master from synchronising.$\r$\n$\r$\nDisable it after installation (see INSTALL.md, section 8)."

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
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

; Zatrzymuje składniki, żeby pliki nie były zablokowane (aktualizacja i deinstalacja).
!macro STOP_COMPONENTS
  nsExec::ExecToLog 'taskkill /F /IM BounceAESTray.exe'
  nsExec::ExecToLog 'taskkill /F /IM BounceAESAudioBridge.exe'
  Pop $0
  nsExec::ExecToLog 'sc.exe stop ${SERVICE}'
  Pop $0
  ; sc stop wraca od razu; usługa potrzebuje chwili na zamknięcie wątków i gniazd
  Sleep 3000
!macroend

Section "BounceAES Master" SecMain
  SectionIn RO
  !insertmacro STOP_COMPONENTS
  SetShellVarContext all
  SetOutPath "$INSTDIR"

  File "${PAYLOAD}\BounceAESService.exe"
  File "${PAYLOAD}\BounceAESAsio.dll"
  File "/oname=BounceAESAsio32.dll" "${PAYLOAD32}\BounceAESAsio.dll"
  File "${PAYLOAD}\BounceAESTray.exe"
  File "${PAYLOAD}\BounceAESAudioBridge.exe"
  File "${PAYLOAD}\bounceaes-test.exe"
  File "${PAYLOAD}\ptp-sim.exe"
  File "${PAYLOAD}\asio-probe.exe"
  File "/oname=asio-probe32.exe" "${PAYLOAD32}\asio-probe.exe"
  File "/oname=LICENSE.txt" "${ROOT}\LICENSE"
  File "/oname=INSTRUKCJA.md" "${ROOT}\docs\INSTALL.md"
  File "/oname=TESTY.md" "${ROOT}\docs\TESTING.md"
  File "/oname=przyklad-orban.sdp" "${ROOT}\tests\data\orban.sdp"
  File "${ROOT}\tray\bounce.ico"

  ; --- usługa: start automatyczny, restart po awarii po 5 s (trzykrotnie) ---
  nsExec::ExecToLog 'sc.exe create ${SERVICE} binPath= "\"$INSTDIR\BounceAESService.exe\"" start= auto DisplayName= "BounceAES Master"'
  Pop $0
  nsExec::ExecToLog 'sc.exe config ${SERVICE} binPath= "\"$INSTDIR\BounceAESService.exe\"" start= auto DisplayName= "BounceAES Master"'
  Pop $0
  nsExec::ExecToLog 'sc.exe description ${SERVICE} "Strumienie audio AES67, zegar PTP i panel WWW dla sterownika ASIO BounceAES Master."'
  Pop $0
  nsExec::ExecToLog 'sc.exe failure ${SERVICE} reset= 86400 actions= restart/5000/restart/5000/restart/5000'
  Pop $0

  ; --- sterownik ASIO (COM + lista sterowników ASIO), 64- i 32-bitowy jak w Axii ---
  ; Program 64-bitowy (np. mAirList x64) widzi tylko sterowniki z 64-bitowej części rejestru,
  ; program 32-bitowy tylko z 32-bitowej (WOW6432Node). Instalator jest procesem 32-bitowym,
  ; więc regsvr32 wskazujemy jawnie: Sysnative = 64-bitowy, SysWOW64 = 32-bitowy.
  nsExec::ExecToLog '"$WINDIR\Sysnative\regsvr32.exe" /s "$INSTDIR\BounceAESAsio.dll"'
  Pop $0
  ${If} $0 != 0
    ; regsvr32 nie powiódł się: wpisz klucze bezpośrednio
    WriteRegStr HKCR "CLSID\${CLSID}" "" "BounceAES Master ASIO Driver"
    WriteRegStr HKCR "CLSID\${CLSID}\InprocServer32" "" "$INSTDIR\BounceAESAsio.dll"
    WriteRegStr HKCR "CLSID\${CLSID}\InprocServer32" "ThreadingModel" "Apartment"
    WriteRegStr HKLM "SOFTWARE\ASIO\BounceAES Master" "CLSID" "${CLSID}"
    WriteRegStr HKLM "SOFTWARE\ASIO\BounceAES Master" "Description" "BounceAES Master"
  ${EndIf}
  nsExec::ExecToLog '"$WINDIR\SysWOW64\regsvr32.exe" /s "$INSTDIR\BounceAESAsio32.dll"'
  Pop $0
  ${If} $0 != 0
    SetRegView 32
    WriteRegStr HKCR "CLSID\${CLSID}" "" "BounceAES Master ASIO Driver"
    WriteRegStr HKCR "CLSID\${CLSID}\InprocServer32" "" "$INSTDIR\BounceAESAsio32.dll"
    WriteRegStr HKCR "CLSID\${CLSID}\InprocServer32" "ThreadingModel" "Apartment"
    WriteRegStr HKLM "SOFTWARE\ASIO\BounceAES Master" "CLSID" "${CLSID}"
    WriteRegStr HKLM "SOFTWARE\ASIO\BounceAES Master" "Description" "BounceAES Master"
    SetRegView 64
  ${EndIf}

  ; --- Dziennik zdarzeń ---
  WriteRegExpandStr HKLM "${EVENTLOG_KEY}" "EventMessageFile" "%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\EventLogMessages.dll"
  WriteRegDWORD HKLM "${EVENTLOG_KEY}" "TypesSupported" 7

  ; --- zapora: RTP 5004, PTP 319/320, SAP 9875, panel 8067 (reguła na program) ---
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="BounceAES Master"'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="BounceAES Master" dir=in action=allow program="$INSTDIR\BounceAESService.exe" enable=yes profile=any'
  Pop $0

  ; --- ikona w zasobniku przy logowaniu ---
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "BounceAES Master" '"$INSTDIR\BounceAESTray.exe"'

  ; --- skrót na pulpicie (wszyscy użytkownicy): ikona w zasobniku + panel ---
  CreateShortcut "$DESKTOP\BounceAES Master.lnk" "$INSTDIR\BounceAESTray.exe" "--panel" "$INSTDIR\bounce.ico" 0 SW_SHOWNORMAL "" "Panel i stan BounceAES Master"

  ; --- menu Start ---
  CreateDirectory "$SMPROGRAMS\BounceAES Master"
  CreateShortcut "$SMPROGRAMS\BounceAES Master\BounceAES Master.lnk" "$INSTDIR\BounceAESTray.exe" "--panel" "$INSTDIR\bounce.ico"
  WriteINIStr "$SMPROGRAMS\BounceAES Master\Panel BounceAES Master.url" "InternetShortcut" "URL" "${PANEL_URL}"
  CreateShortcut "$SMPROGRAMS\BounceAES Master\Instrukcja.lnk" "$INSTDIR\INSTRUKCJA.md"
  CreateShortcut "$SMPROGRAMS\BounceAES Master\Odinstaluj BounceAES Master.lnk" "$INSTDIR\uninstall.exe"

  ; --- rejestr: ścieżka + wpis w Aplikacje / Panel sterowania ---
  WriteRegStr HKLM "Software\Bounce\BounceAES Master" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\bounce.ico"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VER}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://github.com/bounceradiopl/aes"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" 7000
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; --- klient PTP Windows zająłby porty 319/320 ---
  ReadRegDWORD $0 HKLM "SYSTEM\CurrentControlSet\Services\W32Time\TimeProviders\PtpClient" "Enabled"
  ${If} $0 == 1
    MessageBox MB_OK|MB_ICONEXCLAMATION "$(PtpWarn)"
  ${EndIf}

  ; new icons: let Explorer drop its cached copies (shortcuts, Apps list)
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  nsExec::ExecToLog 'sc.exe start ${SERVICE}'
  Pop $0
SectionEnd

Section "Uninstall"
  !insertmacro STOP_COMPONENTS
  SetShellVarContext all
  nsExec::ExecToLog 'sc.exe delete ${SERVICE}'
  Pop $0
  nsExec::ExecToLog '"$WINDIR\Sysnative\regsvr32.exe" /s /u "$INSTDIR\BounceAESAsio.dll"'
  Pop $0
  nsExec::ExecToLog '"$WINDIR\SysWOW64\regsvr32.exe" /s /u "$INSTDIR\BounceAESAsio32.dll"'
  Pop $0
  DeleteRegKey HKCR "CLSID\${CLSID}"
  DeleteRegKey HKLM "SOFTWARE\ASIO\BounceAES Master"
  SetRegView 32
  DeleteRegKey HKCR "CLSID\${CLSID}"
  DeleteRegKey HKLM "SOFTWARE\ASIO\BounceAES Master"
  SetRegView 64
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="BounceAES Master"'
  Pop $0
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "BounceAES Master"
  DeleteRegKey HKLM "${EVENTLOG_KEY}"
  Delete "$DESKTOP\BounceAES Master.lnk"
  RMDir /r "$SMPROGRAMS\BounceAES Master"

  ; tylko własne pliki, nigdy RMDir /r na $INSTDIR (użytkownik mógł wskazać istniejący folder)
  Delete "$INSTDIR\BounceAESService.exe"
  Delete "$INSTDIR\BounceAESAsio.dll"
  Delete "$INSTDIR\BounceAESAsio32.dll"
  Delete "$INSTDIR\BounceAESTray.exe"
  Delete "$INSTDIR\BounceAESAudioBridge.exe"
  Delete "$INSTDIR\bounceaes-test.exe"
  Delete "$INSTDIR\ptp-sim.exe"
  Delete "$INSTDIR\asio-probe.exe"
  Delete "$INSTDIR\asio-probe32.exe"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\INSTRUKCJA.md"
  Delete "$INSTDIR\TESTY.md"
  Delete "$INSTDIR\przyklad-orban.sdp"
  Delete "$INSTDIR\bounce.ico"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "Software\Bounce\BounceAES Master"
  DeleteRegKey HKLM "${UNINST_KEY}"
SectionEnd
