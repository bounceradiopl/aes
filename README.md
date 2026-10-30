# BounceAES Master

Otwarty (GPLv3) sterownik audio **AES67** dla **Windows 10/11 x64** z interfejsem **ASIO**. Napisany od zera jako zamiennik Axia IP-Audio Driver dla sieci, w których pracują wyłącznie urządzenia AES67 (np. Orban Optimod).

- **32 wejścia i 32 wyjścia stereo** (64 + 64 kanały ASIO), każde jako osobny strumień AES67 (RTP L24/48 kHz, multicast).
- **PTPv2** (IEEE 1588, profil AES67, E2E): sterownik podąża za grandmasterem w sieci. Bez resamplingu i bez dryfu.
- **SAP**: własne wyjścia są ogłaszane w sieci, a strumienie innych urządzeń można wybrać z listy.
- **Panel WWW** wzorowany na oknie konfiguracji Axii, z logo Bounce. Działa w przeglądarce i jako okno aplikacji z ikony w zasobniku.
- **Bez sterownika jądra**: całość działa w trybie użytkownika, więc nie wymaga podpisu Microsoftu i nie grozi niebieskim ekranem.

## Jak to działa

```
 sieć AoIP (AES67, PTP)
        │
 BounceAESService.exe ── usługa Windows: PTP, 32× RX, 32× TX, SAP, panel WWW :8067
        │  pamięć współdzielona (oś czasu próbek) + zdarzenie okresu
 BounceAESAsio.dll ───── sterownik ASIO ładowany przez mAirList / inną aplikację
 BounceAESTray.exe ───── ikona stanu w zasobniku, otwiera panel
```

Każda próbka ma pozycję na wspólnej osi czasu, wyznaczonej przez zegar PTP (`mediaclk:direct=0`). Odbiór zapisuje pakiet RTP dokładnie pod jego znacznikiem czasu. Aplikacja ASIO czyta wejścia z opóźnieniem ustawionym osobno dla każdego wejścia, a jej wyjścia trafiają do sieci w kolejnym okresie bufora. Szczegóły w [specyfikacji](docs/superpowers/specs/2026-10-30-bounceaes-master-design.md).

## Porównanie z Axia IP-Audio Driver

| Axia IP-Audio Driver | BounceAES Master |
|---|---|
| Livewire (Standard Streams, Livestreams), zegar Livewire | AES67 (RTP L16/L24), zegar PTPv2 |
| Urządzenia WDM "Axia Wave01…" (sterownik jądra) | ASIO, kanały "BounceAES 01 L/R…" |
| Kanał Livewire wyjścia | Adres multicast wyjścia |
| Browse… (lista kanałów Livewire) | Przeglądaj… (strumienie z SAP) lub wklejenie SDP |
| KEEP CHANNEL ACTIVE | Utrzymuj strumień |
| PC Audio Nominal Level | Poziom nominalny PC (domyślnie −20 dBFS = bez zmian) |
| Livewire Statistics (TxPkts, RxPkts, RxUr, RxOr, RxSeqErr, Jitter, Synchronization Lost) | Statystyki z tymi samymi licznikami |
| GPIO | brak (funkcja konsol Livewire) |
| licencja, klucz | GPLv3, bez klucza |

## Instalacja

Instrukcja: [docs/INSTALL.md](docs/INSTALL.md). W skrócie:
1. Uruchom `BounceAES-Master-setup.exe` (instalator NSIS, z artefaktów CI albo zbudowany lokalnie).
2. Otwórz panel (ikona w zasobniku albo `http://127.0.0.1:8067`) i ustaw hasło.
3. Wybierz kartę sieciową AoIP, włącz urządzenia, przypisz źródła wejść.
4. W mAirList wybierz sterownik ASIO **BounceAES Master**.

## Budowanie

Wymagania: CMake ≥ 3.24, kompilator C++20 (MSVC 2022, GCC 12+, Clang 15+).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

- **Windows (MSVC):** `cmake -S . -B build -A x64 && cmake --build build --config Release` (Visual Studio 2022 lub nowsze). Instalator buduje NSIS, z katalogu `installer/nsis`: `makensis -DPAYLOAD=<bezwzględna ścieżka do plików .exe/.dll> BounceAES-setup.nsi` (działa też na macOS/Linuksie, `brew install makensis`; bez `-DPAYLOAD` bierze `build-win`).
- **Kompilacja skrośna z macOS/Linuksa:** `cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release`.
- **macOS/Linux:** usługę można uruchomić do testów: `build/BounceAESService --console --config moja.json`. Sterownik ASIO i aplikacja w zasobniku powstają tylko na Windows.

### Logo w panelu

Panel pokazuje logotyp Bounce ([panel/logo.svg](panel/logo.svg), biała wersja) z dopiskiem "AES Master". Aby go zmienić, podmień plik `panel/logo.svg` (albo dodaj `logo.png`) i przebuduj projekt. Plik jest wbudowywany w usługę. Bez pliku logo panel pokazuje znak tekstowy "bounce".

## Struktura

| Katalog | Zawartość |
|---|---|
| `core/` | logika niezależna od systemu: SDP, RTP, PTP, SAP, oś czasu, strumienie, konfiguracja, hasła |
| `platform/` | gniazda, pamięć współdzielona, zdarzenia, zegary: implementacje Windows i POSIX |
| `engine/` | usługa: wątki, API HTTP, panel wbudowany w plik exe |
| `service/` | punkt wejścia: usługa Windows lub `--console` |
| `asio/` | sterownik ASIO (COM) |
| `tray/` | aplikacja w zasobniku |
| `panel/` | panel WWW (HTML/CSS/JS) |
| `tools/` | `bounceaes-test` (test pętli), `ptp-sim` (testowy master PTP) |
| `installer/` | instalator NSIS (`nsis/`), skrypty PowerShell |
| `tests/` | testy jednostkowe i integracyjne (doctest) |

## Licencja

GPL-3.0-or-later. ASIO SDK firmy Steinberg jest wykorzystywane na licencji GPLv3 (podwójna licencja SDK 2.3.4). ASIO jest znakiem towarowym Steinberg Media Technologies GmbH. Projekt nie zawiera kodu ani plików binarnych Telos/Axia.

Zależności: [doctest](https://github.com/doctest/doctest) (MIT), [nlohmann/json](https://github.com/nlohmann/json) (MIT), [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT).
