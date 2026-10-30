# BounceAES Master — specyfikacja projektu

- **Data:** 2026-10-30
- **Status:** do przeglądu
- **Licencja projektu:** GPLv3

## 1. Cel

Otwarty (open source) zamiennik sterownika **Axia IP-Audio Driver 2.12** (Telos) dla **Windows 10/11 x64**, napisany od zera. Sterownik udostępnia aplikacjom audio (przede wszystkim mAirList) wejścia i wyjścia sieci AoIP w standardzie **AES67** przez interfejs **ASIO**.

Projekt nie korzysta z kodu ani binariów Telos. Zgodność opiera się wyłącznie na otwartych standardach (AES67, RTP/RFC 3550, RFC 3190, SDP/RFC 4566, SAP/RFC 2974, IEEE 1588-2008) oraz na publicznie dostępnej instrukcji Axii, z której przejmujemy układ i nazewnictwo konfiguracji.

## 2. Zakres

### W zakresie
- Sterownik **ASIO** (64 kanały wejścia + 64 kanały wyjścia = 32 pary stereo).
- **32 wejścia stereo**: odbiór strumieni AES67 (multicast RTP).
- **32 wyjścia stereo**: nadawanie strumieni AES67 (multicast RTP).
- **PTPv2 slave** (IEEE 1588-2008, profil AES67/default, mechanizm E2E, domena konfigurowalna, domyślnie 0).
- **SAP**: ogłaszanie własnych wyjść i wykrywanie strumieni w sieci.
- **Panel WWW** wzorowany na oknie konfiguracji Axii oraz **aplikacja w zasobniku**, która pokazuje ten sam panel w osobnym oknie aplikacji (Microsoft Edge w trybie `--app`, wbudowany w Windows 10/11).
- Instalator **MSI**.

### Poza zakresem (świadomie)
- Urządzenia WDM, czyli widoczność w ustawieniach dźwięku Windows. Wymagałyby sterownika jądra podpisanego przez Microsoft.
- Natywny protokół Livewire (Standard Streams/Livestreams, zegar Livewire, kanały Livewire).
- GPIO i most GPIO Axii.
- Redundancja (SMPTE ST 2022-7) i przełączanie awaryjne.
- Częstotliwości inne niż 48 kHz.
- Tryb PTP master, sprzętowe znaczniki czasu PTP.
- Wersje na macOS i Linux.

## 3. Wymagania sygnałowe

Wzorzec zgodności to SDP z Orbana Optimod:

```
v=0
o=- 2 0 IN IP4 10.80.14.100
s=Optimod_114969_2
c=IN IP4 239.1.14.100/15
t=0 0
a=clock-domain:PTPv2 0
a=ts-refclk:ptp=IEEE1588-2008:30-D6-59-FF-FE-01-C1-19:0
a=mediaclk:direct=0
m=audio 5004 RTP/AVP 98
c=IN IP4 239.1.14.100/15
a=rtpmap:98 L24/48000/2
a=source-filter: incl IN IP4 239.1.14.100 10.80.14.100
a=clock-domain:PTPv2 0
a=sync-time:0
a=framecount:64
a=palign:0
a=ptime:1.33333337
a=ts-refclk:ptp=IEEE1588-2008:30-D6-59-FF-FE-01-C1-19:0
a=mediaclk:direct=0
a=recvonly
```

| Parametr | Odbiór (wejścia) | Nadawanie (wyjścia) |
|---|---|---|
| Kodowanie | L24, L16 | L24 |
| Częstotliwość | 48 000 Hz (inne odrzucane z komunikatem) | 48 000 Hz |
| Kanały w strumieniu | 1 lub 2 (mono trafia na L i R) | 2 |
| ptime | dowolny, wyliczany z rozmiaru pakietu (np. 0,125 / 1 / 1,333 / 4 ms) | do wyboru dla każdego wyjścia: 1 ms (domyślnie), 1,333 ms, 4 ms |
| Adresowanie | multicast, z obsługą `source-filter` (IGMPv3 SSM) | multicast, port 5004, TTL 15 |
| Zegar medialny | `mediaclk:direct=0`, znacznik RTP = próbka od epoki PTP mod 2³² | jak przy odbiorze |

Atrybuty kierunku (`recvonly`, `sendonly`) oraz `sync-time`, `framecount` i `palign` są parsowane, ale nie wpływają na odbiór.

## 4. Architektura

```
                       Sieć AoIP (VLAN, multicast, PTP grandmaster)
                                │  RTP :5004  PTP :319/320  SAP :9875
┌───────────────────────────────┴──────────────────────────────────┐
│  BounceAESService.exe  (usługa Windows, start automatyczny)      │
│   ├─ net      gniazda multicast, wybór karty sieciowej, IGMPv3   │
│   ├─ ptp      PTPv2 slave → zegar medialny (próbki 48 kHz)       │
│   ├─ rx[32]   odbiór RTP → oś czasu w pamięci współdzielonej     │
│   ├─ tx[32]   oś czasu → pakiety RTP                             │
│   ├─ sap      ogłaszanie wyjść + katalog strumieni w sieci       │
│   ├─ config   %ProgramData%\BounceAES Master\config.json         │
│   └─ http     REST API + SSE (mierniki) + pliki panelu           │
└───────────────┬────────────────────────────────┬─────────────────┘
       pamięć współdzielona + zdarzenie          HTTP :8067
┌───────────────┴──────────────┐   ┌──────────────┴───────────────┐
│ BounceAESAsio.dll (ASIO,     │   │ przeglądarka    lub          │
│ ładowany przez aplikację)    │   │ BounceAESTray.exe (okno Edge)│
└──────────────────────────────┘   └──────────────────────────────┘
```

### 4.1 Komponenty

**`core` (biblioteka statyczna, niezależna od systemu operacyjnego).**
Zawiera parser i generator SDP, pakietowanie i odczyt RTP (L16/L24), automat stanów i regulator PTP, matematykę osi czasu (zegar medialny, bufory kołowe), logikę strumieni rx/tx, logikę wymiany buforów ASIO, SAP oraz model i walidację konfiguracji. Nie korzysta z żadnego API Windows i jest w całości testowalna jednostkowo na każdej platformie.

**`platform` i `engine` (biblioteki wieloplatformowe).** `platform` to cienka warstwa systemowa: gniazda UDP, pamięć współdzielona, zdarzenia, zegar monotoniczny, timery. Ma implementacje dla Windows i POSIX. `engine` składa całą usługę (wątki PTP, rx, tx, rytm okresów, SAP, serwer HTTP). Dzięki temu całą usługę można uruchomić i przetestować end-to-end także na macOS i Linuksie (`--console`). Na Windows działa jako usługa systemowa.

**`BounceAESService.exe` (usługa Windows, konto LocalSystem, start automatyczny).**
To jedyny proces, który komunikuje się z siecią. Działa niezależnie od tego, czy jakakolwiek aplikacja audio jest uruchomiona.

| Moduł | Odpowiedzialność |
|---|---|
| `net` | Gniazda UDP (Winsock), wiązanie do wybranej karty sieciowej, dołączanie do grup multicast (ASM i SSM), reakcja na zmiany interfejsów (`NotifyIpInterfaceChange`) |
| `ptp` | Odbiór Sync/Follow_Up/Announce (224.0.1.129, porty 319/320), Delay_Req/Delay_Resp (E2E), wybór mastera (BMCA w roli slave), wyliczanie odwzorowania QPC ↔ czas PTP |
| `rx` | Po jednym obiekcie na aktywne wejście. Odbiera pakiety, zapisuje próbki na osi czasu, liczy statystyki |
| `tx` | Jeden wątek nadawczy dla wszystkich wyjść. Na każdym takcie wysyła pakiety, których czas już nadszedł |
| `sap` | Ogłaszanie SDP własnych aktywnych wyjść co 30 s (239.255.255.255:9875), wysyłka pakietu usunięcia przy wyłączeniu wyjścia, katalog strumieni wykrytych w sieci |
| `config` | Odczyt i zapis `config.json` (zapis atomowy), walidacja, rozsyłanie zmian do modułów bez restartu usługi |
| `http` | Serwer HTTP (cpp-httplib), REST API, strumień SSE (Server-Sent Events) z miernikami i statystykami, statyczne pliki panelu wbudowane w exe |
| `shm` | Utworzenie pamięci współdzielonej i zdarzeń w przestrzeni `Global\` z deskryptorem bezpieczeństwa, który pozwala procesom użytkownika na odczyt i zapis |

**`BounceAESAsio.dll` (sterownik ASIO, serwer COM in-process).**
Nie zawiera kodu sieciowego. Otwiera pamięć współdzieloną usługi i w rytmie zdarzeń okresu kopiuje bufory audio. Rejestruje się w `HKLM\SOFTWARE\ASIO\BounceAES Master`.

**`BounceAESTray.exe` (aplikacja w zasobniku).**
Ikona w kolorze stanu: zielony oznacza PTP zsynchronizowany i usługę działającą, żółty holdover PTP lub ostrzeżenia, czerwony brak usługi. Stan pochodzi z publicznego endpointu `/api/health`. Menu i dwuklik otwierają panel jako okno aplikacji (`msedge --app=http://127.0.0.1:8067`, bez pasków przeglądarki), a gdy Edge jest niedostępny, w domyślnej przeglądarce. Aplikacja startuje przy logowaniu użytkownika.

**Panel WWW.** Statyczny HTML, CSS i JavaScript bez frameworka, komunikujący się z REST API i strumieniem SSE.

**Instalator MSI (WiX).** Instaluje pliki, rejestruje usługę (z restartem po awarii: po 5 s, trzykrotnie), rejestruje sterownik ASIO, dodaje reguły zapory (UDP 5004, 319, 320, 9875, TCP 8067), dodaje autostart aplikacji w zasobniku. Hasło do panelu ustawia się przy pierwszym otwarciu panelu, wyłącznie z tego samego komputera (`127.0.0.1`).

### 4.2 Technologia

- **Język i budowanie:** C++20, CMake, MSVC (Visual Studio 2022). Testy rdzenia budują się też przez Clang/GCC na macOS i Linuksie.
- **Zależności:**

| Zależność | Licencja | Do czego |
|---|---|---|
| Steinberg ASIO SDK | GPLv3 | sterownik ASIO |
| cpp-httplib | MIT | serwer HTTP |
| nlohmann/json | MIT | konfiguracja i API |
| doctest | MIT | testy |

- **CI:** GitHub Actions. `windows-latest` buduje wszystko (MSVC) i tworzy MSI, `ubuntu-latest` uruchamia testy i test pętli.
- **Budowanie lokalne na macOS/Linux:** testy, usługa w trybie `--console` i narzędzia testowe. Części Windows (usługa, ASIO, zasobnik) można skompilować skrośnie przez MinGW-w64 w celu weryfikacji.

### 4.3 Układ repozytorium

```
BounceAES-Master/
  CMakeLists.txt
  core/        biblioteka niezależna od platformy (sdp, rtp, ptp, timeline, rx/tx, sap, config, auth)
  platform/    warstwa systemowa (Windows + POSIX)
  engine/      usługa: wątki, HTTP, panel wbudowany
  service/     punkt wejścia (usługa Windows / --console)
  asio/        sterownik ASIO
  tray/        aplikacja w zasobniku
  panel/       pliki panelu WWW
  installer/   WiX
  tools/       bounceaes-test (test pętli), ptp-sim (prosty master PTP do testów)
  tests/       testy jednostkowe i dane testowe (m.in. SDP Orbana)
  docs/
```

## 5. Przepływ audio i zegara

### 5.1 Zegar medialny

1. Moduł `ptp` utrzymuje odwzorowanie `czas_PTP = f(QPC)`, gdzie `f` to funkcja liniowa z przesunięciem i współczynnikiem częstotliwości. Odwzorowanie koryguje regulator PI na podstawie przefiltrowanych pomiarów offsetu i opóźnienia ścieżki, z odrzucaniem pomiarów odstających.
2. Zegar medialny to `N(t) = floor(czas_PTP(t) · 48000)`, czyli numer próbki od epoki PTP.
3. Znacznik RTP to `N mod 2³²`, zgodnie z `mediaclk:direct=0`.
4. Wszystkie komponenty (rx, tx, rytm ASIO) korzystają wyłącznie z `N(t)`. Nie ma resamplingu.

Używamy programowych znaczników czasu. Oczekiwana dokładność to dziesiątki µs, co wystarcza do strumieniowania programu.

### 5.2 Oś czasu w pamięci współdzielonej

- Nazwa: `Global\BounceAESMaster.v1`.
- **Nagłówek**:
  - magic i wersja układu;
  - częstotliwość próbkowania, długość bufora, liczba kanałów;
  - bieżące `N` i numer okresu;
  - rozmiar bufora ASIO;
  - licznik heartbeat usługi;
  - stan PTP;
  - opóźnienie odbioru każdego wejścia.
- **Bufory:** 128 buforów kołowych (64 wejścia, 64 wyjścia), każdy na 65 536 próbek `int32` (ok. 1,37 s). Próbka 24-bitowa jest wyrównana do lewej, co odpowiada `ASIOSTInt32LSB`. Razem ok. 32 MB.
- **Pozycja próbki** o znaczniku `N` to `N mod 65536`.
- **Znaczniki ważności:** każdy blok 16 próbek ma znacznik z numerem obiegu bufora (`N >> 16`). Odczyt bloku z nieaktualnym znacznikiem zwraca ciszę, więc dane z poprzedniego obiegu nigdy nie zostaną odtworzone.

### 5.3 Wejście (sieć → aplikacja)

1. `rx` przelicza znacznik RTP pakietu na pełne `N` (rozwinięcie 32-bitowego licznika względem bieżącego zegara medialnego). Następnie zapisuje próbki na pozycji `N` i ustawia znaczniki bloków.
2. Pakiety spoza okna (starsze niż opóźnienie odbioru albo z przyszłości o więcej niż 0,5 s) są odrzucane i liczone jako RxOverrun.
3. W okresie `k` aplikacja ASIO odczytuje próbki `[k·B − B − Lrx, k·B − Lrx)`, gdzie `B` to rozmiar bufora ASIO, a `Lrx` opóźnienie odbioru wejścia.
4. `Lrx` jest ustawiane osobno dla każdego wejścia: domyślnie 5 ms (240 próbek), zakres 1–1000 ms.
5. Reguła walidacji: `Lrx ≥ ptime_wejścia + 1 ms`. Pakiet ze znacznikiem `T` nadawca wysyła najwcześniej w chwili `T + P`, więc mniejsze opóźnienie gwarantuje braki. Jeśli wybrany strumień ma większy ptime niż pozwala na to ustawienie, panel automatycznie podnosi `Lrx` do minimum i informuje o tym.

### 5.4 Wyjście (aplikacja → sieć)

1. W okresie `k` aplikacja wypełnia bufor wyjściowy. Jego próbki trafiają na oś czasu pod `[(k+1)·B, (k+2)·B)`.
2. `tx` wysyła pakiet o znaczniku `T` (próbki `[T, T+P)`, gdzie `P` to ptime w próbkach), gdy `N(teraz) ≥ T + P`.
3. Jeśli pakiet nie zdążył (np. przy przeciążeniu systemu), jest wysyłany natychmiast. Przy opóźnieniu większym niż 2·P jest pomijany i liczony jako TxErr.
4. **Utrzymuj strumień** (odpowiednik KEEP CHANNEL ACTIVE z Axii):
   - włączone: wyjście nadaje ciszę, gdy żadna aplikacja ASIO nie jest podłączona;
   - wyłączone: nadawanie zatrzymuje się po 1 s bez aplikacji, a SAP wysyła pakiet usunięcia.
5. Wątek nadawczy korzysta z timera wysokiej rozdzielczości (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) z taktem 250 µs i wysyła paczkę pakietów dla wszystkich wyjść. Szacunkowe obciążenie: 32 × 750 = 24 000 pakietów/s przy ptime 1,333 ms.

### 5.5 Rytm ASIO

- Usługa sygnalizuje zdarzenie `Global\BounceAESMaster.v1.period` na każdej granicy okresu `N mod B == 0`.
- Sterownik ASIO ma wątek z priorytetem MMCSS "Pro Audio". Wątek czeka na zdarzenie, kopiuje bufory między osią czasu a buforami ASIO i wywołuje `bufferSwitchTimeInfo`.
- Rozmiary bufora: 64, 128, 256 i 512 próbek, domyślnie 128. Rozmiar wybiera aplikacja przy `createBuffers` i jest zapisywany w nagłówku pamięci współdzielonej.
- Częstotliwość próbkowania: tylko 48 000 Hz (`canSampleRate` odrzuca inne).
- Raportowane opóźnienia: wejście `B + Lrx` (dla najmniejszego `Lrx` aktywnych wejść), wyjście `B + P`.
- Pozycja próbki i znacznik czasu w `ASIOTime` pochodzą z `N`.
- Kilka aplikacji ASIO jednocześnie nie jest obsługiwanych. Druga aplikacja dostaje `ASE_NotPresent` z komunikatem "urządzenie zajęte".

### 5.6 Poziom nominalny

Jeden globalny parametr „Poziom nominalny PC” w dBFS, z tą samą semantyką co w Axii: wzmocnienie w stronę aplikacji wynosi `poziom + 20 dB`, a w stronę sieci `−(poziom + 20 dB)`. Zakres −30…0 dBFS. **Domyślnie −20 dBFS, czyli 0 dB i tor bit w bit.** Przy 0 dB próbki nie są w ogóle przeliczane. Przy innej wartości stosowane jest skalowanie z nasyceniem.

## 6. Panel i konfiguracja

### 6.1 Okno "Konfiguracja" (wzór: Axia IP-Audio Configuration)

**Ustawienia globalne:**
- karta sieciowa AoIP (lista);
- domena PTP;
- poziom nominalny;
- port panelu;
- zmiana hasła.

**Lista 32 urządzeń "BounceAES 01–32".** Każde urządzenie ma:
- **Aktywne**;
- **Nazwa** (np. "Kraków"). Pojawia się w nazwach kanałów ASIO jako "BounceAES 01 L (Kraków)";
- **Wyjście**: adres multicast (domyślnie `239.69.100.1` … `239.69.100.32`), ptime, przełącznik **Utrzymuj strumień** (domyślnie włączony) oraz przycisk **Pokaż SDP**, który daje SDP do skopiowania do odbiornika, np. Orbana;
- **Wejście**: źródło, czyli przycisk **Przeglądaj…** (lista strumieni z SAP) albo pole **Wklej SDP**, a do tego **opóźnienie odbioru** w ms;
- **Mierniki** wejścia i wyjścia (szczytowe, dBFS) na żywo.

### 6.2 Okno "Statystyki" (wzór: Livewire Statistics)

**Dla każdego urządzenia:**
- TxPkts, TxErr;
- RxPkts, RxBytes;
- RxUnderrun (brakujące pakiety), RxOverrun (pakiety odrzucone jako spóźnione lub spoza okna);
- RxSeqErr;
- Jitter (ms).

**Dla PTP:** stan (zsynchronizowany, holdover, brak), offset od mastera (µs), opóźnienie ścieżki, ID grandmastera, domena i licznik **Synchronization Lost**.

**Dla usługi:** czas działania, stan podłączenia aplikacji ASIO i rozmiar bufora.

Liczniki można wyzerować przyciskiem.

### 6.3 Dostęp i bezpieczeństwo

- Panel nasłuchuje na `0.0.0.0:8067`. Adres i port można zmienić.
- Logowanie: jedno konto administratora. Hasło jest przechowywane jako skrót PBKDF2 w `config.json`, a sesja trzymana w ciasteczku `HttpOnly`.
- HTTPS zapewnia odwrotne proxy, poza zakresem tego projektu.
- Połączenia z `127.0.0.1` (aplikacja w zasobniku) również wymagają logowania. Hasło można zapamiętać w Windows Credential Manager.

### 6.4 Plik konfiguracji

- Lokalizacja: `%ProgramData%\BounceAES Master\config.json`. To jedyne źródło prawdy o konfiguracji.
- Zapis jest atomowy (plik tymczasowy, potem zamiana).
- Zmiany z panelu działają bez restartu usługi. Przebudowywane są tylko zmienione strumienie.
- Zmiana karty sieciowej lub domeny PTP restartuje moduły `net` i `ptp`, ale nie całą usługę.

### 6.5 REST API (zarys)

| Metoda | Ścieżka | Opis |
|---|---|---|
| GET/PUT | `/api/config` | cała konfiguracja |
| GET/PUT | `/api/devices/{1..32}` | jedno urządzenie |
| GET | `/api/devices/{n}/sdp` | SDP wyjścia |
| GET | `/api/streams` | katalog strumieni z SAP |
| GET | `/api/status` | PTP, usługa, ASIO |
| GET | `/api/stats` | liczniki |
| POST | `/api/stats/reset` | zerowanie liczników |
| GET | `/api/logs` | ostatnie wpisy logu |
| GET (SSE) | `/api/live` | mierniki i statystyki 10×/s |
| GET | `/api/health` | publiczny: stan usługi i PTP (dla aplikacji w zasobniku) |
| GET | `/api/interfaces` | lista kart sieciowych |
| POST | `/api/login`, `/api/logout` | sesja |
| POST | `/api/setup` | ustawienie pierwszego hasła (tylko z 127.0.0.1, tylko gdy brak hasła) |

## 7. Obsługa błędów

| Sytuacja | Zachowanie |
|---|---|
| Utrata PTP | Holdover: ostatni współczynnik częstotliwości, audio gra dalej. Status żółty, rośnie licznik Synchronization Lost. Po powrocie offset < 1 ms jest korygowany płynnie (zmiana tempa), a offset ≥ 1 ms skokiem, z wpisem w logu (możliwe jedno zakłócenie) |
| Brak PTP od startu usługi | Usługa startuje na zegarze lokalnym, ze statusem czerwonym dla PTP. Audio działa, ale nie jest zsynchronizowane z siecią |
| Brak pakietów | Cisza w miejscu brakujących danych, rośnie RxUnderrun lub RxSeqErr. Po 1 s bez danych status "brak sygnału" |
| Powrót źródła | Odbiór wznawia się automatycznie |
| Usługa nie działa przy starcie aplikacji | ASIO `init` zwraca błąd "BounceAES Master: usługa nie działa" |
| Usługa pada w trakcie grania | ASIO wykrywa brak heartbeat (> 100 ms) i podaje ciszę. Po powrocie usługi wysyła `kAsioResetRequest`. Windows restartuje usługę po 5 s |
| Odłączony kabel lub zmiana IP | Ponowne dołączenie do grup multicast i ponowne ogłoszenia SAP po powrocie interfejsu |
| Błędny SDP lub nieobsługiwany format | Komunikat w panelu, wejście nieaktywne, pozostałe działają |
| Konflikt adresu multicast | Ostrzeżenie w panelu, gdy SAP pokazuje inne źródło na adresie naszego wyjścia |
| Druga aplikacja ASIO | `ASE_NotPresent`, komunikat "urządzenie zajęte" |

**Logi:** Windows Event Log (źródło "BounceAES Master") oraz plik rotowany `%ProgramData%\BounceAES Master\logs\` (5 × 10 MB), z podglądem w panelu.

## 8. Testy

1. **Testy jednostkowe rdzenia** (doctest, każda platforma, CI):
   - parser i generator SDP, z SDP Orbana jako przypadkiem testowym;
   - RTP L16/L24: pakowanie i odczyt, rozwinięcie 32-bitowego znacznika;
   - oś czasu: zawijanie bufora, znaczniki ważności, okno odbioru;
   - regulator PTP: symulacja z jitterem, dryfem ±100 ppm, utratą i powrotem mastera;
   - walidacja konfiguracji.
2. **Budowanie Windows w CI.** Każda zmiana daje gotowy MSI.
3. **Test w pętli na jednym komputerze** (`tools/bounceaes-test`). Narzędzie podłącza się do pamięci współdzielonej jak klient ASIO, nadaje na 32 wyjścia sygnał z licznikiem próbek, odbiera go na 32 wejściach (przez sieć, multicast loopback) i weryfikuje ciągłość próbka po próbce. Działa na Windows, macOS i Linuksie.
4. **Zgodność w sieci docelowej:**
   - odbiór strumienia z Orbana;
   - nadawanie do odbiornika AES67 (Orban albo GStreamer lub PipeWire na Linuksie);
   - analiza RTP w Wiresharku;
   - zgodność ogłoszeń SAP.
5. **Test wytrzymałościowy** na maszynie docelowej z mAirList.

**Kryterium odbioru:** 32 wejścia i 32 wyjścia przez **72 godziny** bez przerw. RxUnderrun = 0 i TxErr = 0 przy działającym PTP, bez restartów usługi.

## 9. Ryzyka

| Ryzyko | Ograniczenie |
|---|---|
| Jitter harmonogramu Windows przy nadawaniu (24 000 pakietów/s) | Timer wysokiej rozdzielczości, MMCSS, plan zasilania "Wysoka wydajność". W razie potrzeby nadawanie przez RIO (Registered I/O). Pomiar w teście 72 h |
| Dokładność programowego PTP | Filtrowanie i odrzucanie pomiarów odstających. Opóźnienie odbioru z zapasem (domyślnie 5 ms) |
| Moderacja przerwań karty sieciowej zwiększa jitter | Instrukcja konfiguracji karty w dokumentacji (wyłączenie moderacji przerwań i oszczędzania energii) |
| Konflikt z klientem PTP w Windows (w32time) | Instalator sprawdza i ostrzega, gdy `w32time` ma włączonego dostawcę PTP |
| Odbiorniki AES67 z małym buforem mogą nie tolerować jittera nadawania | Domyślny ptime 1 ms, do wyboru 4 ms dla odbiorników wrażliwych |

## 10. Decyzje

| Decyzja | Uzasadnienie |
|---|---|
| Przepisanie od zera, a nie modyfikacja | Binaria Axii są zamknięte i chronione prawem autorskim |
| Tylko ASIO, bez WDM | Bez podpisu Microsoftu, bez kodu w jądrze, mAirList obsługuje ASIO |
| Tylko AES67 | W sieci są wyłącznie urządzenia AES67 |
| Sieć w usłudze, a nie w DLL ASIO | Strumienie i PTP działają niezależnie od aplikacji, a awaria aplikacji nie przerywa sieci |
| Jeden panel WWW, aplikacja w zasobniku otwiera go jako okno Edge `--app` | Jeden interfejs zamiast dwóch, bez dodatkowego SDK |
| SSE zamiast WebSocket | Jednokierunkowy strumień mierników wystarcza; obsługiwany natywnie przez cpp-httplib i przeglądarki |
| Usługa wieloplatformowa w trybie `--console` | Pełny test end-to-end (sieć, PTP, pamięć współdzielona) bez Windows |
| Poziom nominalny domyślnie −20 dBFS (0 dB) | Bez ingerencji w sygnał z Orbana, semantyka jak w Axii |
| Bez GPIO | Funkcja konsol Livewire, nieobecnych w sieci |
| Nazwa: BounceAES Master | Decyzja zamawiającego |
