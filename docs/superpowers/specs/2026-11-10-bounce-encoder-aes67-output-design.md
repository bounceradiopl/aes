# Bounce Encoder 1.2 — wyjście AES67 stacji

- **Data:** 2026-11-10
- **Status:** zatwierdzony w rozmowie, do przeglądu pisemnego
- **Dotyczy:** Bounce Encoder (BounceAES Master bez zmian)

## 1. Cel

Każda stacja może oprócz strumieni Icecast nadawać swój gotowy sygnał jako strumień AES67 do sieci AoIP, np. do nadajnika FM albo kodeka linku. Wyjście jest opcjonalne. Stacje bez wyjścia AES67 działają jak w 1.1.

Stereo Tool zostaje tym, czym jest w 1.1: **opcją stacji**. Wyjście AES67 niczego w nim nie zmienia i go nie wymaga. Stacja z włączonym processingiem wysyła na AES67 sygnał przetworzony, stacja bez processingu wysyła swój miks.

Decyzje z rozmowy (2026-11-10):

| Pytanie | Decyzja |
|---|---|
| Do czego | **nadajnik / link FM**: gotowy sygnał stacji, ten sam co w strumieniach (z opóźnieniem; po Stereo Tool tylko wtedy, gdy stacja ma włączony processing) |
| Zegar | enkoder zostaje **slave'em PTP**; masterem jest Orban, który działa 24/7. Enkoder nie zostaje masterem |
| Postać | **osobna sekcja „Wyjście AES67” w stacji** (wariant A), nie kolejny typ strumienia Icecast |
| Liczba | **jedno wyjście na stację**; kilka urządzeń odbiera ten sam multicast |
| Adres domyślny | `239.69.110.N`, N = stały numer stacji (slot portu Stereo Tool + 1); BounceAES Master używa `239.69.100.x` |
| Konflikt adresu | ostrzeżenie (SAP), **bez** samodzielnej zmiany adresu |
| Utrata Orbana w trakcie pracy | nadawaj dalej na podtrzymanym zegarze, ostrzeż |
| Wersja | Bounce Encoder 1.2.0 |

Druga prośba z tego samego zgłoszenia (BounceAES: kilka źródeł ASIO na kartę dźwiękową Windows) już działa przez cele `win-out` i Miks (INSTALL.md, „Miksowanie”), więc nie wchodzi do tej specyfikacji.

## 2. Zakres

### W zakresie
- Na stację: `aes67_output {enabled, name, address, ptime_us}`. Domyślnie wyłączone; nazwa pusta = nazwa stacji; adres pusty = adres domyślny; czas pakietu 1000 µs (dozwolone 1000, 1333, 4000, jak w BounceAES).
- Format: L24, 48 kHz, 2 kanały, payload type 98, port RTP 5004, TTL jak w BounceAES (`kMulticastTtl`).
- Ogłaszanie przez SAP (co 30 s, wycofanie przy wyłączeniu wyjścia i przy zatrzymaniu usługi); SDP z `ts-refclk` grandmastera PTP i `mediaclk:direct=0`, jak w BounceAES.
- Panel: sekcja w edycji stacji, przyciski **Kopiuj SDP** / **Pobierz SDP**, stan, opóźnienie wyjścia, znaczek „AES67” z kropką stanu na liście stacji, liczniki w Statystykach.
- Walidacja: adres multicast IPv4 z zakresu `239.0.0.0/8`, unikalny wśród stacji; wejście stacji odbierające wyjście tego enkodera jest odrzucane (pętla).
- Ostrzeżenie o konflikcie: inne urządzenie ogłasza przez SAP ten sam adres.
- Dokumentacja (rozdział instrukcji), testy, CI.

### Poza zakresem
- Enkoder jako master PTP.
- Więcej niż jedno wyjście na stację, unicast, inne formaty (L16, 44,1 kHz, więcej kanałów).
- Wybór sygnału przed processingiem.
- Gain wyjścia (poziom ustawia się w Stereo Tool albo w odbiorniku).
- Zmiany w BounceAES Master.

## 3. Tor sygnału i oś czasu

```
wejścia → opóźnienie → miks stacji → [Stereo Tool, opcja stacji] ─┬─► strumienie Icecast (bez zmian)
                                                                  └─► pierścienie wyjścia AES67 stacji
                                                                        └─► wątek nadawczy (co 0,25 ms, TxStream) → multicast
```

- **Ten sam sygnał co strumienie.** Wyjście dostaje dokładnie to, co dziś trafia do `StreamWorker::push`: z processingiem — wynik workera processingu (także jego wyrównane obejście); bez processingu — miks stacji. Bez resamplingu i kodeka: float → 24 bit z przycięciem do pełnej skali, bez ditheringu. Przy 0 dB i bez processingu tor wejście → wyjście jest bit w bit.
- **Oś czasu.** Każdy blok oddawany wyjściu niesie swoją pozycję na zegarze mediów: pozycję miksu, z którego powstał (wątek stacji zna ją jako `pos_`; worker processingu przenosi ją przez swoje opóźnienie `L + 240`). Wyjście zapisuje próbkę o pozycji miksu `p` w pierścieniach (`ChannelRing`, oś czasu jak w BounceAES) pod pozycją `p + D`.
- **Stałe opóźnienie wyjścia `D`** (od pozycji miksu do znacznika czasu RTP) = opóźnienie odbioru stacji + takt stacji (10 ms) + opóźnienie processingu (`L + 240` ramek, gdy processing włączony; 0 bez processingu) + zapas 10 ms. Wartość jest stała, dopóki obsługa nie zmieni opóźnienia odbioru albo nie włączy/wyłączy processingu. Panel pokazuje opóźnienie wyjścia względem wejścia: `D` + opóźnienia wejść stacji.
- **Wysyłanie.** Jeden wątek nadawczy dla wszystkich stacji, budzony co 0,25 ms (jak `tx_loop` BounceAES) z priorytetem MMCSS „Pro Audio”, pompuje `TxStream` każdego włączonego wyjścia (`bam_core`, bez zmian) i wysyła przez jedno gniazdo UDP związane z kartą AoIP (`interface_ip`). Pakiet o znaczniku `T` wychodzi, gdy zegar mediów minie `T + P`.
- **Brakujące próbki** (wątek stacji spóźniony, stacja zatrzymana, luka w workerze): pierścień zwraca ciszę dla pozycji niezapisanych (znaczniki `ChannelRing`), więc pakiety wychodzą dalej w rytmie zegara; wyjście liczy takie pakiety („luki”).
- **Ponowne ustawienie** z wyciszeniem 5 ms przed i rampą 5 ms po: zmiana `D` (włączenie/wyłączenie processingu, zmiana opóźnienia odbioru), skok zegara PTP (`TxStream::reset`). Przy włączeniu processingu przerwa trwa około opóźnienia Stereo Tool (≈ 90 ms), jak dziś w strumieniach.

## 4. Zegar PTP

- Enkoder zostaje slave'em (domena z konfiguracji). Wyjście używa tego samego zegara mediów co wejścia.
- **Nigdy nie zsynchronizowany od startu usługi:** wyjście nie nadaje i nie ogłasza się; stan „czeka na zegar PTP”.
- **Utrata mastera po synchronizacji:** wyjście nadaje dalej na podtrzymanym zegarze (holdover na ostatniej częstotliwości), stan „nadaje · zegar podtrzymany (brak mastera PTP)” w kolorze ostrzeżenia, wpis w logu. Po powrocie mastera wyjście znów podąża za nim; skok zegara → ponowne ustawienie (§3).
- PTP wyłączony w Ustawieniach: wyjście nie nadaje, stan „PTP wyłączony — wyjście AES67 wymaga zegara PTP”.

## 5. Konfiguracja i API

- `StationConfig::aes67_output` (`Aes67OutputConfig {bool enabled = false; std::string name; std::string address; uint32_t ptime_us = 1000;}`), w JSON `"aes67_output"`. Brak klucza w pliku z 1.1 = wyłączone.
- `normalize_config`: pusty adres zostaje pusty w pliku (adres domyślny liczony z `web_port_slot`, więc nie zmienia się przy przestawianiu stacji); efektywny adres jest w stanie stacji.
- Walidacja (`validate_config`, komunikaty po polsku):
  - adres spoza `239.0.0.0/8` albo niepoprawny: błąd;
  - ten sam efektywny adres na dwóch stacjach: błąd z nazwami stacji;
  - `ptime_us` spoza {1000, 1333, 4000}: błąd;
  - wejście stacji, którego SDP ma adres multicast równy efektywnemu adresowi wyjścia którejkolwiek stacji tego enkodera: błąd „to wyjście AES67 tego enkodera (stacja …): pętla”.
- Stan stacji (`StationStatus::aes67`): `state` (`Off`, `WaitingForPtp`, `Sending`, `Holdover`, `Error`), `reason`, efektywny adres i nazwa, `output_latency_ms`, liczniki (`packets`, `gaps`, `send_errors`), `conflict` (nazwa i adres urządzenia z SAP albo puste), `sdp`.
- API: `GET /api/stations/<id>/aes67.sdp` (po zalogowaniu) zwraca SDP jako `application/sdp` z nazwą pliku `<nazwa stacji>.sdp`.

## 6. Konflikty adresów (SAP)

- Enkoder słucha ogłoszeń SAP (`239.255.255.255:9875`) na karcie AoIP, korzystając z `SapDirectory` z `bam_core`.
- Ogłoszenie innego nadawcy (inne `origin` niż ten enkoder) z tym samym adresem multicast co włączone wyjście: stan wyjścia dostaje ostrzeżenie „ten sam adres ogłasza <nazwa> (<origin>)”, a log wpis. Wyjście **nadaje dalej** i adresu nie zmienia (nadajnik jest zapisany na ten adres); zmiana należy do obsługi.
- BounceAES Master na tym samym komputerze jest innym nadawcą i też jest wykrywany.

## 7. Panel

- Edycja stacji: sekcja „Wyjście AES67” między „Processing” a „Strumienie Icecast”: przełącznik, Nazwa (podpowiedź: nazwa stacji), Adres multicast (podpowiedź: adres domyślny), Czas pakietu (lista), pola stanu (Stan, Adres, Opóźnienie wyjścia, Pakiety/Luki), przyciski Kopiuj SDP i Pobierz SDP (aktywne, gdy wyjście nadaje albo czeka). Wygląd wspólny z resztą panelu (`panel-common`).
- Lista stacji: znaczek „AES67” z kropką stanu (zielona: nadaje; żółta: czeka, podtrzymany zegar, konflikt; czerwona: błąd).
- Statystyki: liczniki wyjścia na stację.

## 8. Bezpieczeństwo

- Testy i CI nadają **wyłącznie na `127.0.0.1`** (multicast na pętli zwrotnej, jak testy wejść). Żaden test nie wysyła do prawdziwej sieci.
- Nadawanie do sieci zaczyna się tylko po włączeniu wyjścia przez zalogowanego użytkownika; domyślnie wyłączone, także po aktualizacji z 1.1.
- Pętla wyjście → wejście tego samego enkodera jest odrzucana przy zapisie (§5).
- SDP nie zawiera nic poza parametrami strumienia (żadnych kluczy ani haseł).

## 9. Testy

Automatyczne (doctest, 127.0.0.1):
- Tor bit w bit: atrapa nadawcy AES67 → stacja bez processingu → wyjście AES67 → `RxStream` → porównanie próbek; zmierzone opóźnienie równe `D` + opóźnienia wejść i stałe przez cały test.
- To samo z atrapą Stereo Tool: wyjście niesie wynik processingu; włączenie/wyłączenie processingu ustawia wyjście na nowo bez trzasku (rampy), opóźnienie po zmianie zgodne z nowym `D`.
- Skok zegara PTP: ponowne ustawienie bez lawiny pakietów i bez błędów.
- Zegar: bez synchronizacji od startu brak pakietów i SAP; po utracie mastera pakiety idą dalej, stan `Holdover`.
- Luki: wstrzymany wątek stacji → pakiety z ciszą w rytmie zegara, licznik luk rośnie, po powrocie sygnał wraca.
- SAP: ogłoszenie i wycofanie (wyłączenie wyjścia, `stop()`); konflikt z obcym ogłoszeniem → ostrzeżenie, adres bez zmian.
- Walidacja: zakres adresu, duplikaty, `ptime_us`, pętla; konfiguracja 1.1 bez klucza `aes67_output` wczytuje się bez zmian.
- API i panel: `aes67.sdp` tylko po zalogowaniu, logika panelu (`logic.js`).

Ręczne (ENCODER-TESTING.md): odbiór w BounceAES Master na tym samym komputerze, w urządzeniu Axia albo Orbanie, w nadajniku/kodeku linku; ponowne ustawienie przy włączeniu processingu słyszalne tylko jako krótka przerwa; zachowanie przy wyłączeniu Orbana; 24 h ciągłej pracy (licznik luk 0).

## 10. Dokumentacja i wydanie

- ENCODER-INSTALL.md: nowy rozdział „Wyjście AES67” (konfiguracja, SDP/SAP, opóźnienie, zegar, konflikty, rozwiązywanie problemów), aktualizacja schematu toru i wymagań.
- ENCODER-TESTING.md: nowe wiersze testów ręcznych.
- Bounce Encoder 1.2.0: PDF, CZYTAJ.txt („Nowości 1.2.0”), pełny instalator lokalnie z Stereo Tool, jak przy 1.1.0.

## 11. Ryzyka

- **Odbiornik nadajnika wymaga innego czasu pakietu albo formatu.** Mitigacja: 1 / 1,333 / 4 ms do wyboru; inne formaty poza zakresem, do dodania, jeśli konkretne urządzenie tego wymaga.
- **Opóźnienie linku w odbiorniku za małe** względem rytmu wysyłania enkodera (wątek 0,25 ms + jitter Windows). Mitigacja: instrukcja podaje zalecany link offset odbiornika (co najmniej 3 × czas pakietu); licznik spóźnionych pakietów w Statystykach.
- **Podtrzymany zegar bez Orbana dryfuje** względem innych urządzeń. Mitigacja: ostrzeżenie w panelu i logu; po powrocie mastera ponowne ustawienie.
