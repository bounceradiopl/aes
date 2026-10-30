# BounceAES Master: testy i odbiór

## 1. Testy automatyczne

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Zakres (108 przypadków testowych):
- SDP (w tym plik z Orbana), RTP L16/L24, zegar medialny, oś czasu i pamięć współdzielona (ringi źródeł, tabela routingu, zegar, żądanie/raport bridge'a);
- RxStream (duplikaty, przestawione pakiety, spóźnione pakiety, braki) i TxStream (opóźnienie źródła `D_src`);
- PTP: kodek wiadomości, symulacja slave'a z dryfem ±100 ppm, jitterem, utratą i powrotem mastera; wybór mastera między dwoma komputerami, przejęcie roli przez sprzętowy master i jej odzyskanie, rola „tylko slave”;
- routing (identyfikatory, kanonizacja), konfiguracja 1.1 z migracją z 1.0, SAP, hasła (wektory RFC), wymiana buforów ASIO;
- regulator PI i resampler: dryf ±100 ppm z jitterem ±2 ms przez godzinę czasu symulowanego, poziom sinusa 1 kHz ±0,1 dB, protokół bridge'a;
- warstwa systemowa: gniazda multicast, pamięć współdzielona, zdarzenia;
- silnik end-to-end, próbka w próbkę: ASIO → sieć → subskrypcja → wejście ASIO; subskrypcja przekazana na inne wyjście; lokalna pętla ASIO ze zmianą połączenia w trakcie pracy; aplikacja Windows (atrapa bridge'a) → wyjście AoIP; migracja pliku 1.0;
- API HTTP: logowanie, konfiguracja 1.1, SDP, routing, subskrypcje, panel, strumień SSE.

**Testy nigdy nie wysyłają ruchu do prawdziwej sieci**: używają wyłącznie interfejsu `127.0.0.1`. Na Linuksie trzeba włączyć multicast na pętli zwrotnej: `sudo ip link set lo multicast on`.

## 2. Test pętli na jednym komputerze

Weryfikuje cały tor: usługę, PTP, sieć (multicast w pętli zwrotnej), pamięć współdzieloną i protokół klienta ASIO, na wszystkich 64 kanałach.

```bash
# 1. testowy master PTP: tylko 127.0.0.1, najniższy priorytet; nigdy nie przejmie prawdziwej sieci
build/ptp-sim --iface 127.0.0.1 &
# 2. usługa ograniczona do pętli zwrotnej, w roli slave (żeby podążała za ptp-sim)
echo '{"version":2,"interface_ip":"127.0.0.1","ptp_role":"slave","http_bind":"127.0.0.1"}' > test.json
build/BounceAESService --console --config test.json &
# 3. hasło (pierwsze uruchomienie) i konfiguracja pętli: subskrypcja N = wyjście N, asio-in:N <- net-in:N
curl -H 'Content-Type: application/json' -d '{"password":"haslo-testowe"}' http://127.0.0.1:8067/api/setup
build/bounceaes-test --configure-loopback --devices 32 --password haslo-testowe --interface 127.0.0.1
# 4. weryfikacja próbka po próbce
build/bounceaes-test --loopback --seconds 60 --devices 32
```

Wynik z macOS (Apple Silicon), 30.10.2026:

```
OK: 64 kanałów, 22876 okresów po 128 próbek, 2880128 sprawdzonych próbek na kanał, pominięte okresy klienta: 0
PTP locked, offset −5,7 µs, opóźnienie ścieżki 143 µs, Synchronization Lost 0
32 urządzenia: TxPkts 2 833 984, RxPkts 2 829 559, TxErr 0, RxUnderrun 0, RxOverrun 0, RxSeqErr 0, jitter ≤ 0,026 ms
```

Na Windows te same polecenia działają z plikami `.exe`. Pliku konfiguracji `--config` nie trzeba podawać, gdy usługa jest zainstalowana. Wtedy `--configure-loopback --interface` zmienia konfigurację zainstalowanej usługi, więc po teście trzeba ją przywrócić.

## 3. Test zgodności w sieci docelowej

Wykonuj w oknie serwisowym, bo test wysyła strumienie do sieci AoIP.

1. **Odbiór z Orbana**: **Dodaj źródło…** (SAP albo wklejone SDP), w Routingu połącz subskrypcję z wejściem ASIO 01, ustaw w mAirList nagrywanie z "BounceAES 01 L/R". Sygnał ma być czysty, a RxUnderrun, RxSeqErr i Synchronization Lost mają wynosić 0.
2. **Nadawanie do odbiornika AES67**: wybierz w odbiorniku (np. Orbanie, jeśli ma wejście AES67, albo GStreamerze/PipeWire na Linuksie) strumień "BounceAES 01" przez SAP albo wklej SDP z przycisku SDP. Zagraj ton 1 kHz z mAirList.
3. **Wireshark** na porcie lustrzanym przełącznika:
   - *Telephony → RTP → RTP Streams*: brak zgubionych pakietów, stały odstęp pakietów (1 / 1,333 / 4 ms), znacznik czasu rośnie o 48 / 64 / 192;
   - *PTP*: Delay_Req co ~1 s z identyfikatora BounceAES, odpowiedzi Delay_Resp od grandmastera; komputer w roli slave nie wysyła Announce ani Sync;
   - *SAP*: ogłoszenia co 30 s, a przy wyłączeniu wyjścia pakiet usunięcia.

## 4. Lista kontrolna Windows 11 (wersja 1.1.1)

Ręcznie, na komputerze z Windows 11, z zalogowanym użytkownikiem (ikona w zasobniku uruchamia BounceAES Audio Bridge). Pasek panelu ma pokazywać **Windows: aktywne**.

| # | Scenariusz | Oczekiwany wynik |
|---|---|---|
| 1 | Przeglądarka → AoIP: odtwórz dźwięk w Chrome, w Routingu połącz **Windows → Chrome** z wyjściem AoIP 1, odbierz wyjście na drugim komputerze albo w Orbanie | czysty dźwięk; po zamknięciu i ponownym otwarciu Chrome dźwięk wraca w ciągu ~2 s |
| 2 | AoIP → VB-CABLE → nagrywanie: **Wirtualne karty… → Z sieci**, „CABLE: do aplikacji” ← subskrypcja → *Odbieraj*; nagrywanie z *CABLE Output* w Audacity | nagranie bez trzasków, bez narastającego opóźnienia |
| 2a | Aplikacja → VB-CABLE → AoIP: mAirList (lub Audacity) gra na *CABLE Input*; **Wirtualne karty… → Do sieci**, „CABLE: z aplikacji” → wyjście 1 → *Wysyłaj* | czysty dźwięk na odbiorniku; w siatce karta na początku grupy Windows z oznaczeniem „wirt.”; brak osobnego wiersza „CABLE Output” |
| 3 | Odsłuch: subskrypcja → **Windows → Słuchawki** | czysty dźwięk; po odłączeniu słuchawek kratka szarzeje („urządzenie niedostępne”), po podłączeniu dźwięk wraca |
| 4 | Dwa komputery Bounce, oba w roli „automatyczna” | dokładnie jeden pokazuje **PTP: master**, drugi **PTP: slave**; strumień z A odbierany na B bez RxUnderrun; po zatrzymaniu usługi na masterze drugi przejmuje rolę w ≤ 10 s |
| 5 | Orban (albo inne urządzenie AES67 z zegarem PTP) w sieci | oba komputery są slave'ami Orbana; w SDP wyjść (`a=ts-refclk`) identyfikator Orbana |
| 6 | Rola „tylko slave” na komputerze bez mastera w sieci | komputer nie zostaje masterem (stan „brak mastera”), Wireshark nie pokazuje jego Announce |
| 7 | 24 godziny: Chrome → AoIP i subskrypcja → słuchawki jednocześnie | brak trzasków; opóźnienie stałe (porównanie z nagraniem kontrolnym); w Statystykach brak błędów |

## 5. Test wytrzymałościowy (odbiór)

Na docelowym komputerze z mAirList, z zainstalowaną usługą, w sieci produkcyjnej:

1. Skonfiguruj docelowe wyjścia, subskrypcje i routing (np. 7 wyjść regionalnych i subskrypcję Orbana).
2. W zakładce Statystyki kliknij **Zeruj liczniki** i zanotuj czas.
3. Pozostaw system pod normalnym obciążeniem na **72 godziny**.

**Kryteria odbioru** (zakładka Statystyki):
- RxUnderrun = 0 i TxErr = 0 na wszystkich aktywnych wyjściach i subskrypcjach;
- RxSeqErr = 0 (pojedyncze zwiększenie przy restarcie źródła jest dopuszczalne);
- Synchronization Lost = 0, stan PTP "slave" (zsynchronizowany) albo "master", odchyłka slave'a stale poniżej ±100 µs;
- brak restartów usługi w Dzienniku zdarzeń;
- brak słyszalnych zakłóceń w nagraniu kontrolnym.
