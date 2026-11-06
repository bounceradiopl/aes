# Bounce Encoder 1.1 — processing Stereo Tool na stacji

- **Data:** 2026-11-06
- **Status:** zatwierdzony w rozmowie, do przeglądu pisemnego
- **Dotyczy:** Bounce Encoder (BounceAES Master bez zmian)

## 1. Cel

Opcjonalny processing dźwięku stacji przez Stereo Tool (Thimeo), wbudowany w Bounce Encoder przez bibliotekę `libStereoTool`. Każda stacja może mieć własną instancję Stereo Tool z własnym presetem i kluczem licencji. Stacje bez processingu działają jak w 1.0.

Decyzje z rozmowy (2026-11-06):

| Pytanie | Decyzja |
|---|---|
| Licencje | **klucz na stację** (domyślnie klucz firmy BOUNCE); jedna licencja = jedna stacja z processingiem naraz; panel ostrzega o tym samym kluczu na dwóch włączonych stacjach |
| Biblioteka | **dołączona do instalatora** (`libStereoTool_64.dll` 11.07, przypięta SHA-256); poza Bounce wymaga potwierdzenia od Thimeo |
| Strojenie | **preset `.sts` + interfejs WWW Stereo Tool** tej stacji (tylko 127.0.0.1); zmiany zapisywane jako nowy preset |
| Miejsce w torze | po miksie stacji, przed wszystkimi strumieniami stacji |
| Wersja | Bounce Encoder 1.1.0 |

## 2. Zakres

### W zakresie
- Na stację: przełącznik **Processing**, **klucz licencji** (pole jak hasło: nigdy nie wraca z API, puste = bez zmian), **preset** z listy wgranych presetów.
- Wgrywanie presetów `.sts` przez panel (nazwa, do 1 MB, najwyżej 64 presety), usuwanie nieużywanych, pobieranie.
- Interfejs WWW Stereo Tool każdej stacji z processingiem, nasłuchujący tylko na 127.0.0.1, na porcie z zakresu ustalanego w konfiguracji (domyślnie 8100 + numer stacji); przycisk „Otwórz Stereo Tool” w panelu (działa z komputera z enkoderem albo przez zdalny pulpit).
- „Zapisz jako preset”: zapis bieżących ustawień instancji do nowego pliku na liście.
- Stan w panelu i API: biblioteka (wersja / brak / błąd), licencja ważna dla bieżących ustawień (`CheckLicenseValid`, lista funkcji bez licencji z `GetUnlicensedUsedFeatures`), opóźnienie processingu (ms), **zużycie procesora** (czas przetwarzania / czas dźwięku, %), ostrzeżenie powyżej 80 %.
- Ostrzeżenia walidacji: ten sam klucz na dwóch włączonych stacjach z processingiem; processing włączony bez klucza albo bez presetu.
- Dokumentacja (rozdział instrukcji), LICENSING.md, testy, CI, instalator.

### Poza zakresem
- Wyjście FM/MPX (`ProcessFM`), RDS/UECP Stereo Tool, GUI natywne (Windows) — strumienie Icecast potrzebują tylko audio.
- Processing na pojedynczym strumieniu (tylko na stacji).
- Pobieranie biblioteki z internetu przez program.

## 3. Tor sygnału

```
wejścia → opóźnienie → miks stacji (48 kHz float stereo)
        → [Stereo Tool: stereoTool_Process, 48 kHz, 2 kanały]   ← tylko gdy processing włączony i działa
        → strumienie stacji (gain → resampler → kodek → Icecast)
```

- Processing działa na **osobnym wątku na stację** (worker processingu), między wątkiem stacji a workerami strumieni: wątek stacji oddaje bloki miksu do kolejki processingu (jak dziś do strumieni), worker woła `stereoTool_Process` w miejscu i rozdaje wynik strumieniom. Wątek stacji nigdy nie czeka na Stereo Tool.
- Rozmiar bloku: tak jak dziś pass stacji (10 ms = 480 ramek); Stereo Tool przyjmuje dowolne bloki.
- Opóźnienie Stereo Tool (`GetLatency2(48000, …)`) jest podawane w statusie; strumienie po prostu je dziedziczą (dla Icecasta bez znaczenia).
- **Bezpieczne obejście (bypass):** gdy biblioteki brak, instancja się nie utworzy, licencja jest nieważna dla użytych funkcji (decyzja: dalej przetwarza — Stereo Tool sam ogranicza funkcje bez licencji; panel ostrzega), worker nie nadąża (kolejka > 200 ms) albo `Process` rzuci wyjątek/SEH — stacja przechodzi płynnie (rampa 5 ms) na sygnał bez processingu i panel pokazuje przyczynę. Powrót do processingu tylko po zmianie ustawień albo ręcznym „Uruchom ponownie processing”. Nigdy cisza z powodu processingu.
- Wyłączenie/włączenie processingu i zmiana presetu: przejście z rampą 5 ms (krzyżowe przenikanie sygnału przetworzonego i nieprzetworzonego, wyrównanych o opóźnienie Stereo Tool).

## 4. Biblioteka i proces

- `libStereoTool_64.dll` (Thimeo, wersja 11.07, ok. 117 MB) ładowana w czasie pracy (`DynamicLibrary`, tylko z folderu programu, jak kodeki); funkcje przez wskaźniki do `stereoTool_*` z `libStereoTool.h` (nagłówek nie trafia do repozytorium, jeśli jego licencja na to nie pozwala — wtedy deklaracje typów piszemy sami, tylko dla używanych funkcji).
- Przed pierwszym `Create`: `stereoTool_global_setBindIp("127.0.0.1", "127.0.0.1")` (interfejs WWW i UECP tylko lokalnie), `setPriority` bez zmian.
- Instancja: `stereoTool_Create3(has_gui=false, key, st_name="station-<id>", home_path=%ProgramData%\Bounce Encoder\stereotool\<id>, load_ini=false)`, potem `LoadPreset(<plik .sts>)`. Port interfejsu WWW ustawiany przez parametr presetu/ustawień instancji (`SetStsValue`), ustalony w implementacji z `ParameterEnum.h`.
- CI: pobiera `Stereo_Tool_Generic_plugin.zip` z download.thimeo.com, sprawdza SHA-256 pliku DLL, wkłada do artefaktów i instalatora Windows x64. Linux/macOS: testy na atrapie (§8); prawdziwa biblioteka tylko w Windows CI (test „ładuje się i przetwarza ciszę” bez klucza).
- Konfiguracja budowania: `-DBAM_WITH_STEREOTOOL=ON` (domyślnie ON dla 64-bit Windows z kodekami), pamięć podręczna pobrań jak kodeki (`BAM_CODEC_CACHE`).

## 5. Konfiguracja i API

`StationConfig` dostaje:

```json
"processing": { "enabled": false, "license_key": "", "preset": "" }
```

- `license_key`: przechowywany w `config.json` (folder tylko dla administratorów); API zwraca `license_key_set: true/false`, nigdy klucza; puste pole przy zapisie = bez zmian (jak hasła, reguła E7 nie dotyczy — klucz nie jest związany z serwerem).
- `preset`: nazwa pliku z listy presetów (`%ProgramData%\Bounce Encoder\stereotool\presets\*.sts`).
- Nowe endpointy (z sesją, JSON, jak reszta API; wgranie pliku jako JSON z treścią base64, limit body 1 MB → preset ≤ ~700 KB po zakodowaniu; jeśli presety bywają większe, osobny limit dla tego endpointu do 4 MB):
  - `GET /api/stereotool` — biblioteka (wersja, stan), lista presetów, porty interfejsu WWW stacji.
  - `POST /api/stereotool/presets` (`{name, data_base64}`), `DELETE /api/stereotool/presets/<name>`, `GET /api/stereotool/presets/<name>` (pobranie).
  - `POST /api/stations/<id>/processing/save-preset` (`{name}`), `POST /api/stations/<id>/processing/restart`.
- `/api/status`: na stację `processing: {state: off|running|bypass|error, reason, latency_ms, cpu_pct, license_valid, unlicensed_features}`.
- Domyślny klucz: pole „Domyślny klucz Stereo Tool” w oknie **Ustawienia…** (globalnie), używane, gdy klucz stacji jest pusty; ostrzeżenie o tym samym kluczu dotyczy też klucza domyślnego użytego na dwóch stacjach.

## 6. Panel

- Edycja stacji: sekcja **Processing** (przełącznik, klucz z podpowiedzią „zapisany (puste = bez zmian)”, wybór presetu, przycisk „Wgraj preset…”, „Otwórz Stereo Tool”, „Zapisz jako preset…”, „Uruchom ponownie processing”), stan i zużycie procesora.
- Lista stacji: znacznik processingu (stan, % procesora), ostrzeżenie o licencji.
- Okno presetów: lista, wgranie, pobranie, usunięcie (nie można usunąć presetu używanego przez stację).
- Styl wspólny z panelami 1.2.3 (bez nowych wzorców).

## 7. Bezpieczeństwo

- DLL tylko z folderu programu (chroniony ACL instalatora); nic nie da się wgrać przez panel poza plikami `.sts` (dane, nie kod), zapisywanymi pod nazwą oczyszczoną (litery, cyfry, `-_ .`), z limitem rozmiaru i liczby.
- Interfejs WWW Stereo Tool i UECP tylko na 127.0.0.1; reguła zapory instalatora nie otwiera tych portów (dotyczy tylko programu, a połączenia z zewnątrz i tak nie dojdą do 127.0.0.1).
- Klucz licencji nigdy w logach, odpowiedziach API ani w Dzienniku zdarzeń.

## 8. Testy

- **Atrapa biblioteki** (`fake_stereotool`, budowana w testach jako biblioteka dynamiczna z tymi samymi nazwami funkcji): stały gain + opóźnienie znanej długości, licznik wywołań, tryby awarii (brak instancji, wyjątek, za wolno, licencja nieważna). Testy: tor (stacja → atrapa → FLAC → atrapa Icecast, wynik zgodny bit w bit z oczekiwanym gainem i opóźnieniem), bypass przy każdej awarii (bez ciszy, rampa), przełączanie presetu i processingu z rampą, ostrzeżenia klucza, maskowanie klucza w API, presety (wgranie, nazwy, limity, usunięcie używanego), status i CPU.
- **Windows CI z prawdziwą biblioteką:** ładowanie, wersja, `Create3` bez klucza, `Process` ciszy (wynik skończony, bez wyjątku), `Delete`.
- **Ręcznie (ENCODER-TESTING.md):** odsłuch z presetem, strojenie przez interfejs WWW, zapis presetu, ważność licencji BOUNCE, pomiar procesora na komputerze docelowym, 24 h stabilności z processingiem.

## 9. Licencje i dokumentacja

- LICENSING.md: Stereo Tool / libStereoTool © Thimeo Holding B.V., biblioteka komercyjna, nie GPL; ładowana w czasie pracy jako osobny plik; jedna licencja = jedna instancja; dołączenie do instalatora dla Bounce (licencjobiorca), przekazanie poza Bounce wymaga zgody Thimeo i własnych licencji odbiorcy. Dodatkowe uprawnienie GPLv3 §7 (jak dla FDK AAC) rozszerzone o połączenie z libStereoTool.
- ENCODER-INSTALL.md: nowy rozdział „Processing (Stereo Tool)”, wymagania sprzętowe (rdzeń na instancję, ~600 MB RAM na instancję, procesor obciążony najwyżej w połowie), diagnostyka.

## 10. Ryzyka

| Ryzyko | Ograniczenie |
|---|---|
| Biblioteka 117 MB powiększa instalator | akceptowane; instalator kompresuje (LZMA) |
| Thimeo zmienia API / licencję | wersja przypięta SHA-256; aktualizacja świadomie, z testem w CI |
| Stereo Tool blokuje wątek / ulega awarii | osobny wątek na stację, kolejka z limitem, bypass bez ciszy; wyjątki SEH łapane w workerze |
| Licencja nieważna dla funkcji presetu | stan i lista funkcji w panelu; Stereo Tool sam ogranicza funkcje |
| Interfejs WWW Stereo Tool niedostępny zdalnie | celowo (bezpieczeństwo); zdalny pulpit; opisane w instrukcji |
