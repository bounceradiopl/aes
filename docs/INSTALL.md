# BounceAES Master: instalacja i konfiguracja

## 1. Wymagania

- Windows 10 lub 11, 64-bit.
- Karta sieciowa podłączona do sieci AoIP (najlepiej osobna, w VLAN-ie AoIP).
- Zegar PTPv2 w sieci: grandmaster (np. Orban, Omnia, konsola) albo jeden z komputerów BounceAES Master w roli mastera (rozdział 6).
- Do przechwytywania dźwięku pojedynczych aplikacji: Windows 11.
- Na przełączniku: IGMP snooping z querierem w VLAN-ie AoIP.

## 2. Instalacja

1. Uruchom `BounceAES-Master-setup.exe` i potwierdź uprawnienia administratora.
2. Instalator:
   - kopiuje pliki do `C:\Program Files\BounceAES Master`,
   - instaluje usługę **BounceAES Master** (`BounceAESMaster`, start automatyczny razem z Windows, restart po awarii po 5 s),
   - rejestruje sterownik ASIO **BounceAES Master** w dwóch wersjach, jak Axia: 64-bitowej (`BounceAESAsio.dll`) dla programów 64-bitowych i 32-bitowej (`BounceAESAsio32.dll`) dla programów 32-bitowych (np. 32-bitowego mAirList),
   - dodaje regułę zapory dla usługi (RTP 5004, PTP 319/320, SAP 9875, panel 8067),
   - dodaje ikonę w zasobniku do autostartu (przy każdym logowaniu); ikona uruchamia też BounceAES Audio Bridge (audio Windows),
   - tworzy skrót **BounceAES Master** na pulpicie i w menu Start.
3. Pasek zadań: ikona BounceAES Master w obszarze powiadomień jest zawsze widoczna. Aby mieć też przypięty skrót, kliknij prawym przyciskiem skrót na pulpicie → *Pokaż więcej opcji* → *Przypnij do paska zadań* (Windows nie pozwala instalatorom robić tego samodzielnie).
4. Bez instalatora (np. przy testowaniu buildu z CI): w PowerShellu jako administrator, w folderze z plikami, uruchom
   `powershell -ExecutionPolicy Bypass -File install.ps1`. Odinstalowanie: `uninstall.ps1`.

Konfiguracja i logi znajdują się w `C:\ProgramData\BounceAES Master\` (`config.json`, `logs\bounceaes.log`). Ostrzeżenia i błędy trafiają też do Dziennika zdarzeń Windows (Aplikacja, źródło "BounceAES Master").

## 3. Pierwsze uruchomienie panelu

1. Kliknij dwukrotnie ikonę BounceAES Master w zasobniku albo otwórz `http://127.0.0.1:8067` na tym komputerze.
2. Ustaw hasło panelu (co najmniej 8 znaków). Pierwsze hasło można ustawić **tylko z tego komputera**.
3. Panel jest dostępny również z innych komputerów pod `http://<adres-komputera>:8067`, po zalogowaniu. Jeśli panel ma być dostępny z zewnątrz, wystaw go przez odwrotne proxy z HTTPS.

Kolory ikony w zasobniku:

| Kolor | Znaczenie |
|---|---|
| zielony | usługa działa, PTP zsynchronizowany albo ten komputer jest masterem |
| żółty | usługa działa, PTP nie jest zsynchronizowany (brak mastera, holdover) |
| czerwony | usługa nie odpowiada |

## 4. Konfiguracja (zakładka Konfiguracja)

### Ustawienia globalne
- **Nazwa stacji**: trafia do nazw strumieni w sieci jako „stacja / wyjście”, np. „STUDIO-1 / Warszawa”, żeby wyjścia różnych komputerów Bounce były rozróżnialne. Puste pole oznacza nazwę komputera.
- **Karta sieciowa AoIP**: karta podłączona do sieci audio. „Automatycznie” wybiera pierwszą aktywną kartę, ale przy kilku kartach wybierz właściwą ręcznie.
- **Domena PTP**: taka jak w grandmasterze (w SDP z Orbana: `a=clock-domain:PTPv2 0`).
- **Rola PTP** (rozdział 6):
  - *automatyczna* (domyślna, priorytet 250): komputer zostaje masterem tylko wtedy, gdy w sieci nie ma lepszego zegara;
  - *preferowany master* (priorytet 100): komputer wygrywa z urządzeniami o domyślnym priorytecie 128 — **może przejąć zegar całej sieci AoIP**;
  - *tylko slave*: komputer nigdy nie nadaje zegara.
- **Priorytet PTP 1 / 2**: im niższy, tym ważniejszy zegar (0–255).
- **Poziom nominalny PC**: działa tak jak w Axii. −20 dBFS oznacza tor bez zmian (bit w bit) i jest to ustawienie zalecane. Inne wartości wzmacniają sygnał w stronę aplikacji i tłumią go w stronę sieci o tę samą wartość.
- **Port panelu**: zmiana działa po restarcie usługi.

### Wyjścia AoIP (1–32)
Każde wyjście to strumień stereo nadawany do sieci. Co na nie trafia, ustala zakładka **Routing**. Nowe wyjście najprościej dodać przyciskiem **Nadawaj do urządzenia…** (rozdział „Nadawanie do Orbana lub Omnii” niżej).

| Pole | Opis |
|---|---|
| Aktywne | włącza wyjście (strumień i ogłoszenie SAP) |
| Nazwa | druga część nazwy strumienia w sieci |
| Adres multicast | adres strumienia (domyślnie 239.69.100.N), port 5004; **SDP** pokazuje opis strumienia: *Kopiuj* albo *Pobierz plik .sdp* dla odbiornika, który nie korzysta z SAP |
| ptime | 1 ms (standard AES67), 1,333 ms (jak Orban) lub 4 ms (dla odbiorników z małym buforem) |
| Utrzymuj | nadawaj ciszę, gdy aplikacja ASIO nie gra (Axia: KEEP CHANNEL ACTIVE). Wyjścia zasilane z sieci lub z Windows nadają zawsze |
| Źródło | aktualne połączenie z zakładki Routing |
| **Usuń** (pierwsza kolumna) | od razu, po potwierdzeniu: wyłącza wyjście, rozłącza je i wycofuje ogłoszenie SAP; nazwa i adres wracają do domyślnych |

### Subskrypcje (1–32)
Subskrypcja to odbierany strumień stereo z sieci (Orban, Omnia, konsola, inny komputer Bounce). **Dodaj źródło…** pokazuje strumienie ogłaszane w sieci (SAP); **Wklej SDP…** przyjmuje plik SDP, np. z Orbana.

| Pole | Opis |
|---|---|
| Aktywna | włącza odbiór |
| Nazwa | nazwa w siatce routingu (domyślnie nazwa sesji z SDP) |
| Źródło | nazwa i adres z SDP; kropka pokazuje, czy sygnał dociera; **SDP…** zmienia źródło |
| Opóźn. ms | opóźnienie odbioru: bufor na jitter sieci. Minimum to ptime źródła + 1 ms; panel sam podnosi zbyt małą wartość. Zacznij od 5 ms i zwiększ, jeśli rośnie licznik RxUnderrun |
| **Usuń** (pierwsza kolumna) | od razu, po potwierdzeniu: usuwa subskrypcję i rozłącza wszystko, co zasilała (okno potwierdzenia wymienia te połączenia) |

Usuwanie i dodawanie (źródeł i wyjść) działa od razu, bez „Zapisz zmiany”; niezapisane zmiany w innych wierszach zostają. To samo okno otwiera się w zakładce **Routing** po kliknięciu nazwy subskrypcji (wiersz) albo wyjścia AoIP (kolumna): adres, połączenia, SDP i **Usuń**.

### Nazwy par ASIO
Nazwy 32 par, które widzą aplikacje ASIO, np. „BounceAES 02 L (Kraków)”.

Po zmianach kliknij **Zapisz zmiany**. Zmiany działają od razu. Przebudowywane są tylko zmienione strumienie, pozostałe grają dalej.

### Strumień z Orbana
1. **Dodaj źródło…** i wybierz Orbana z listy albo kliknij **Wklej SDP…** i wklej plik SDP (np. `c=IN IP4 239.1.14.100/15`, `L24/48000/2`, `ptime 1.333`).
2. Opóźnienie odbioru zostanie podniesione co najmniej do 2,334 ms (ptime 1,333 ms + 1 ms).
3. W zakładce Routing połącz subskrypcję z celem, np. z wejściem ASIO albo słuchawkami.

### Nadawanie do Orbana lub Omnii
1. **Routing** (albo Konfiguracja → Wyjścia AoIP) → **Nadawaj do urządzenia…**
2. Wpisz nazwę (np. „Orban Warszawa”), wybierz, co nadawać (np. *ASIO: Program* z mAirList), pakiet zostaw **1 ms**, adres zostaw pusty — panel wybierze wolny.
3. **Utwórz**: wyjście zaczyna nadawać od razu, a okno pokazuje gotowe SDP. **Pobierz plik .sdp** zapisuje plik `STACJA - nazwa.sdp`.
4. Wczytaj plik w odbiorniku (Orban, Omnia: wejście AES67 → SDP). Urządzenia obsługujące SAP widzą strumień „STACJA / nazwa” bez pliku.

Ten sam plik pobierzesz później przyciskiem **SDP** przy wyjściu albo z okna wyjścia w Routingu.

### Aktualizacja z wersji 1.0
Konfiguracja 1.0 przenosi się sama przy pierwszym uruchomieniu 1.1 (kopia: `config.json.v1.bak`). Urządzenie N staje się wyjściem N (zasilanym z ASIO N), subskrypcją N (jeśli miało źródło, połączoną z wejściem ASIO N) i nazwą pary ASIO N, więc wszystko gra tak jak wcześniej.

## 5. Routing (zakładka Routing)

Siatka jak w Dante Controller: **wiersze to źródła, kolumny to cele**. Kliknięcie kratki łączy źródło z celem od razu, drugie kliknięcie rozłącza. Każdy cel ma jedno źródło; jedno źródło może zasilać wiele celów. Grupy (Sieć AoIP, ASIO, Windows) można zwijać, a „tylko używane” ukrywa nieużywane wiersze i kolumny. Szara kratka oznacza połączenie ze źródłem chwilowo niedostępnym (np. aplikacja zamknięta). Kliknięcie nazwy subskrypcji albo wyjścia AoIP otwiera jego okno: adres, połączenia, SDP i **Usuń**. Przyciski **Nadawaj do urządzenia…** i **Dodaj źródło…** dodają wyjście i subskrypcję.

| Źródło | Co to jest |
|---|---|
| Sieć AoIP | subskrypcje |
| ASIO | to, co aplikacja ASIO gra na „BounceAES NN” |
| Windows: aplikacja | dźwięk jednego programu (np. Chrome, Spotify), przechwytywany wprost z procesu — **Windows 11** |
| Windows: Odsłuch: urządzenie | wszystko, co Windows gra na danym urządzeniu (loopback) |
| Windows: wirtualna karta „…: z aplikacji” | to, co aplikacje grają na wirtualnej karcie (np. CABLE Input) |
| Windows: urządzenie nagrywające | mikrofon, wejście liniowe |

| Cel | Co to jest |
|---|---|
| Sieć AoIP | wyjścia AoIP |
| ASIO | wejścia „BounceAES NN”, z których nagrywa aplikacja ASIO |
| Windows | urządzenia odtwarzające: słuchawki, głośniki; wirtualna karta „…: do aplikacji” |

Opóźnienie połączenia zależy od źródła: subskrypcja — jej opóźnienie odbioru; ASIO — jeden bufor; Windows — 20 ms (bufor urządzenia i dopasowanie zegara).

### Audio Windows
Źródła i cele Windows obsługuje **BounceAES Audio Bridge**, uruchamiany przez ikonę w zasobniku w sesji zalogowanego użytkownika (usługa Windows nie ma dostępu do dźwięku użytkownika). Bez zalogowanego użytkownika panel pokazuje „Audio Windows: brak sesji użytkownika”; AoIP i ASIO działają dalej. Urządzenia Windows mają własny zegar, więc bridge dopasowuje je do zegara PTP resamplerem (±1000 ppm).

Zalecenia:
- Ustaw urządzenia Windows na **48 000 Hz** (Panel dźwięku → urządzenie → Właściwości → Zaawansowane). Inne częstotliwości działają, ale wymagają przeliczania.
- Dźwięk przeglądarki do AoIP: w siatce połącz **Windows → Chrome** (albo Edge, Firefox) z wyjściem AoIP. Nie trzeba zmieniać urządzenia w samej przeglądarce.
- Odsłuch strumienia AoIP: połącz subskrypcję z celem **Windows → Słuchawki**.

### Wirtualne karty (VB-CABLE): dla aplikacji, które wymagają karty dźwiękowej
Część programów (np. komunikatory, programy do nagrywania, mAirList bez ASIO) potrzebuje urządzenia audio do wyboru w swoich ustawieniach. Do tego służy darmowy **VB-CABLE** (VB-Audio, donationware, podpisany przez Microsoft — działa przy włączonym Secure Boot). Licencja VB-Audio nie pozwala dołączyć go do instalatora, więc instaluje się go osobno (link jest też na ostatniej stronie instalatora):

1. Pobierz VB-CABLE Driver Pack ze strony vb-audio.com/Cable, rozpakuj i uruchom `VBCABLE_Setup_x64.exe` jako administrator → *Install Driver*. Uruchom komputer ponownie.
2. W Panelu dźwięku ustaw **CABLE Input** (odtwarzanie) i **CABLE Output** (nagrywanie) na 48 000 Hz, 24 bity.
3. W panelu BounceAES: **Routing → Wirtualne karty…**
   - **Do sieci:** przy „CABLE: z aplikacji” wybierz wyjście AoIP → *Wysyłaj*. W aplikacji wybierz wyjście *CABLE Input*.
   - **Z sieci:** przy „CABLE: do aplikacji” wybierz subskrypcję → *Odbieraj*. W aplikacji wybierz wejście *CABLE Output*.

To samo widać w siatce: wirtualne karty są na początku grupy Windows, z oznaczeniem „wirt.”. Drugi koniec kabla (CABLE Output) nie jest pokazywany jako osobne źródło, bo niesie ten sam dźwięk z większym opóźnieniem; połączenia z wersji 1.1 z „CABLE Output” działają dalej i pozostają widoczne.

Rozpoznawane wirtualne karty (po nazwie sterownika, także po zmianie nazwy urządzenia w Windows): VB-CABLE, CABLE-A…D, Hi-Fi Cable, Voicemeeter, Virtual Audio Cable (Line N), Virtual Audio Driver, Steam Streaming, Scream. Jeden VB-CABLE to jedna para stereo; kolejne kable (A+B, C+D) VB-Audio udostępnia za dobrowolną opłatą.

**Otwarte sterowniki (Virtual Audio Driver, Scream).** Stan na październik 2026: nie mają podpisu Microsoftu i na Windows 11 z Secure Boot kończą się błędem 52 („Windows nie może zweryfikować podpisu”). Działają tylko w trybie testowym Windows (wyłączony Secure Boot, `bcdedit /set testsigning on`), czego nie zalecamy na komputerach emisyjnych. Gdy taki sterownik dostanie podpis, wystarczy go zainstalować — BounceAES Master rozpozna go bez aktualizacji.

### Dwa komputery z BounceAES Master
1. Na komputerze A włącz wyjście i połącz je ze źródłem (np. ASIO albo aplikacją Windows). W sieci pojawi się strumień „STACJA-A / nazwa wyjścia”.
2. Na komputerze B: **Dodaj źródło…** → wybierz ten strumień, potem połącz subskrypcję z celem.
3. PTP: przy roli „automatyczna” na obu komputerach jeden z nich zostaje masterem (decyduje identyfikator zegara), drugi się do niego synchronizuje. Jeśli w sieci jest urządzenie z lepszym zegarem (Orban, Omnia, konsola z priorytetem 128 lub lepszą klasą zegara), oba komputery są slave'ami. Gdy master zniknie, rolę przejmuje inny komputer w ciągu kilku sekund.

## 6. PTP: kto jest masterem
- Pasek u góry panelu pokazuje **PTP: master** (ten komputer nadaje zegar) albo **PTP: slave** (podpowiedź pokazuje identyfikator mastera). Zakładka Statystyki pokazuje szczegóły.
- Urządzenia AES67 (Orban, Omnia, konsole) zwykle mają priorytet 128 i lepszą klasę zegara niż komputer (248), więc wygrywają z komputerem w roli automatycznej.
- Rola **preferowany master** jest przeznaczona dla sieci bez sprzętowego grandmastera. Ustaw ją na jednym komputerze.
- Rola **tylko slave** jest właściwa, gdy zegar ma zawsze pochodzić z urządzenia, a komputer nie może go przejąć nawet na chwilę.

## 7. mAirList (i inne aplikacje ASIO)

**Gdzie jest sterownik ASIO.** ASIO nie jest kartą dźwiękową Windows: *nie* pojawia się w Ustawieniach → Dźwięk ani w Panelu dźwięku. Widać go wyłącznie w programie, który obsługuje ASIO (mAirList, Reaper, Audition itp.), na liście sterowników ASIO, pod nazwą **BounceAES Master**. Zwykłe programy Windows łączy się przez zakładkę Routing (rozdział 5) albo VB-CABLE.

1. W konfiguracji urządzeń audio mAirList wybierz typ **ASIO** i sterownik **BounceAES Master**.
2. Rozmiar bufora: 128 próbek (2,67 ms) to dobry punkt wyjścia. Przy trzaskach zwiększ do 256.
3. Częstotliwość próbkowania: **48 000 Hz** (jedyna obsługiwana).
4. Kanały: „BounceAES NN L” i „BounceAES NN R” dla pary NN. Co trafia z wyjść do sieci i co przychodzi na wejścia, ustala zakładka Routing (domyślnie wyjście ASIO N → wyjście AoIP N).
5. Z poziomu okna ustawień ASIO („Panel sterowania”) otwiera się panel WWW.

Sterownik obsługuje **jedną aplikację ASIO naraz**. Druga dostanie komunikat „urządzenie zajęte”.

### Program nie widzi sterownika „BounceAES Master”
1. Upewnij się, że zainstalowana jest wersja **1.1.2 lub nowsza**. Starsze instalowały tylko sterownik 64-bitowy, którego nie widzą programy 32-bitowe (np. 32-bitowy mAirList).
2. Uruchom w wierszu poleceń oba narzędzia z folderu `C:\Program Files\BounceAES Master`:
   - `asio-probe.exe` — co widzą programy **64-bitowe**,
   - `asio-probe32.exe` — co widzą programy **32-bitowe**.

   Każde wypisuje listę sterowników ASIO, ładuje „BounceAES Master” tak jak mAirList i kończy się `OK` albo opisem błędu.
3. Jeśli „BounceAES Master” nie ma na liście: zainstaluj ponownie jako administrator albo zarejestruj ręcznie w wierszu poleceń uruchomionym jako administrator:
   - 64-bit: `C:\Windows\System32\regsvr32.exe "C:\Program Files\BounceAES Master\BounceAESAsio.dll"`
   - 32-bit: `C:\Windows\SysWOW64\regsvr32.exe "C:\Program Files\BounceAES Master\BounceAESAsio32.dll"`
4. Jeśli sterownik jest na liście, ale `init()` zgłasza „usługa nie działa”: uruchom usługę (`services.msc` → BounceAES Master → Uruchom).
5. Po zmianach zamknij i uruchom ponownie mAirList — programy czytają listę sterowników ASIO przy starcie.

## 8. Strojenie Windows i karty sieciowej

Software'owe znaczniki czasu PTP i wysyłanie tysięcy pakietów na sekundę są wrażliwe na zarządzanie energią. Na komputerze emisyjnym:

- **Plan zasilania**: "Wysoka wydajność" lub "Najwyższa wydajność".
- **Karta sieciowa** (Menedżer urządzeń → Właściwości → Zaawansowane):
  - *Interrupt Moderation*: wyłączone,
  - *Energy Efficient Ethernet / Green Ethernet*: wyłączone,
  - *Flow Control*: wyłączone,
  - Zarządzanie energią: odznacz "Zezwalaj komputerowi na wyłączanie tego urządzenia".
- **Klient PTP Windows** (w32time) nie może być włączony, bo zająłby porty 319/320. Instalator ostrzega, jeśli jest aktywny.
- Wyłącz Wi-Fi i inne karty w tej samej podsieci co AoIP.

## 9. Diagnostyka

| Objaw | Sprawdź |
|---|---|
| Ikona żółta, PTP "brak mastera" | domena PTP, czy wybrana karta jest w VLAN-ie AoIP, IGMP/querier na przełączniku, zapora |
| RxUnderrun rośnie | zwiększ opóźnienie odbioru; sprawdź ustawienia karty sieciowej (rozdział 8) |
| RxOverrun rośnie | pakiety spóźnione: zwiększ opóźnienie odbioru |
| TxErr rośnie | komputer nie nadąża z wysyłaniem: plan zasilania, obciążenie CPU, ustawienia karty |
| Brak sygnału w subskrypcji | czy źródło nadaje (Dodaj źródło… pokazuje strumienie SAP), czy adres/port w SDP jest poprawny, czy przełącznik przepuszcza multicast |
| „Audio Windows: brak sesji użytkownika” | czy ikona BounceAES Master jest w zasobniku (uruchamia bridge); zaloguj się na komputerze |
| Aplikacja Windows „wymaga Windows 11” | przechwytywanie procesu działa od Windows 11; na Windows 10 użyj „Odsłuch: urządzenie” albo VB-CABLE |
| Aplikacja Windows „aplikacja nie działa” | program jest zamknięty; bridge sprawdza co 2 s i podłącza się, gdy program wystartuje |
| Dwa komputery jednocześnie „PTP: master” | sprawdź domenę PTP i czy oba są w tej samej sieci/VLAN-ie (multicast 224.0.1.129) |
| mAirList nie pokazuje sterownika ASIO | rozdział 7, „Program nie widzi sterownika”: `asio-probe.exe` i `asio-probe32.exe` |
| "usługa nie działa" w mAirList | `services.msc` → BounceAES Master → Uruchom; logi w `C:\ProgramData\BounceAES Master\logs` |
| Ostrzeżenie o konflikcie adresu | inne urządzenie nadaje na tym samym adresie multicast: zmień adres wyjścia |

Uruchomienie usługi w oknie konsoli (do diagnostyki, po zatrzymaniu usługi Windows):
`"C:\Program Files\BounceAES Master\BounceAESService.exe" --console`

## 10. Podpisywanie plików (wewnętrzne)

Pliki BounceAES Master nie są podpisane komercyjnym certyfikatem. Do użytku wewnętrznego można je podpisać własnym, darmowym certyfikatem firmy i zaufać mu na komputerach firmowych. Nigdy nie używaj certyfikatu ani nazwy innego producenta.

Na komputerze, który buduje wydanie (PowerShell):

```powershell
# jednorazowo: utworzenie certyfikatu i eksport części publicznej
.\installer\sign.ps1 -CreateCert
# podpisanie plików w folderze (np. z artefaktów CI) i instalatora
.\installer\sign.ps1 -Path .\dist
.\installer\sign.ps1 -Path .\installer\nsis
```

Na każdym komputerze firmowym, jako administrator (albo przez zasady grupy):

```powershell
.\installer\sign.ps1 -Trust -Cer .\bounce-codesign.cer
```

Certyfikat trafia do magazynów *Zaufane główne urzędy certyfikacji* i *Zaufani wydawcy* komputera. Klucz prywatny zostaje tylko na komputerze, który podpisuje (eksport z hasłem do kopii zapasowej: `certmgr.msc` → Osobisty).
