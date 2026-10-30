# BounceAES Master 1.1 — routing, PTP master, audio Windows

- **Data:** 2026-10-30
- **Status:** do przeglądu
- **Bazuje na:** `2026-10-30-bounceaes-master-design.md` (wersja 1.0)

## 1. Cel

Trzy rozszerzenia, dla środowiska wewnętrznego Bounce, bez kosztów licencji i certyfikatów:

1. **Routing jak w Dante Controller**: dowolne źródło (strumień z sieci, wyjście aplikacji ASIO, aplikacja albo urządzenie Windows) do dowolnego celu (wyjście AoIP, wejście ASIO, urządzenie odtwarzające Windows).
2. **PTP master**: komputer z BounceAES Master może nadawać zegar. Gdy w sieci jest lepszy zegar (Orban, Omnia, konsola), ustępuje mu automatycznie.
3. **Audio Windows ("wirtualne karty")** na Windows 11 bez własnego sterownika jądra: przechwytywanie aplikacji, wyjść i wejść Windows oraz odtwarzanie na urządzeniach Windows, w tym na darmowym, podpisanym kablu VB-CABLE.

## 2. Zakres

### W zakresie
- Matryca routingu parami stereo, jedno źródło na cel, rozgałęzianie źródła do wielu celów, bez sumowania (bit w bit).
- Do 32 subskrypcji sieciowych, 32 wyjść AoIP, 32 par ASIO w każdą stronę, 16 źródeł i 16 celów Windows.
- PTP: role "Automatycznie", "Preferowany master", "Tylko slave", wybór mastera według BMCA (IEEE 1588-2008 §9.3).
- BounceAES Audio Bridge: proces w sesji użytkownika (WASAPI shared, przechwytywanie procesu, loopback urządzenia, wejścia i wyjścia Windows) z adaptacyjnym resamplingiem względem PTP.
- Zakładka "Routing" w panelu, API, migracja konfiguracji 1.0 → 1.1.

### Poza zakresem
- Własny sterownik jądra (WDM). Wymaga podpisu Microsoftu (certyfikat EV); opcja na przyszłość.
- Routing pojedynczych kanałów mono, sumowanie i miksowanie, regulacja głośności w routingu.
- Zdalna zmiana routingu na innych komputerach Bounce (każdy komputer ma własny panel).
- Strumienie unicast, częstotliwości inne niż 48 kHz.

## 3. Model routingu

### 3.1 Identyfikatory

| Rodzaj | Źródło (`src`) | Cel (`dst`) |
|---|---|---|
| Sieć | `net-in:N` (subskrypcja N = 1..32) | `net-out:N` (wyjście AoIP N = 1..32) |
| ASIO | `asio-out:N` (aplikacja gra na "BounceAES NN") | `asio-in:N` (aplikacja nagrywa z "BounceAES NN") |
| Windows | `win-app:<plik.exe>`, `win-loop:<id urządzenia>`, `win-in:<id urządzenia>` | `win-out:<id urządzenia>` |

`<id urządzenia>` to identyfikator punktu końcowego MMDevice (`{0.0.0.00000000}.{guid}`). Aplikacje są identyfikowane nazwą pliku, bez rozróżniania wielkości liter, więc routing działa także po ponownym uruchomieniu programu.

### 3.2 Zasady
- Każdy cel ma najwyżej jedno źródło. Źródło może zasilać wiele celów.
- Brak źródła lub źródło niedostępne oznacza ciszę w celu.
- Routing domyślny (i migracja z 1.0): `net-out:N ← asio-out:N` dla aktywnych wyjść oraz `asio-in:N ← net-in:N` dla urządzeń, które miały ustawione źródło.

### 3.3 Oś czasu i opóźnienie źródła
Każde źródło zapisuje próbki na wspólnej osi czasu PTP (pozycja = numer próbki medialnej) i deklaruje **opóźnienie gotowości** `D_src`, czyli o ile za "teraz" dane są już kompletne:

| Źródło | `D_src` |
|---|---|
| `net-in:N` | opóźnienie odbioru subskrypcji (`latency_us`, ≥ ptime + 1 ms) |
| `asio-out:N` | 0 (aplikacja zapisuje z wyprzedzeniem dwóch buforów) |
| `win-*` | bufor bridge'a, domyślnie 20 ms |

Cel czyta próbki z pozycji `teraz − D_src − (bufor celu)`:
- **`net-out`**: pakiet ze znacznikiem `T` zawiera próbki źródła `[T − D_src, T − D_src + P)`. Strumień wyjściowy jest więc źródłem opóźnionym o `D_src`. Dla `asio-out` to obecne zachowanie bez zmian.
- **`asio-in`**: sterownik ASIO czyta parę z ringu źródła z pozycji `S − B − D_src`, gdzie S to początek okresu, a B rozmiar bufora.
- **`win-out`**: bridge czyta z pozycji `teraz − D_src − bufor_render`.

## 4. Pamięć współdzielona v2

- Nazwa: `BounceAESMaster.v2`, z wersją układu 2. Sterownik ASIO 1.0 odmówi połączenia z komunikatem o niezgodnej wersji, więc instalator wymienia oba składniki naraz.
- **Ringi źródeł:**
  - `net-in`: 64 kanały, czyli dawne ringi wejść ASIO;
  - `asio-out`: 64 kanały, bez zmian;
  - `win-src`: 32 kanały (16 par).
- Ringi wejść ASIO znikają, bo ASIO czyta bezpośrednio z ringu wybranego źródła.
- **Tabela routingu ASIO** w nagłówku: dla każdej z 32 par `asio-in` jest rodzaj źródła (`none`, `net`, `asio`, `win`), indeks pary i `D_src` w próbkach. Zapis jest chroniony seqlockiem.
- **Strefa bridge'a:**
  - `bridge_pid`, `bridge_heartbeat`;
  - `win_src_delay` (`D_src` źródeł Windows);
  - dwa bufory tekstowe JSON po 64 KB z wersją (seqlock):
    - **żądania** (usługa → bridge): lista slotów źródeł Windows (slot 1..16 ↔ `win-app`/`win-loop`/`win-in`) i celów (`win-out` ↔ źródło z jego ringiem i `D_src`);
    - **raport** (bridge → usługa): urządzenia odtwarzające i nagrywające (id, nazwa, format, stan), aplikacje z aktywną sesją audio (plik, nazwa, PID) oraz stan każdego slotu (`ok`, `unavailable` z powodem).

## 5. PTP master

### 5.1 Konfiguracja

| Pole | Wartości | Domyślnie |
|---|---|---|
| `ptp_role` | `auto`, `preferred`, `slave` | `auto` |
| `ptp_priority1` | 0–255 | 250 dla `auto`, 100 dla `preferred` (pole edytowalne) |
| `ptp_priority2` | 0–255 | 128 |

Zestaw danych komputera: clockClass 248, clockAccuracy 0xFE, offsetScaledLogVariance 0xFFFF, timeSource 0xA0 (oscylator wewnętrzny), clockIdentity jak dziś.

### 5.2 Działanie (`PtpPort` zastępuje `PtpSlave`)
- **Stany:** `LISTENING` → `MASTER` albo `SLAVE`. W stanie `SLAVE` holdover działa jak dziś.
- **BMCA** porównuje zestawy z Announce wszystkich obcych masterów z własnym zestawem (`compare_announce`). Przy remisie decyduje clockIdentity. Rola `slave` nigdy nie wybiera siebie.
- **Przejście do `MASTER`** następuje, gdy własny zestaw jest najlepszy przez 3 okresy Announce (3 s). Czas zostaje ciągły, bo master nadaje `clock.to_ptp(teraz)` bez skoku.
- **Master nadaje:**
  - Announce co 1 s (logAnnounceInterval 0);
  - Sync dwuetapowy 8/s (logSyncInterval −3) z Follow_Up zawierającym programowy znacznik nadania;
  - Delay_Resp na każdy Delay_Req, ze znacznikiem odbioru.
  Wszystko w domenie z konfiguracji.
- **Przejście do `SLAVE`:** pierwszy lepszy Announce natychmiast zatrzymuje nadawanie. Synchronizacja przebiega jak w 1.0 i przy rozbieżności czasu ponad 1 ms następuje jedna korekta skokowa.
- **Utrata mastera:** brak Announce przez 3 interwały (min. 6 s) daje ponowną ocenę BMCA. W trybie `auto`/`preferred` komputer może wtedy przejąć rolę.

### 5.3 Panel
- W ustawieniach globalnych: rola i priorytety.
- Przy wyborze `preferred` panel ostrzega: "komputer może przejąć zegar całej sieci AoIP".
- Status pokazuje "master" albo "slave → <identyfikator> (<nazwa z SAP, jeśli znana>)".

## 6. BounceAES Audio Bridge

### 6.1 Proces
- Plik `BounceAESAudioBridge.exe`, w sesji użytkownika.
- Uruchamia go i nadzoruje `BounceAESTray.exe`: restart po awarii po 2 s, ponowna próba po 30 s przy trzech awariach z rzędu.
- Łączy się z pamięcią współdzieloną v2 (claim przez `bridge_pid`, jeden bridge na komputer).
- Wątki audio działają z priorytetem MMCSS "Pro Audio".

### 6.2 Urządzenia i aplikacje
- Wyliczanie urządzeń: `IMMDeviceEnumerator` z powiadomieniami o zmianach.
- Lista aplikacji: `IAudioSessionManager2` na urządzeniach odtwarzających (sesje aktywne i nieaktywne, nazwa pliku z PID).
- **`win-app`:** `ActivateAudioInterfaceAsync` z `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` (`PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`), dla PID pierwszego procesu o danej nazwie. Gdy proces się zakończy, bridge szuka nowego co 2 s. Wymaga Windows 11 (build ≥ 22000); na starszym systemie slot ma stan `unavailable: wymaga Windows 11`.
- **`win-loop`:** `IAudioClient` shared z `AUDCLNT_STREAMFLAGS_LOOPBACK` na urządzeniu odtwarzającym.
- **`win-in`:** `IAudioClient` shared na urządzeniu nagrywającym.
- **`win-out`:** `IAudioClient` shared, sterowany zdarzeniami, okres 10 ms.
- Format: format miksu urządzenia (zwykle float32, 48 kHz). Mono jest powielane na L/R. Z kanałów powyżej 2 brane są pierwsze dwa.

### 6.3 Zegar i resampling
- Każdy strumień ma resampler libsamplerate (BSD-2, wbudowany w repozytorium) w trybie zmiennego współczynnika, `SRC_SINC_MEDIUM_QUALITY`.
- **Przechwytywanie:**
  - próbki z urządzenia → resampler (48 kHz × korekta) → FIFO → zapis do ringu `win-src` na pozycjach zegara medialnego;
  - regulator PI utrzymuje zapełnienie FIFO na 10 ms;
  - korekta ograniczona do ±1000 ppm.
- **Odtwarzanie:** odczyt z ringu źródła → resampler → bufor WASAPI; regulator PI utrzymuje zapełnienie bufora urządzenia na 10 ms.
- Częstotliwości inne niż 48 kHz (np. 44,1 kHz) obsługuje ten sam resampler, ze stałym współczynnikiem bazowym.
- `D_src` źródeł Windows wynosi 20 ms (bufor WASAPI + FIFO) i jest zapisywane w pamięci współdzielonej.

### 6.4 Błędy
- Zniknięcie urządzenia (`AUDCLNT_E_DEVICE_INVALIDATED`) lub zamknięcie aplikacji daje stan slotu `unavailable` i ciszę, z ponowną próbą co 2 s.
- Brak bridge'a (nikt nie jest zalogowany) daje ciszę we wszystkich źródłach i celach Windows, a panel pokazuje "Audio Windows: brak sesji użytkownika". AoIP i ASIO działają bez zmian.

## 7. Konfiguracja 1.1

```json
{
  "version": 2,
  "station_name": "STUDIO-1",
  "interface_ip": "", "ptp_domain": 0,
  "ptp_role": "auto", "ptp_priority1": 250, "ptp_priority2": 128,
  "pc_nominal_level_dbfs": -20, "http_bind": "0.0.0.0", "http_port": 8067, "password_hash": "...",
  "outputs":       [ { "enabled": true, "name": "Warszawa", "address": "239.69.100.1", "ptime_us": 1000, "keep_active": true } ],
  "subscriptions": [ { "enabled": true, "name": "Orban", "sdp": "v=0...", "latency_us": 5000 } ],
  "asio_names":    [ "Warszawa" ],
  "routes": { "net-out:1": "asio-out:1", "asio-in:1": "net-in:1", "win-out:{0.0.0.00000000}.{…}": "net-in:1" }
}
```

- `outputs`, `subscriptions` i `asio_names` mają po 32 pozycje.
- `station_name` (domyślnie nazwa komputera) trafia do nazwy sesji SDP jako `"<station_name> / <nazwa wyjścia>"`, żeby wyjścia różnych komputerów Bounce były rozróżnialne w sieci.
- **Migracja z 1.0** (brak pola `version`): `devices[i]` → `outputs[i]` (enabled, name, tx_address, tx_ptime_us, keep_active), `subscriptions[i]` (enabled = rx_sdp niepuste, sdp, latency) i `asio_names[i]`, plus routing domyślny z §3.2. Przed migracją zapisywana jest kopia `config.json.v1.bak`.

## 8. API i panel

### 8.1 Nowe i zmienione endpointy

| Metoda | Ścieżka | Opis |
|---|---|---|
| GET | `/api/routing` | źródła i cele (id, nazwa, grupa, dostępność, poziom), aktualne połączenia |
| PUT | `/api/routing` | `{ "dst": "...", "src": "..." \| null }`, zmiana jednego połączenia, działa od razu |
| GET/PUT | `/api/config` | nowy kształt z §7 |
| POST | `/api/subscriptions` | dodanie subskrypcji z SDP lub z id strumienia SAP (pierwszy wolny slot) |
| DELETE | `/api/subscriptions/{n}` | usunięcie subskrypcji i jej połączeń |

### 8.2 Zakładka "Routing"
- Siatka: wiersze to źródła pogrupowane na Sieć, ASIO i Windows; kolumny to cele pogrupowane tak samo. Grupy można zwijać.
- Filtr "tylko używane". Przy nazwach źródeł są mierniki.
- Kliknięcie kratki łączy źródło z celem i od razu wysyła `PUT /api/routing`. Drugie kliknięcie rozłącza.
- Kratka ma trzy stany: połączona, wolna, połączona ze źródłem niedostępnym (szara, z podpowiedzią "niedostępne").
- Nad siatką jest przycisk "Dodaj źródło" (lista SAP albo wklejenie SDP).
- Styl jest zgodny z obecnym, ciemnym i minimalistycznym panelem.

### 8.3 Zakładka "Konfiguracja"
- **Ustawienia globalne:** nazwa stacji, karta, domena PTP, rola i priorytety PTP, poziom nominalny, port.
- **Tabela "Wyjścia AoIP":** aktywne, nazwa, adres, ptime, utrzymuj, SDP, mierniki.
- **Tabela "Subskrypcje":** nazwa, źródło (SDP/SAP), opóźnienie odbioru, sygnał.
- **Nazwy par ASIO.**

## 9. Testy

1. **Routing (automatyczne, wieloplatformowe):** każde połączenie `net-in`/`asio-out` → `net-out`/`asio-in` w pętli, sprawdzane próbka w próbkę, z `D_src` zgodnym z §3.3. Do tego zmiana połączenia w trakcie pracy bez zatrzymania innych strumieni.
2. **PTP:**
   - symulacja dwóch komputerów `auto` (dokładnie jeden master, decyduje identyfikator);
   - pojawienie się lepszego mastera (oddanie roli ≤ 3 s, jedna korekta);
   - jego zniknięcie (przejęcie roli ≤ 6 s);
   - rola `slave` nigdy nie nadaje.
3. **Resampler:** symulacja urządzenia z dryfem ±100 ppm i jitterem okresu ±2 ms przez 1 h czasu symulowanego. Zapełnienie FIFO stabilne (±2 ms), bez przepełnień i niedoborów, poziom sinusa 1 kHz bez zmian (±0,1 dB).
4. **Migracja konfiguracji 1.0 → 1.1:** zgodność routingu domyślnego z dotychczasowym działaniem.
5. **Windows CI:** kompilacja bridge'a i testy logiki bridge'a z atrapą WASAPI.
6. **Ręcznie na Windows 11** (lista kontrolna w `docs/TESTING.md`):
   - przeglądarka → AoIP;
   - AoIP → VB-CABLE → nagrywanie w aplikacji;
   - odsłuch na słuchawkach;
   - dwa komputery Bounce ze wzajemnym PTP master/slave;
   - wejście Orbana jako mastera;
   - 24-godzinny test bez trzasków i narastania opóźnienia.

## 10. Ryzyka

| Ryzyko | Ograniczenie |
|---|---|
| Programowe znaczniki czasu mastera PTP (dziesiątki µs) | Wystarczy dla audio z buforem ≥ 2 ms. Urządzenia sprzętowe z lepszym zegarem i tak wygrywają BMCA |
| Przechwytywanie procesu zależy od API Windows 11 | Jasny stan "wymaga Windows 11" i alternatywa `win-loop` |
| VB-CABLE: jeden darmowy kabel | Wystarcza na jedną ścieżkę AoIP → aplikacje. Kolejne kable VB-Audio to symboliczna dotacja albo w przyszłości własny sterownik |
| Zmiana układu pamięci współdzielonej (v2) | Instalator wymienia usługę i sterownik ASIO naraz. Niezgodność wersji kończy się czytelnym komunikatem |

## 11. Decyzje

| Decyzja | Uzasadnienie |
|---|---|
| Bez własnego sterownika jądra | Brak budżetu na certyfikat EV. Tryb testowy Windows nie nadaje się na komputer emisyjny |
| Bridge w sesji użytkownika | Usługa w sesji 0 nie ma dostępu do audio aplikacji użytkownika |
| Pary stereo, bez sumowania | Zastosowanie radiowe, przejście bit w bit, prostsza i pewniejsza matryca |
| `D_src` per źródło na wspólnej osi czasu | Dowolne połączenie bez kopiowania i dodatkowych wątków |
| priority1 250 w trybie `auto` | Komputer nigdy nie przejmie zegara od urządzenia z domyślną konfiguracją |
