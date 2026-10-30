# BounceAES Master: testy i odbiór

## 1. Testy automatyczne

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Zakres (82 przypadki testowe):
- SDP (w tym plik z Orbana), RTP L16/L24, zegar medialny, oś czasu i pamięć współdzielona;
- RxStream (duplikaty, przestawione pakiety, spóźnione pakiety, braki) i TxStream;
- PTP: kodek wiadomości i symulacja slave'a z dryfem ±100 ppm, jitterem, utratą i powrotem mastera oraz zmianą mastera;
- SAP, konfiguracja, hasła (wektory RFC), wymiana buforów ASIO;
- warstwa systemowa: gniazda multicast, pamięć współdzielona, zdarzenia;
- silnik end-to-end: wyjście wraca przez sieć na wejście próbka w próbkę;
- API HTTP: logowanie, konfiguracja, SDP, panel, strumień SSE.

**Testy nigdy nie wysyłają ruchu do prawdziwej sieci**: używają wyłącznie interfejsu `127.0.0.1`. Na Linuksie trzeba włączyć multicast na pętli zwrotnej: `sudo ip link set lo multicast on`.

## 2. Test pętli na jednym komputerze

Weryfikuje cały tor: usługę, PTP, sieć (multicast w pętli zwrotnej), pamięć współdzieloną i protokół klienta ASIO, na wszystkich 64 kanałach.

```bash
# 1. testowy master PTP: tylko 127.0.0.1, najniższy priorytet; nigdy nie przejmie prawdziwej sieci
build/ptp-sim --iface 127.0.0.1 &
# 2. usługa ograniczona do pętli zwrotnej
echo '{"interface_ip":"127.0.0.1"}' > test.json
build/BounceAESService --console --config test.json &
# 3. hasło (pierwsze uruchomienie) i konfiguracja pętli: wejście N = wyjście N
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

1. **Odbiór z Orbana**: wklej SDP Orbana do urządzenia 01, ustaw w mAirList nagrywanie z "BounceAES 01 L/R". Sygnał ma być czysty, a RxUnderrun, RxSeqErr i Synchronization Lost mają wynosić 0.
2. **Nadawanie do odbiornika AES67**: wybierz w odbiorniku (np. Orbanie, jeśli ma wejście AES67, albo GStreamerze/PipeWire na Linuksie) strumień "BounceAES 01" przez SAP albo wklej SDP z przycisku SDP. Zagraj ton 1 kHz z mAirList.
3. **Wireshark** na porcie lustrzanym przełącznika:
   - *Telephony → RTP → RTP Streams*: brak zgubionych pakietów, stały odstęp pakietów (1 / 1,333 / 4 ms), znacznik czasu rośnie o 48 / 64 / 192;
   - *PTP*: Delay_Req co ~1 s z identyfikatora BounceAES, odpowiedzi Delay_Resp od grandmastera;
   - *SAP*: ogłoszenia co 30 s, a przy wyłączeniu wyjścia pakiet usunięcia.

## 4. Test wytrzymałościowy (odbiór)

Na docelowym komputerze z mAirList, z zainstalowaną usługą, w sieci produkcyjnej:

1. Skonfiguruj docelowe urządzenia (np. 7 wyjść regionalnych i wejście z Orbana).
2. W zakładce Statystyki kliknij **Zeruj liczniki** i zanotuj czas.
3. Pozostaw system pod normalnym obciążeniem na **72 godziny**.

**Kryteria odbioru** (zakładka Statystyki):
- RxUnderrun = 0 i TxErr = 0 na wszystkich aktywnych urządzeniach;
- RxSeqErr = 0 (pojedyncze zwiększenie przy restarcie źródła jest dopuszczalne);
- Synchronization Lost = 0, stan PTP "zsynchronizowany", odchyłka stale poniżej ±100 µs;
- brak restartów usługi w Dzienniku zdarzeń;
- brak słyszalnych zakłóceń w nagraniu kontrolnym.
