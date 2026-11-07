# BounceAES Master

Otwarty (GPLv3) sterownik audio **AES67** dla **Windows 10/11 x64** z interfejsem **ASIO**. Napisany od zera jako zamiennik Axia IP-Audio Driver dla sieci, w których pracują wyłącznie urządzenia AES67 (np. Orban Optimod).

- **32 wyjścia i 32 subskrypcje stereo**, każde jako osobny strumień AES67 (RTP L24/48 kHz, multicast), oraz 32 + 32 pary ASIO.
- **Routing jak w Dante Controller**: siatka źródła × cele. Dowolne źródło (subskrypcja AoIP, wyjście ASIO, aplikacja Windows, urządzenie Windows) do dowolnego celu (wyjście AoIP, wejście ASIO, urządzenie Windows), zmiana od razu, bez przerywania innych strumieni (na wielokanałowej karcie Windows kartę restartuje tylko zmiana opóźnienia najwolniejszej pary, np. źródło Windows zamiast ASIO).
- **Miksowanie**: dowolny cel może zamiast jednego źródła mieć miks kilku źródeł naraz, każde z własną głośnością (−60…+12 dB) i wyciszeniem — do 32 miksów, do 16 źródeł w miksie. Tryb miksu zawsze przechodzi przez szynę miksu: +4 ms względem najwolniejszego źródła, przy 0 dB bit w bit; chcesz zero dodatkowego opóźnienia — wyłącz miks (pojedyncze połączenie). Zmiana najwolniejszego źródła miksu (np. dodanie lub usunięcie subskrypcji w miksie z ASIO) zmienia opóźnienie miksu i może krótko przerwać dźwięk; pozostałe zmiany (głośność, wyciszenie, źródła o tym samym lub mniejszym opóźnieniu) przechodzą płynnie.
- **Karty wielokanałowe Windows**: urządzenie odtwarzające z więcej niż dwoma kanałami (interfejs USB, karta 7.1) jest widoczne jako osobny cel na każdą parę kanałów („Nazwa 1-2”, „Nazwa 3-4”…), zgodnie z formatem głośników ustawionym w Windows. Wszystkie pary jednej karty grają jednym strumieniem urządzenia, ale zmiana routingu jednej pary nie przerywa pozostałych par tej karty; zmiana opóźnienia najwolniejszej pary (np. źródło Windows zamiast ASIO) może krótko zrestartować kartę; inne urządzenia grają bez przerwy.
- **Audio Windows bez sterownika jądra**: dźwięk dowolnej aplikacji (np. przeglądarki) przechwytywany wprost z procesu (Windows 11), odsłuch i wejścia urządzeń Windows, odtwarzanie strumieni AoIP na słuchawkach. Aplikacje, które wymagają karty dźwiękowej, korzystają z darmowego VB-CABLE: panel rozpoznaje wirtualne karty (VB-CABLE, Voicemeeter, Virtual Audio Cable, Virtual Audio Driver i inne) i łączy je z AoIP jednym kliknięciem („Wirtualne karty…”).
- **PTPv2** (IEEE 1588, profil AES67, E2E): komputer podąża za grandmasterem (Orban, Omnia, konsola) albo sam zostaje masterem, gdy w sieci nie ma lepszego zegara (BMCA, role: automatyczna, preferowany master, tylko slave).
- **SAP**: wyjścia są ogłaszane jako „stacja / wyjście”, strumienie innych urządzeń i innych komputerów Bounce można dodać z listy.
- **Panel WWW** wzorowany na oknie konfiguracji Axii, z logo Bounce. Działa w przeglądarce i jako okno aplikacji z ikony w zasobniku.
- **Bez sterownika jądra**: całość działa w trybie użytkownika, więc nie wymaga podpisu Microsoftu i nie grozi niebieskim ekranem.

## Jak to działa

```
 sieć AoIP (AES67, PTP)
        │
 BounceAESService.exe ──── usługa Windows: PTP slave/master, 32× RX, 32× TX, routing, SAP, panel WWW :8067
        │  pamięć współdzielona: oś czasu próbek, tabela routingu, zegar, zdarzenie okresu
 BounceAESAsio.dll ─────── sterownik ASIO ładowany przez mAirList / inną aplikację
 BounceAESAudioBridge.exe ─ audio Windows w sesji użytkownika (WASAPI, przechwytywanie procesów)
 BounceAESTray.exe ─────── ikona stanu w zasobniku, otwiera panel, nadzoruje bridge
```

Każda próbka ma pozycję na wspólnej osi czasu, wyznaczonej przez zegar PTP (`mediaclk:direct=0`). Każde źródło zapisuje swój ring na tej osi i deklaruje opóźnienie gotowości `D_src` (subskrypcja: opóźnienie odbioru, ASIO: 0, Windows: 20 ms), a cel czyta z pozycji „teraz − `D_src`”. Dzięki temu każde połączenie działa bez kopiowania i bez dodatkowych wątków. Cel w trybie miksu czyta zamiast tego gotowy miks z szyny miksera (usługa, osobny wątek o rytmie 1 ms), z opóźnieniem najwolniejszego członka + 4 ms. Audio Windows ma własny zegar urządzenia, więc bridge dopasowuje je do PTP resamplerem z regulatorem PI. Szczegóły: [specyfikacja 1.0](docs/superpowers/specs/2026-10-30-bounceaes-master-design.md), [specyfikacja 1.1](docs/superpowers/specs/2026-10-30-bounceaes-routing-ptp-windows-design.md), [specyfikacja 1.2](docs/superpowers/specs/2026-11-03-bounceaes-mixing-design.md).

## Porównanie z Axia IP-Audio Driver

| Axia IP-Audio Driver | BounceAES Master |
|---|---|
| Livewire (Standard Streams, Livestreams), zegar Livewire | AES67 (RTP L16/L24), zegar PTPv2 |
| Urządzenia WDM "Axia Wave01…" (sterownik jądra) | ASIO, kanały "BounceAES 01 L/R…" |
| Kanał Livewire wyjścia | Adres multicast wyjścia |
| Browse… (lista kanałów Livewire) | Dodaj źródło… (strumienie z SAP) lub wklejenie SDP |
| urządzenia WDM dla dowolnej aplikacji | przechwytywanie aplikacji i urządzeń Windows w siatce routingu, VB-CABLE dla aplikacji wymagających karty |
| KEEP CHANNEL ACTIVE | Utrzymuj strumień |
| PC Audio Nominal Level | Poziom nominalny PC (domyślnie −20 dBFS = bez zmian) |
| Livewire Statistics (TxPkts, RxPkts, RxUr, RxOr, RxSeqErr, Jitter, Synchronization Lost) | Statystyki z tymi samymi licznikami |
| GPIO | brak (funkcja konsol Livewire) |
| licencja, klucz | GPLv3, bez klucza |

## Instalacja

Instrukcja: [docs/INSTALL.md](docs/INSTALL.md). W skrócie:
1. Uruchom `BounceAES-Master-setup.exe` (instalator NSIS, z artefaktów CI albo zbudowany lokalnie).
2. Otwórz panel (ikona w zasobniku albo `http://127.0.0.1:8067`) i ustaw hasło.
3. W zakładce **Konfiguracja** wybierz kartę sieciową AoIP, nazwę stacji, rolę PTP i włącz wyjścia.
4. **Dodaj źródło…** subskrybuje strumienie z sieci (Orban, Omnia, inny komputer Bounce).
5. W zakładce **Routing** połącz źródła z celami.
6. W mAirList wybierz sterownik ASIO **BounceAES Master**.

## Co nowego w 1.2.5

Poprawki bezpieczeństwa panelu: złośliwie zbudowane zapytania (bardzo głęboko zagnieżdżony albo ogromny JSON) są odrzucane przed logowaniem, zanim usługa zacznie je czytać, a bardzo długie hasło nie obciąża już procesora przy logowaniu. Zapisane hasła działają dalej, bez zmian w obsłudze. Te same poprawki ma Bounce Encoder 1.0.4.

## Co nowego w 1.2.4

Nowa ikona, żeby nie mylić programów: BounceAES Master ma litery **BA**, Bounce Encoder (1.0.3) litery **BE**, obie na czarnym kwadracie z różową kropką. Ikona w zasobniku pokazuje stan kolorową kropką w prawym dolnym rogu (zielona, żółta, czerwona), jak dotąd. Bez zmian w działaniu.

## Co nowego w 1.2.3

Nowy wygląd panelu, bez zmian w działaniu: zakładki w górnym pasku, stan usługi (PTP, ASIO, Audio Windows, wyjścia, subskrypcje, karta sieciowa, czas działania) w pasku stanu na dole, mierniki LED ze skalą dBFS i zapamiętanym szczytem, przełączniki zamiast pól wyboru. Wygląd jest wspólny z Bounce Encoder 1.0.2 (`panel-common/common.css`).

## Co nowego w 1.2.2

Bezpieczeństwo panelu, z zabezpieczeniami przetestowanymi w Bounce Encoder 1.0 (dźwięk, ASIO i routing bez zmian): jedno żądanie na połączenie, treść żądań tylko jako JSON (do 1 MB), pierwsze hasło tylko z `127.0.0.1`/`localhost`, blokada po 5 nieudanych logowaniach zamiast spowalniania, zmiana hasła wylogowuje inne sesje, a podgląd na żywo kończy się z sesją. Własne skrypty korzystające z API muszą wysyłać JSON z `Content-Type: application/json` (panel i ikona w zasobniku już to robią). Szczegóły: [docs/INSTALL.md](docs/INSTALL.md) (rozdziały 4 i 10).

## Bounce Encoder

Osobny program z tego samego repozytorium (wspólny rdzeń AES67/PTP): **enkoder AES67 → Icecast** dla Windows 10/11 x64. BounceAES Master działa bez zmian obok niego.

- Do **16 stacji**, każda z do **3 wejściami AES67** (SDP wklejony albo z pliku; gain −60…+12 dB; opóźnienie 0–60 s co 0,1 s), sumowanymi w sygnał stacji.
- Do **8 strumieni Icecast** na stację: **MP3** (LAME), **AAC-LC**, **HE-AAC**, **HE-AACv2** (Fraunhofer FDK AAC), **FLAC** (Ogg); 48 / 44,1 / 32 / 24 / 22,05 kHz; każdy z własnym serwerem, mountpointem, gainem i ponawianiem połączenia.
- Panel WWW z logowaniem (port **8070**): stacje z miernikami, edycja, odpowiedniki bitrate'ów AAC ↔ MP3, statystyki, logi. Usługa Windows `BounceEncoder`, ikona w zasobniku, instalator `BounceEncoder-setup.exe`.
- Bez processingu, RDS i metadanych; PTP tylko jako slave.
- Wersja 1.0.1: blokada logowania działa też przy równoległych próbach (wspólna poprawka z BounceAES Master 1.2.2).
- Wersja 1.0.2: nowy wygląd panelu, wspólny z BounceAES Master 1.2.3 (lampka „NA ANTENIE” przy nadającym strumieniu, mierniki LED, pasek stanu na dole); bez zmian w działaniu.
- Wersja 1.0.3: własna ikona z literami **BE** (BounceAES Master ma **BA**), w zasobniku, na skrótach, w instalatorze i w karcie przeglądarki; bez zmian w działaniu.
- Wersja 1.0.4: poprawki bezpieczeństwa panelu (wspólne z BounceAES Master 1.2.5): złośliwie zbudowane zapytania są odrzucane przed logowaniem, bardzo długie hasła nie obciążają procesora.

Instrukcja: [docs/ENCODER-INSTALL.md](docs/ENCODER-INSTALL.md), testy i odbiór: [docs/ENCODER-TESTING.md](docs/ENCODER-TESTING.md), specyfikacja: [docs/superpowers/specs/2026-11-05-bounce-encoder-design.md](docs/superpowers/specs/2026-11-05-bounce-encoder-design.md). Licencja Bounce Encoder (GPLv3) ma dodatkowe uprawnienie na połączenie z fdk-aac; w Bounce instalator z AAC jest bez ograniczeń, a przed przekazaniem poza firmę trzeba sprawdzić patenty AAC ([LICENSING.md](LICENSING.md)).

## Budowanie

Wymagania: CMake ≥ 3.24, kompilator C++20 (MSVC 2022, GCC 12+, Clang 15+).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

- **Windows (MSVC):** `cmake -S . -B build -A x64 && cmake --build build --config Release` (Visual Studio 2022 lub nowsze). Instalator buduje NSIS, z katalogu `installer/nsis`: `makensis -DPAYLOAD=<bezwzględna ścieżka do plików .exe/.dll> BounceAES-setup.nsi` (działa też na macOS/Linuksie, `brew install makensis`; bez `-DPAYLOAD` bierze `build-win`).
- **Bounce Encoder:** buildy 64-bitowe pobierają i budują kodeki (LAME, fdk-aac, libogg, libFLAC; `BAM_WITH_CODECS`, `cmake/codecs.cmake`) i tworzą `BounceEncoder` oraz `BounceEncoderTray` (Windows). **Konfiguracja 64-bitowego buildu z kodekami potrzebuje dostępu do sieci** (archiwa źródeł z sourceforge.net, github.com i xiph.org, sprawdzane po SHA-256), chyba że podasz `-DBAM_WITH_CODECS=OFF` (wtedy bez Bounce Encodera). `-DBAM_CODEC_CACHE=<folder>` trzyma pobrane archiwa w tym folderze i przy kolejnych konfiguracjach bierze je stamtąd bez sieci (tak robi CI). Instalator: `makensis -DPAYLOAD=<folder z BounceEncoder.exe> BounceEncoder-setup.nsi` w `installer/nsis`.
- **Kompilacja skrośna z macOS/Linuksa:** `cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release`.
- **macOS/Linux:** usługę można uruchomić do testów: `build/BounceAESService --console --config moja.json`. Sterownik ASIO i aplikacja w zasobniku powstają tylko na Windows.

### Logo w panelu

Panel pokazuje logotyp Bounce ([panel/logo.svg](panel/logo.svg), biała wersja) z dopiskiem "AES Master". Aby go zmienić, podmień plik `panel/logo.svg` (albo dodaj `logo.png`) i przebuduj projekt. Plik jest wbudowywany w usługę. Bez pliku logo panel pokazuje znak tekstowy "bounce".

## Struktura

| Katalog | Zawartość |
|---|---|
| `core/` | logika niezależna od systemu: SDP, RTP, PTP (slave/master), SAP, oś czasu, routing, strumienie, konfiguracja, regulator i resampler, hasła |
| `platform/` | gniazda, pamięć współdzielona, zdarzenia, zegary: implementacje Windows i POSIX |
| `engine/` | usługa: wątki, API HTTP, panel wbudowany w plik exe |
| `service/` | punkt wejścia: usługa Windows lub `--console` |
| `asio/` | sterownik ASIO (COM) |
| `tray/` | aplikacja w zasobniku |
| `bridge/` | BounceAES Audio Bridge: WASAPI, przechwytywanie procesów, resampling |
| `panel/` | panel WWW (HTML/CSS/JS) |
| `panel-common/` | arkusz stylów wspólny dla paneli BounceAES Master i Bounce Encoder |
| `encoder/` | Bounce Encoder: kodeki, bufor opóźnienia, miks stacji, klient Icecast, silnik, API i panel WWW (`encoder/panel/`) |
| `encoder_service/`, `encoder_tray/` | Bounce Encoder: usługa Windows / `--console` i ikona w zasobniku |
| `tools/` | `bounceaes-test` (test pętli), `ptp-sim` (testowy master PTP), `make_icons.py` (generuje ikony BA i BE: `python3 tools/make_icons.py`, wymaga Pillow) |
| `installer/` | instalator NSIS (`nsis/`), skrypty PowerShell |
| `tests/` | testy jednostkowe i integracyjne (doctest) |

## Licencja

GPL-3.0-or-later. ASIO SDK firmy Steinberg jest wykorzystywane na licencji GPLv3 (podwójna licencja SDK 2.3.4). ASIO jest znakiem towarowym Steinberg Media Technologies GmbH. Projekt nie zawiera kodu ani plików binarnych Telos/Axia.

Bounce Encoder dodaje kodeki pobierane przy budowaniu: LAME 3.100 (LGPL), Fraunhofer FDK AAC 2.0.3 (licencja Fraunhofera, niezgodna z GPL), libogg 1.3.5 i libFLAC 1.4.3 (BSD-3-Clause); szczegóły w [LICENSING.md](LICENSING.md).

Zależności: [doctest](https://github.com/doctest/doctest) (MIT), [nlohmann/json](https://github.com/nlohmann/json) (MIT), [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT), [libsamplerate](https://github.com/libsndfile/libsamplerate) 0.2.2 (BSD-2-Clause). VB-CABLE (VB-Audio, donationware) nie jest częścią projektu; instaluje się go osobno, jeśli jest potrzebny.
