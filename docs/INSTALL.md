# BounceAES Master: instalacja i konfiguracja

## 1. Wymagania

- Windows 10 lub 11, 64-bit.
- Karta sieciowa podłączona do sieci AoIP (najlepiej osobna, w VLAN-ie AoIP).
- W sieci działa grandmaster PTPv2 (domena zgodna z urządzeniami, np. 0). Bez niego sterownik działa na zegarze lokalnym i nie jest zsynchronizowany z siecią.
- Na przełączniku: IGMP snooping z querierem w VLAN-ie AoIP.

## 2. Instalacja

1. Uruchom `BounceAES-Master-setup.exe` i potwierdź uprawnienia administratora.
2. Instalator:
   - kopiuje pliki do `C:\Program Files\BounceAES Master`,
   - instaluje usługę **BounceAES Master** (`BounceAESMaster`, start automatyczny razem z Windows, restart po awarii po 5 s),
   - rejestruje sterownik ASIO **BounceAES Master**,
   - dodaje regułę zapory dla usługi (RTP 5004, PTP 319/320, SAP 9875, panel 8067),
   - dodaje ikonę w zasobniku do autostartu (przy każdym logowaniu),
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
| zielony | usługa działa, PTP zsynchronizowany |
| żółty | usługa działa, PTP nie jest zsynchronizowany (brak mastera, holdover) |
| czerwony | usługa nie odpowiada |

## 4. Konfiguracja (zakładka Konfiguracja)

### Ustawienia globalne
- **Karta sieciowa AoIP**: karta podłączona do sieci audio. "Automatycznie" wybiera pierwszą aktywną kartę, ale przy kilku kartach wybierz właściwą ręcznie.
- **Domena PTP**: taka jak w grandmasterze (w SDP z Orbana: `a=clock-domain:PTPv2 0`).
- **Poziom nominalny PC**: działa tak jak w Axii. −20 dBFS oznacza tor bez zmian (bit w bit) i jest to ustawienie zalecane. Inne wartości wzmacniają sygnał w stronę aplikacji i tłumią go w stronę sieci o tę samą wartość.
- **Port panelu**: zmiana działa po restarcie usługi.

### Urządzenia (BounceAES 01–32)
Każde urządzenie to para stereo: jedno wyjście do sieci i jedno wejście z sieci.

| Pole | Opis |
|---|---|
| Aktywne | włącza urządzenie |
| Nazwa | pojawia się w nazwach kanałów ASIO, np. "BounceAES 02 L (Kraków)" |
| Wyjście (multicast) | adres strumienia nadawanego do sieci (domyślnie 239.69.100.N), port 5004 |
| SDP | opis wyjścia do skopiowania do odbiornika, który nie korzysta z SAP |
| ptime | 1 ms (standard AES67), 1,333 ms (jak Orban) lub 4 ms (dla odbiorników z małym buforem) |
| Utrzymuj | nadawaj ciszę, gdy aplikacja nie gra (Axia: KEEP CHANNEL ACTIVE). Zalecane przy automatyce emisyjnej |
| Wejście: Przeglądaj… | wybór strumienia ogłaszanego w sieci (SAP) |
| Wejście: SDP… | wklejenie pliku SDP źródła, np. z Orbana |
| Opóźn. ms | opóźnienie odbioru wejścia: bufor na jitter sieci. Minimum to ptime źródła + 1 ms; panel sam podnosi zbyt małą wartość. Zacznij od 5 ms i zwiększ, jeśli rośnie licznik RxUnderrun |

Po zmianach kliknij **Zapisz zmiany**. Zmiany działają od razu. Przebudowywane są tylko zmienione strumienie, pozostałe grają dalej.

### Strumień z Orbana
1. Skopiuj SDP z Orbana (plik z `c=IN IP4 239.1.14.100/15`, `L24/48000/2`, `ptime 1.333`).
2. Przy wybranym urządzeniu kliknij **SDP…**, wklej treść i zapisz.
3. Opóźnienie odbioru zostanie podniesione co najmniej do 2,334 ms (ptime 1,333 ms + 1 ms).

## 5. mAirList (i inne aplikacje ASIO)

1. W konfiguracji urządzeń audio mAirList wybierz typ **ASIO** i sterownik **BounceAES Master**.
2. Rozmiar bufora: 128 próbek (2,67 ms) to dobry punkt wyjścia. Przy trzaskach zwiększ do 256.
3. Częstotliwość próbkowania: **48 000 Hz** (jedyna obsługiwana).
4. Kanały: "BounceAES NN L" i "BounceAES NN R" dla urządzenia NN. Wyjścia trafiają do sieci, wejścia przychodzą z sieci.
5. Z poziomu okna ustawień ASIO ("Panel sterowania") otwiera się panel WWW.

Sterownik obsługuje **jedną aplikację ASIO naraz**. Druga dostanie komunikat "urządzenie zajęte".

## 6. Strojenie Windows i karty sieciowej

Software'owe znaczniki czasu PTP i wysyłanie tysięcy pakietów na sekundę są wrażliwe na zarządzanie energią. Na komputerze emisyjnym:

- **Plan zasilania**: "Wysoka wydajność" lub "Najwyższa wydajność".
- **Karta sieciowa** (Menedżer urządzeń → Właściwości → Zaawansowane):
  - *Interrupt Moderation*: wyłączone,
  - *Energy Efficient Ethernet / Green Ethernet*: wyłączone,
  - *Flow Control*: wyłączone,
  - Zarządzanie energią: odznacz "Zezwalaj komputerowi na wyłączanie tego urządzenia".
- **Klient PTP Windows** (w32time) nie może być włączony, bo zająłby porty 319/320. Instalator ostrzega, jeśli jest aktywny.
- Wyłącz Wi-Fi i inne karty w tej samej podsieci co AoIP.

## 7. Diagnostyka

| Objaw | Sprawdź |
|---|---|
| Ikona żółta, PTP "brak mastera" | domena PTP, czy wybrana karta jest w VLAN-ie AoIP, IGMP/querier na przełączniku, zapora |
| RxUnderrun rośnie | zwiększ opóźnienie odbioru; sprawdź ustawienia karty sieciowej (rozdział 6) |
| RxOverrun rośnie | pakiety spóźnione: zwiększ opóźnienie odbioru |
| TxErr rośnie | komputer nie nadąża z wysyłaniem: plan zasilania, obciążenie CPU, ustawienia karty |
| Brak sygnału na wejściu | czy źródło nadaje (Przeglądaj…), czy adres/port w SDP jest poprawny, czy przełącznik przepuszcza multicast |
| "usługa nie działa" w mAirList | `services.msc` → BounceAES Master → Uruchom; logi w `C:\ProgramData\BounceAES Master\logs` |
| Ostrzeżenie o konflikcie adresu | inne urządzenie nadaje na tym samym adresie multicast: zmień adres wyjścia |

Uruchomienie usługi w oknie konsoli (do diagnostyki, po zatrzymaniu usługi Windows):
`"C:\Program Files\BounceAES Master\BounceAESService.exe" --console`
