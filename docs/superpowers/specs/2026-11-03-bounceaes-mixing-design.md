# BounceAES Master 1.2 — miksowanie źródeł i karty wielokanałowe

- **Data:** 2026-11-03
- **Status:** zatwierdzony w rozmowie, do przeglądu pisemnego
- **Bazuje na:** `2026-10-30-bounceaes-routing-ptp-windows-design.md` (1.1) i wydaniach 1.1.1–1.1.5

## 1. Cel

1. **Miksowanie:** kilka źródeł na jeden cel, każde z własną głośnością — na wyjściach AoIP, wejściach ASIO i urządzeniach odtwarzających Windows. Przykład z praktyki: kilka par ASIO z mAirList na jedne słuchawki albo na jedno wyjście do Orbana.
2. **Karty wielokanałowe Windows:** osobne pary kanałów jednej karty (np. interfejs USB z 4 parami, karta 7.1) jako osobne cele: ASIO 1 → wyjścia 1-2, ASIO 2 → wyjścia 3-4.

Decyzje z rozmowy (2026-10-31, 2026-11-03):

| Pytanie | Decyzja |
|---|---|
| Przypadek | jedno i drugie: miks na parę oraz osobne pary kart wielokanałowych |
| Gdzie miks | wszędzie (AoIP, ASIO, Windows) |
| Podejście | szyny miksu w usłudze (mikser centralny), cele czytają gotowy miks |
| Obsługa w siatce | tryb miksu włączany na celu; domyślnie klik zastępuje źródło jak dotąd |

## 2. Zakres

### W zakresie
- Do **32 miksów** jednocześnie, do **16 źródeł** w miksie, głośność **−60…+12 dB** (krok 0,5 dB) i wyciszenie dla każdego źródła.
- Mikser w usłudze, niezależny od aplikacji ASIO (działa też bez mAirList).
- Miks z dokładnie jednym, niewyciszonym źródłem na 0 dB nie przechodzi przez mikser: zostaje połączeniem bezpośrednim, bit w bit, bez dodatkowego opóźnienia.
- Pary kanałów urządzeń odtwarzających Windows: tyle celów, ile par kanałów ma format urządzenia w trybie współdzielonym (WASAPI mix format).
- Okno celu w siatce (dla każdego celu), API, dokumentacja, testy.

### Poza zakresem
- Miksowanie pojedynczych kanałów mono, panoramowanie, korekcja, kompresja, automatyka głośności.
- Nagrywanie (capture) z kart wielokanałowych — zostaje stereo (para 1-2).
- Tryb wyłączny WASAPI.

## 3. Model

### 3.1 Konfiguracja

Obok `routes` (cel → jedno źródło) dochodzi `mixes` (cel → lista źródeł). Cel występuje w co najwyżej jednym z nich.

```json
"routes": { "net-out:2": "asio-out:2" },
"mixes": {
  "win-out:{0.0.0.00000000}.{guid}": { "sources": [
    { "source": "asio-out:1", "gain_db": 0,   "mute": false },
    { "source": "asio-out:2", "gain_db": -6,  "mute": false },
    { "source": "net-in:1",   "gain_db": -12, "mute": true }
  ] }
}
```

Walidacja: cel i źródła poprawne (`parse_dest` / `parse_source`), cel nie w `routes`, 0–16 źródeł bez powtórzeń, `gain_db` w −60…+12, co najwyżej 32 miksy. Brak pola `mixes` = pusty (konfiguracje 1.1 bez zmian). Pusty miks (0 źródeł) jest dozwolony — cel milczy, tryb miksu zostaje.

### 3.2 Identyfikatory par kanałów

- `win-out:<endpoint>` — para 1 (kanały 1-2), jak dotąd; istniejące połączenia działają.
- `win-out:<endpoint>#N` — para N (N ≥ 2), kanały 2N−1 i 2N. Nieparzysta liczba kanałów: ostatnia para ma tylko lewy kanał.
- `parse_dest` / `to_id` obsługują przyrostek `#N`; `RouteDest` dostaje pole `pair` (1-based, domyślnie 1).

## 4. Usługa

### 4.1 Szyny miksu

- `RuntimeSet` dostaje przydział szyn: cel w trybie miksu → szyna 0..31, stabilny między przebudowami (jak `win_slots`): istniejący miks zachowuje swoją szynę, nowy bierze pierwszą wolną.
- Miks „bezpośredni” (jedno niewyciszone źródło, 0 dB) nie dostaje szyny; `resolve` zwraca źródło jak w trybie pojedynczym.
- Opóźnienie gotowości szyny: `D_mix = max(D_src członków) + 96` próbek (2 ms zapasu na rytm miksera). Przykłady: ASIO + ASIO → 2 ms; ASIO + subskrypcja 5 ms → 7 ms; z Windows (20 ms) → 22 ms.
- `ResolvedSource` dla celu w trybie miksu: `kind = Mix`, `pair = szyna`, `delay = D_mix`.

### 4.2 Wątek miksera

- Uruchamiany razem z silnikiem; rytm 1 ms (zegar o wysokiej rozdzielczości, jak wątek nadawania), priorytet „Pro Audio”.
- Dla każdej szyny pamięta `mixed_upto`. W każdym przebiegu miksuje pozycje `[mixed_upto, now − max(D_src))` blokami po 48 próbek: odczyt pierścieni członków, mnożenie przez wzmocnienie liniowe, suma w `float`, zapis do pierścieni szyny z nasyceniem (`from_float`).
- Zmiana generacji zegara (skok PTP) albo cofnięcie się/oddalenie o > 100 ms: `mixed_upto = now − max(D_src)` (bez nadrabiania wstecz).
- Przebudowa konfiguracji podmienia migawkę członków atomowo (jak `RuntimeSet`); zmiana głośności nie przerywa szyny.
- Licznik `mixer_late` (przebiegi, w których mikser nie zdążył przed `D_mix`) w statusie i zakładce Statystyki.
- Mierniki: szczyt każdej szyny liczony przy zapisie; miernik celu w trybie miksu pokazuje szczyt szyny.

### 4.3 Konsumenci

- **Wyjście AoIP:** `TxStream` czyta pierścienie szyny z `delay = D_mix`. `follows_asio` = wszyscy członkowie to ASIO (lub brak członków).
- **Wejście ASIO:** `set_asio_route(pair, Mix, szyna, D_mix)`; sterownik czyta przez `source_ring(Mix, …)` — kod wspólny (`core`), bez zmian logiki w sterowniku. `getLatencies` uwzględnia `D_mix`.
- **Urządzenie Windows:** żądanie do Audio Bridge niesie `kind = Mix`, `pair = szyna`, `delay_frames = D_mix`.

### 4.4 Pamięć współdzielona (wersja 3)

- 32 pary pierścieni szyn (64 kanały, +16 MB) za pierścieniami Windows; `ShmSource::Mix = 4`; `source_ring` obsługuje `Mix`.
- `kShmVersion = 3`, nazwa `BounceAESMaster.v3`, magia `BAM3`, pole `channels_mix` w nagłówku. Sterownik/Bridge ze starszą usługą: `compatible()` = fałsz i komunikat „niezgodna wersja usługi i sterownika ASIO” (jak dotąd).
- Przypięte układy (`static_assert` w `shm_layout.cpp`) zaktualizowane; CI dalej sprawdza 32-bitowy sterownik z 64-bitową usługą.
- Widok pamięci w 32-bitowym procesie mAirList rośnie do ok. 57 MB — mieści się w przestrzeni adresowej; ryzyko opisane w §8.

## 5. Audio Bridge: karty wielokanałowe

- `BridgeEndpoint` dostaje `channels` (z formatu mieszania urządzenia odtwarzającego).
- `BridgeDestReq` dostaje `channel_pair` (1-based). Bridge grupuje cele tego samego urządzenia w **jeden** `RenderStream` otwierający wszystkie kanały.
- `RenderPacer` i `DriftResampler` uogólnione na N kanałów (libsamplerate obsługuje dowolną liczbę); `Reader` wypełnia kanały pary, pary bez źródła grają ciszę.
- Zachowane z 1.1.4: zapas dwóch okresów urządzenia (min. 20 ms), bufor 40 ms, wypełnienie zapasu przy zakotwiczeniu.

## 6. Panel i API

### 6.1 Siatka
- Kliknięcie nazwy **każdej** kolumny otwiera okno celu (dla wyjść AoIP dotychczasowe szczegóły — adres, SDP, Usuń — są w tym samym oknie).
- Okno celu: przełącznik **Miks**; w trybie miksu lista źródeł z suwakiem dB, przyciskiem wycisz, miernikiem, „×” do usunięcia, oraz opóźnienie miksu w ms.
- W trybie miksu kliknięcie kratki dodaje/usuwa źródło (kolumna może mieć wiele kropek); nagłówek ma oznaczenie „miks”.
- Włączenie miksu: dotychczasowe źródło staje się pierwszym członkiem (0 dB). Wyłączenie przy ≥ 2 źródłach: okno pyta, które zostawić jako pojedyncze.
- Karty wielokanałowe: kolumny „Nazwa 1-2”, „Nazwa 3-4”…

### 6.2 API
- `GET /api/routing`: dodatkowo `mixes` (cel → lista `{source, gain_db, mute}`) i dla każdego celu `mix_delay_ms`, gdy w trybie miksu.
- `PUT /api/routing {dst, src}`: cel w trybie pojedynczym — zastępuje (jak dotąd); w trybie miksu — przełącza członkostwo `src` (dodaje z 0 dB / usuwa).
- `PUT /api/mixes/<dst> {enabled, sources?, keep?}`: włącza/wyłącza tryb miksu, ustawia całą listę (głośności, wyciszenia); przy wyłączaniu `keep` wskazuje źródło pojedyncze.
- Wszystkie zmiany działają od razu (jak routing), konfiguracja zapisywana, szkic w Konfiguracji synchronizowany.

## 7. Testy

| Obszar | Test |
|---|---|
| Konfiguracja | `mixes` round-trip JSON; walidacja (cel w `routes` i `mixes`, > 16 źródeł, dB poza zakresem, > 32 miksy, duplikaty) |
| Identyfikatory | `win-out:…#N` parse/to_id; para 1 bez przyrostka |
| Mikser (jednostkowo) | suma dwóch rampy z 0 dB dokładnie; −6,02 dB ≈ ½ (±1 LSB 24 bit); wyciszenie; nasycenie zamiast zawinięcia; wyrównanie członków o różnym `D_src` |
| Silnik | miks ASIO 1 + ASIO 2 → wyjście AoIP → subskrypcja pętlą → zgodność z sumą; miks → wejście ASIO (atrapa hosta); miks → urządzenie Windows (atrapa Bridge'a: żądanie `Mix`, `D_mix`); miks jednoźródłowy 0 dB = połączenie bezpośrednie bit w bit; zmiana głośności bez przerwy w szynie; stabilny przydział szyn |
| Bridge | grupowanie celów jednego urządzenia, `channel_pair` w żądaniu; `RenderPacer` 4- i 8-kanałowy na modelu silnika Windows: 0 przerw |
| Pamięć | nowe `static_assert`; CI: 32-bitowy klient z 64-bitową usługą przez szynę miksu |
| Panel | przeglądarka, tylko 127.0.0.1: tryb miksu, klik dodaje/usuwa, suwak dB, wyłączenie z wyborem źródła, kolumny par kart |
| Ręcznie (Windows) | prawdziwa karta wielokanałowa (TESTING.md) |

## 8. Ryzyka

| Ryzyko | Ograniczenie |
|---|---|
| Dodatkowe opóźnienie miksu (najwolniejsze źródło + 2 ms) | Tylko cele w trybie miksu; jednoźródłowy 0 dB zostaje bezpośredni; opóźnienie widoczne w oknie celu |
| Miks w torze emisyjnym | Tryb miksu włączany świadomie na celu; domyślnie zastępowanie jak dotąd |
| Pamięć +16 MB w 32-bitowym mAirList | ~57 MB widoku; w razie problemu zmniejszyć liczbę szyn do 16 (stała w `shm_layout.hpp`) |
| Niezgodność wersji usługi i sterownika (v3) | Instalator wymienia wszystko naraz; czytelny komunikat przy niezgodności |
| Format mieszania karty może być stereo mimo wielu wyjść sprzętowych | Kolumny par wynikają z formatu Windows; opis w INSTALL.md (konfiguracja głośników w Panelu dźwięku) |

## 9. Wersja

1.2.0; konfiguracja `"version": 2` (nowe pole opcjonalne); pamięć współdzielona v3.
