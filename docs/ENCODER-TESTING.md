# Bounce Encoder: testy i odbiór

## 1. Testy automatyczne

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Kodeki (LAME 3.100, fdk-aac 2.0.3, libogg 1.3.5, libFLAC 1.4.3) CMake pobiera przy pierwszej konfiguracji z oficjalnych archiwów źródłowych, sprawdza ich sumy SHA-256 (`cmake/codecs.cmake`) i buduje razem z projektem. Buildy 64-bitowe mają kodeki domyślnie (`BAM_WITH_CODECS=ON`); 32-bitowy build (sterownik ASIO BounceAES Master dla programów 32-bitowych) jest bez kodeków i bez Bounce Encoder.

Zakres testów Bounce Encoder (część `bam_tests`, razem z testami BounceAES Master):
- **kodeki**: tabela kodeków i walidacja ustawień (polskie komunikaty), każde dozwolone ustawienie otwiera kodek i strumień niesie to ustawienie; MP3 (ramki CBR, poziom i korelacja sinusa po zdekodowaniu), AAC-LC, HE-AAC, HE-AACv2 (nagłówki ADTS, poziom i korelacja po zdekodowaniu dekoderem fdk-aac); FLAC w Ogg zdekodowany **bit w bit**, strona Ogg co najwyżej co 300 ms także przy ciszy cyfrowej; biblioteki kodeków ładowane tylko ze wskazanego folderu, nigdy ze ścieżki systemowej; błąd kodowania zgłaszany i naprawiany ponownym otwarciem;
- **DSP**: bufor opóźnienia (dokładne przesunięcie, zmiana opóźnienia z rampą 5 ms bez skoku, 60 s pojemności), miks stacji (gain, rampa, wyciszenie, saturacja), resampler 48 → 44,1 / 32 / 24 / 22,05 kHz (poziom i długość), gain strumienia;
- **klient Icecast** z atrapą serwera na 127.0.0.1: nagłówki `PUT`, Basic auth, 401/403/brak połączenia z polskimi komunikatami, przejście na `SOURCE` po 400/405, ponawianie co 1, 2, 5, 10 s, nowy początek strumienia po ponownym połączeniu, przepełnienie kolejki;
- **konfiguracja**: limity (16 stacji, 3 wejścia, 8 strumieni, gain, opóźnienie), kombinacje kodek/bitrate/częstotliwość, hasła (nigdy w API, zachowane tylko przy tym samym serwerze i porcie), czyszczenie ręcznie zepsutego pliku przy starcie;
- **silnik end-to-end**: AES67 (atrapa nadawcy na 127.0.0.1) → opóźnienie 2 s + drugie wejście −6 dB → FLAC 48 kHz → atrapa Icecasta → dekodowanie → zgodność **bit w bit** i zmierzone opóźnienie; edycja jednego strumienia restartuje tylko ten strumień; usunięcie wejścia; nowe SDP; zmiana opóźnienia odbioru zachowuje 10 s historii opóźnienia; brak biblioteki kodeka; PTP slave na pętli zwrotnej;
- **API HTTP i panel**: logowanie, pierwsze hasło tylko lokalnie, limity żądań, jedno żądanie na połączenie, odporność na przemycanie żądań, stacje (`POST`/`PUT`/`DELETE`), kodeki, strumień SSE, wbudowane pliki panelu (CSP, bez skryptów inline), `public\panel.json` dla ikony w zasobniku;
- **logika panelu** (`tests/panel/encoder_panel_logic.test.js`, node): formatowanie, listy bitrate'ów, podpowiedzi AAC↔MP3, hasło po zmianie serwera.

**Testy nigdy nie wysyłają ruchu do prawdziwej sieci**: używają wyłącznie `127.0.0.1` (Icecast i nadawca AES67 to atrapy w procesie testu). Na Linuksie trzeba włączyć multicast na pętli zwrotnej i porty PTP bez uprawnień: `sudo ip link set lo multicast on`, `sudo sysctl -w net.ipv4.ip_unprivileged_port_start=0`.

**CI** (`.github/workflows/build.yml`): Linux buduje z kodekami i uruchamia wszystkie testy; Windows buduje MSVC x64 z kodekami, uruchamia wszystkie testy, sprawdza, czy `BounceEncoder.exe` uruchomiony na 127.0.0.1 widzi wszystkie pięć kodeków obok siebie, buduje instalatory BounceAES Master i Bounce Encoder i udostępnia artefakty `BounceEncoder-setup` i `BounceEncoder-binaries`.

## 2. Przygotowanie testu ręcznego

- Komputer z Windows 10/11 x64 w sieci AoIP, z zainstalowanym `BounceEncoder-setup.exe`.
- Źródło AES67 (Orban, konsola albo BounceAES Master grający ton i muzykę) i jego SDP.
- Serwer **Icecast 2.4** (testowy, np. na drugim komputerze), w `icecast.xml` `<sources>` ≥ 10 i znane hasło źródła.
- Odtwarzacze: **VLC** oraz przeglądarka (Chrome lub Edge, Firefox).

Testy z tego rozdziału wysyłają strumienie do prawdziwego serwera Icecast: wykonuj je na serwerze testowym albo w oknie serwisowym.

## 3. Lista kontrolna Windows

| # | Scenariusz | Oczekiwany wynik |
|---|---|---|
| 1 | Instalacja: `services.msc`, zasobnik, pulpit, menu Start, `wf.msc` (reguły przychodzące), Podgląd zdarzeń | usługa **Bounce Encoder** działa (start automatyczny), ikona w zasobniku, skrót **Bounce Encoder** otwiera panel, reguła zapory „Bounce Encoder” dla `BounceEncoder.exe`, w Dzienniku Aplikacja źródło „Bounce Encoder” bez błędu „nie można odnaleźć opisu” |
| 2 | Uprawnienia: `icacls "C:\ProgramData\Bounce Encoder"` i `icacls "C:\ProgramData\Bounce Encoder\public\panel.json"`; jako zwykły użytkownik (bez podniesienia uprawnień) `type "C:\ProgramData\Bounce Encoder\config.json"` i `type "C:\ProgramData\Bounce Encoder\public\panel.json"` | folder: tylko Administratorzy i SYSTEM (pełna kontrola, dziedziczona), bez dziedziczenia z ProgramData; `config.json`: *odmowa dostępu*; `panel.json`: czytelny, dziedziczy Użytkownicy (RX) z `public`. Po restarcie usługi (plik zapisany od nowa) nadal czytelny |
| 2a | Folder założony wcześniej przez kogoś innego: przed instalacją (na czystym komputerze) jako zwykły użytkownik `mkdir "C:\ProgramData\Bounce Encoder"`, potem jako administrator `icacls "C:\ProgramData\Bounce Encoder" /grant *S-1-5-32-545:(OI)(CI)F`; zainstaluj, poczekaj na start usługi, potem jako zwykły użytkownik `type "C:\ProgramData\Bounce Encoder\config.json"` i `icacls "C:\ProgramData\Bounce Encoder"` | instalator bez ostrzeżenia o uprawnieniach; właściciel folderu i plików: Administratorzy; brak wpisu Użytkownicy na folderze (żadnego `Users:F`); `config.json`: *odmowa dostępu*; `public\panel.json` czytelny |
| 3 | Pierwsze hasło z innego komputera, potem z tego komputera | z innego komputera tylko informacja o `127.0.0.1:8070`; z tego komputera hasło ustawione, panel otwarty |
| 4 | Stacja z wejściem z Orbana (SDP wklejone, drugie z pliku) | oba wejścia „sygnał”, mierniki stacji i wejść ruszają; w Statystykach pakiety rosną, niedobory 0 |
| 5 | Pięć strumieni na prawdziwy Icecast 2.4: MP3 128 kb/s 44,1 kHz, AAC-LC 128 kb/s 48 kHz, HE-AAC 64 kb/s 44,1 kHz, HE-AACv2 32 kb/s 48 kHz, FLAC 48 kHz | wszystkie „nadaje”; w panelu administracyjnym Icecasta pięć mountpointów z typami `audio/mpeg`, `audio/aac`, `audio/aacp`, `audio/aacp`, `audio/ogg`; strumienie niewidoczne w katalogach (`Ice-Public: 0`) |
| 6 | Odsłuch w **VLC**: każdy z pięciu mountpointów | czysty dźwięk, poprawny kodek i częstotliwość w *Narzędzia → Informacje o kodeku*; HE-AACv2 stereo |
| 7 | Odsłuch w **przeglądarce** (adres mountpointu): MP3, AAC-LC, HE-AAC, HE-AACv2; FLAC sprawdź, jeśli przeglądarka obsługuje Ogg FLAC | czysty dźwięk; wynik FLAC w przeglądarce zanotuj (nie jest wymagany) |
| 8 | Błędne hasło w jednym strumieniu; drugi enkoder (albo `ffmpeg`) na zajętym mountpoincie; nieistniejąca nazwa serwera; zły port | odpowiednio „złe hasło lub użytkownik”, „mountpoint zajęty lub zabroniony”, „nie można znaleźć serwera …”, „serwer nieosiągalny”; pozostałe strumienie nadają dalej; ponowienia co 1, 2, 5, 10 s, potem co 10 s |
| 9 | Restart Icecasta w trakcie nadawania | wszystkie strumienie wracają same (w ciągu ~10 s od startu serwera); VLC po ponownym otwarciu gra, także FLAC (nowy nagłówek strumienia) |
| 10 | Zmiana bitrate'u jednego strumienia (np. MP3 128 → 192 kb/s) i zapis stacji | łączy się od nowa tylko ten strumień, bez błędu 403; w VLC na innym mountpoincie dźwięk bez przerwy; w Icecaście czas połączenia innych źródeł się nie zeruje |
| 11 | Zmiana serwera strumienia z zapisanym hasłem | podpowiedź „po zmianie serwera wpisz hasło ponownie”; bez hasła zapis zablokowany; z hasłem strumień łączy się z nowym serwerem |
| 12 | Usunięcie pierwszego z dwóch wejść w trakcie pracy, zapis | drugie wejście gra dalej bez trzasku i bez przerwy; strumienie bez ponownego łączenia |
| 13 | Opóźnienie wejścia: wpisz 10 s, zmierz (krótki ton albo klaśnięcie w źródle; nagraj jednocześnie źródło i strumień FLAC odtwarzany w VLC, odejmij bufor VLC); potem zmień tylko opóźnienie odbioru tego wejścia (np. 5 → 20 ms) | opóźnienie ≈ 10 s + bufor odtwarzacza; po zmianie opóźnienia odbioru opóźnienie wejścia zostaje 10 s (bez skoku o 10 s) |
| 14 | Zmiana opóźnienia w trakcie pracy (0 → 2 s → 0,5 s) | bez trzasku (rampa 5 ms); przy zwiększeniu cisza o długości różnicy, przy zmniejszeniu przeskok |
| 15 | Brak kodeka: zatrzymaj usługę, zmień nazwę `libfdk-aac-2.dll`, uruchom usługę; potem przywróć plik | AAC, HE-AAC i HE-AACv2 wyszarzone „(niedostępny)”, strumienie AAC „kodek niedostępny”, MP3 i FLAC nadają; po przywróceniu i restarcie wszystko nadaje |
| 16 | Czas zatrzymania usługi przy pięciu nadających strumieniach: `Measure-Command { Stop-Service BounceEncoder }` w PowerShellu jako administrator; powtórz, gdy jeden strumień łączy się z nieosiągalnym serwerem i gdy nazwa serwera nie istnieje | zatrzymanie w czasie wyraźnie poniżej 10 s (podpowiedź czasu dla Windows), bez błędu 1053/przekroczenia czasu; zanotuj zmierzone czasy |
| 17 | Ikona w zasobniku: kolory (zatrzymaj usługę, odłącz źródło), menu, drugie kliknięcie skrótu, wylogowanie i logowanie | zielona / żółta / czerwona zgodnie z INSTRUKCJA.md, rozdział 3; jedna ikona (drugi start tylko otwiera panel); po zalogowaniu ikona wraca bez otwierania okna |
| 18 | Ustawienia…: adres panelu = adres karty administracyjnej, restart usługi | panel dostępny pod tym adresem, niedostępny pod innymi; ikona w zasobniku i skrót *Panel Bounce Encoder* w menu Start otwierają nowy adres (czytają `public\panel.json`) |
| 18a | Zapomniane hasło przy adresie panelu z punktu 18: procedura z INSTRUKCJA.md, rozdział 3 | nowe hasło ustawione z `http://127.0.0.1:8070`; po przywróceniu adresu i restarcie panel działa pod adresem karty z nowym hasłem, stacje bez zmian |
| 19 | PTP: grandmaster w sieci, potem bez niego | „PTP: zsynchronizowany”, potem „zegar własny”; strumienie nadają bez przerwy |
| 20 | BounceAES Master i Bounce Encoder na jednym komputerze | oba panele działają (8067 i 8070), oba podążają za tym samym zegarem PTP, Bounce Encoder odbiera wyjście BounceAES Master (SDP z przycisku SDP) |
| 21 | Aktualizacja: ponowna instalacja tego samego lub nowszego instalatora przy nadających strumieniach | instalator zatrzymuje usługę i ikonę, wymienia pliki, uruchamia usługę; konfiguracja i hasło zostają |
| 22 | Deinstalacja: odpowiedź **Nie** na pytanie o konfigurację, ponowna instalacja; potem deinstalacja z odpowiedzią **Tak**; na koniec `uninstall.exe /S` po kolejnej instalacji | usługa, reguła zapory, źródło Dziennika, autostart, skróty i pliki usunięte; przy **Nie** i przy `/S` folder `C:\ProgramData\Bounce Encoder` zostaje, a ponowna instalacja używa starej konfiguracji; przy **Tak** folder usunięty |

## 4. Test wytrzymałościowy (24 h)

Na docelowym komputerze, z prawdziwymi źródłami AES67 i serwerem Icecast:

1. Skonfiguruj docelowe stacje (np. 2 stacje, w każdej MP3 128 kb/s, AAC-LC 128 kb/s, HE-AAC 64 kb/s, HE-AACv2 32 kb/s i FLAC), przy jednym wejściu z opóźnieniem (np. 5 s).
2. Zanotuj czas, pamięć procesu `BounceEncoder.exe` (Menedżer zadań → Szczegóły → *Zestaw roboczy*) i liczniki ze Statystyk.
3. Pozostaw system pod normalnym obciążeniem na **24 godziny**. Na początku, po 12 i po 24 godzinach posłuchaj każdego mountpointu przez minutę w VLC.

**Kryteria odbioru:**
- wszystkie strumienie „nadaje” przez cały czas; **Ponowienia** = 0 (poza celowymi restartami Icecasta), **Odrzucone** = 0, ramki odrzucone (kolejka, zegar) = 0;
- **Pominięte ramki** stacji = 0; niedobory i błędy sekwencji wejść = 0 (pojedyncze zwiększenie przy restarcie źródła jest dopuszczalne);
- PTP zsynchronizowany przez cały czas (jeśli w sieci jest grandmaster), bez utraty synchronizacji;
- pamięć procesu po 24 h nie rośnie ponad stan po pierwszej godzinie o więcej niż kilka MB;
- brak restartów usługi i błędów w Dzienniku zdarzeń;
- opóźnienie wejścia takie samo na początku i na końcu testu; brak słyszalnych zakłóceń.
