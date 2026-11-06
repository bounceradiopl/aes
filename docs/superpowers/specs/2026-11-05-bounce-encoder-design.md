# Bounce Encoder 1.0 — enkoder AES67 → Icecast

- **Data:** 2026-11-05
- **Status:** zatwierdzony w rozmowie, do przeglądu pisemnego
- **Repozytorium:** to samo co BounceAES Master (`bounceradiopl/aes`), nowa aplikacja na wspólnym rdzeniu

## 1. Cel

Osobny program Windows (.exe), który odbiera strumienie AES67, miksuje je w sygnał stacji i wysyła do serwerów Icecast w wybranych kodekach. Obsługuje wiele stacji naraz. Bez processingu dźwięku i bez RDS/metadanych (robi to inny system).

Decyzje z rozmowy (2026-11-05):

| Pytanie | Decyzja |
|---|---|
| Wejścia stacji | do 3, **sumowane** w jeden sygnał, każde z własnym gainem |
| Strumienie | **kilka** na stację (różne kodeki, serwery, mountpointy) |
| Relacja do BounceAES Master | **osobny program**; BounceAES Master (ASIO, routing) zostaje bez zmian |
| Processing, RDS | **nie** |
| Kod Omnii | nie jest używany ani czytany; wszystko z publicznych standardów i bibliotek open source |

## 2. Zakres

### W zakresie
- Do **16 stacji**; każda włączana/wyłączana, z nazwą.
- Do **3 wejść AES67** na stację: SDP wgrywany z pliku albo wklejany; L16/L24, 48 kHz, mono lub stereo (jak w BounceAES Master); gain −60…+12 dB (krok 0,5 dB); **opóźnienie ręczne 0–60 s, krok 0,1 s**.
- Sumowanie wejść z płynną zmianą gainu i wyciszenia (rampa 5 ms) — ta sama zasada co mikser BounceAES Master 1.2.
- Do **8 strumieni Icecast** na stację, każdy: kodek, bitrate, częstotliwość próbkowania, gain −60…+12 dB, serwer (host lub IP), port, mountpoint, użytkownik, hasło, włączony/wyłączony.
- Kodeki: **MP3** (LAME), **AAC-LC**, **HE-AAC (v1)**, **HE-AACv2** (libfdk-aac), **FLAC** (libFLAC, kontener Ogg).
- Częstotliwości strumienia: 48 000, 44 100, 32 000, 24 000, 22 050 Hz (resampling z 48 kHz; dozwolone kombinacje zależą od kodeka, §5.2).
- Panel WWW z logowaniem: lista stacji, edycja, mierniki wejść / stacji / strumieni, stan połączeń, objaśnienie bitrate’ów AAC↔MP3.
- Usługa Windows (autostart), ikona w zasobniku, instalator NSIS „Bounce Encoder”, instrukcja.

### Poza zakresem
- Processing (kompresja, limiter, AGC), RDS, metadane ICY / tytuły utworów.
- Wysyłanie AES67 (Bounce Encoder tylko odbiera), master PTP (tylko slave — por. §4.1), ASIO, routing.
- Opus/Vorbis, SHOUTcast v2 z wieloma strumieniami na jednym porcie, nagrywanie do plików.

## 3. Architektura

```
            ┌──────────────────────── stacja ──────────────────────────┐
AES67 ─► RX ─► pierścień (oś czasu PTP) ─► bufor opóźnienia ─┐          │
AES67 ─► RX ─► pierścień ─► bufor opóźnienia ────────────────┼─► miks ─┼─► strumień: gain → resampler → enkoder → klient Icecast
AES67 ─► RX ─► pierścień ─► bufor opóźnienia ────────────────┘          │   strumień: …
            └───────────────────────────────────────────────────────────┘
```

Moduły (każdy z jedną odpowiedzialnością, testowalny osobno):

| Moduł | Rola | Pochodzenie |
|---|---|---|
| `core/` RTP, SDP, `RxStream`, `ChannelRing`, PTP slave, zegar medialny | odbiór AES67 na oś czasu | wspólne z BounceAES Master |
| `encoder/delay_line` | bufor opóźnienia wejścia (do 60 s), odczyt z bieżącym opóźnieniem | nowe |
| `encoder/station_mixer` | suma wejść z gainem i rampą 5 ms, mierniki | nowe (zasada z `core/mixer`) |
| `encoder/codecs` | wspólny interfejs `AudioEncoder` + MP3 / AAC / FLAC | nowe |
| `encoder/icecast_client` | połączenie, uwierzytelnienie, wysyłanie, ponawianie | nowe |
| `encoder/stream_worker` | gain strumienia, resampling (libsamplerate), enkoder, kolejka do klienta | nowe |
| `encoder/encoder_engine` | stacje, wątki, konfiguracja, status | nowe |
| `encoder/http` + `encoder/panel` | API i panel WWW | nowe, na wzorcu `engine/http_server` |
| `encoder_service` | usługa Windows / tryb konsolowy | na wzorcu `service/` |

## 4. Tor sygnału

### 4.1 Wejścia
- Każde wejście to `RxStream` piszący na własną parę pierścieni na osi czasu medialnego (PTP), jak subskrypcja w BounceAES Master. Bounce Encoder jest **PTP slave** (domena konfigurowalna, domyślnie 0); bez mastera w sieci działa na zegarze własnym (free-run), a stan PTP widać w panelu.
- Wątek stacji co 10 ms czyta z pierścieni bloki do `now − opóźnienie_odbioru` (jak cel w BounceAES Master) i dopisuje je do **bufora opóźnienia** wejścia.

### 4.2 Bufor opóźnienia
- Kolejka próbek float stereo o pojemności 60 s + zapas. Odczyt z przesunięciem `delay_s × 48000`. Zmiana opóźnienia w trakcie pracy: przeskok odczytu z wyciszeniem/podgłośnieniem 5 ms (bez trzasku); zwiększenie opóźnienia wstawia ciszę o długości różnicy.
- Brak pakietów wejścia = cisza (jak w BounceAES Master), nie przerwa w strumieniu.

### 4.3 Miks stacji
- Suma wejść (float), każde z liniowym gainem z dB; rampa 5 ms przy zmianie gainu, wyciszeniu, dodaniu i usunięciu wejścia.
- Sygnał stacji 48 kHz stereo trafia do wszystkich strumieni stacji.

### 4.4 Strumień
- Gain strumienia (rampa 5 ms) → resampler (libsamplerate SINC_MEDIUM, stały współczynnik 48000 → wybrana częstotliwość) → enkoder → kolejka ramek → klient Icecast.
- Enkodowanie w **osobnym wątku na strumień**, żeby wolny kodek albo serwer nie wstrzymywał innych strumieni ani stacji. Kolejka między wątkiem stacji a strumieniem ma limit 5 s; przepełnienie (strumień nie nadąża) gubi najstarsze dane i liczy to w statusie.
- Przesterowanie po gainie: przycięcie (saturacja), nie zawijanie.

## 5. Kodeki

### 5.1 Interfejs
```cpp
struct EncoderSettings { Codec codec; uint32_t bitrate_kbps; uint32_t sample_rate; int channels = 2; };
class AudioEncoder {
 public:
  virtual ~AudioEncoder() = default;
  virtual bool open(const EncoderSettings&, std::string* error) = 0;
  virtual void encode(const float* interleaved, size_t frames, std::vector<uint8_t>& out) = 0;  // appends bytes ready to send
  virtual void flush(std::vector<uint8_t>& out) = 0;
  virtual const char* content_type() const = 0;  // "audio/mpeg", "audio/aac", "audio/aacp", "audio/ogg"
};
std::unique_ptr<AudioEncoder> make_encoder(Codec);  // nullptr when the codec library is not available
bool codec_available(Codec);
```

### 5.2 Kodeki i dozwolone ustawienia
| Kodek | Biblioteka | Bitrate (kb/s) | Częstotliwości | Content-Type |
|---|---|---|---|---|
| MP3 | LAME (LGPL), CBR | 32–320 (standardowe kroki MPEG) | 48 000, 44 100, 32 000, 24 000, 22 050 | `audio/mpeg` |
| AAC-LC | libfdk-aac, ADTS | 32–320 | 48 000, 44 100, 32 000, 24 000, 22 050 | `audio/aac` |
| HE-AAC (v1) | libfdk-aac (AOT 5), ADTS | 24–128 | 48 000, 44 100, 32 000 | `audio/aacp` |
| HE-AACv2 | libfdk-aac (AOT 29), ADTS, stereo | 16–64 | 48 000, 44 100, 32 000 | `audio/aacp` |
| FLAC | libFLAC + libogg, poziom 5, 16 bit | — (bezstratny) | 48 000, 44 100, 32 000, 24 000, 22 050 | `audio/ogg` |

Walidacja konfiguracji odrzuca kombinacje spoza tabeli z polskim komunikatem.

### 5.3 Licencje i dołączanie bibliotek
- **libfdk-aac** ma licencję Fraunhofera niezgodną z GPL; AAC jest objęty patentami. Ładowany w czasie pracy z **osobnego pliku `libfdk-aac-2.dll`** obok programu (`LoadLibrary`). Brak pliku → AAC/HE-AAC wyszarzone w panelu z wyjaśnieniem; MP3 i FLAC działają.
- **LAME** (LGPL) — także osobna DLL (`libmp3lame.dll`), ładowana w czasie pracy (zgodność z LGPL).
- **libFLAC, libogg** (BSD) — mogą być wkompilowane.
- Repozytorium nie zawiera binariów fdk-aac ani LAME; skrypt budowania (CI) pobiera i buduje je ze źródeł do instalatora. Dokumentacja: do użytku wewnętrznego, bez publicznego rozpowszechniania instalatora z fdk-aac bez licencji patentowej.

## 6. Klient Icecast
- TCP, `PUT /<mountpoint> HTTP/1.1` z `Authorization: Basic`, `Content-Type`, `Ice-Public: 0`, `Ice-Name: <nazwa stacji>`, `Expect: 100-continue`; odpowiedź 2xx/100 → wysyłanie danych bez `Content-Length` (strumień ciągły). Gdy serwer odrzuci `PUT` (405/400) — próba starym protokołem `SOURCE /<mountpoint> ICE/1.0`.
- Błędy: 401 → „złe hasło lub użytkownik”, 403 → „mountpoint zajęty lub zabroniony”, brak połączenia → „serwer nieosiągalny”; ponawianie co 1, 2, 5, 10 s (dalej co 10 s); po ponownym połączeniu enkoder zaczyna od nowej ramki (dla FLAC/Ogg nowy nagłówek strumienia).
- Status strumienia: stan (łączy / nadaje / błąd + opis), wysłane bajty, bitrate rzeczywisty, liczba ponowień, odrzucone dane przy przepełnieniu.
- Hasła przechowywane w konfiguracji (plik w `%ProgramData%`, dostęp tylko dla administratora i usługi); w API zwracane jako puste (`"password": ""` = bez zmiany).

## 7. Konfiguracja i API

```json
{
  "version": 1,
  "interface_ip": "", "ptp_domain": 0, "http_bind": "0.0.0.0", "http_port": 8070, "password_hash": "...",
  "stations": [
    { "id": "s1", "name": "Radio Bounce", "enabled": true,
      "inputs": [ { "enabled": true, "name": "", "sdp": "v=0…", "gain_db": 0, "delay_s": 0.0, "latency_us": 5000 } ],
      "streams": [ { "enabled": true, "codec": "mp3", "bitrate_kbps": 128, "sample_rate": 44100, "gain_db": 0,
                     "host": "stream.example", "port": 8000, "mount": "/radio.mp3", "user": "source", "password": "…" } ] } ]
}
```
- Port panelu domyślnie **8070** (BounceAES Master ma 8067 — oba mogą działać na jednym komputerze).
- API: `GET/PUT /api/config` (hasła maskowane), `POST/DELETE /api/stations`, `GET /api/status` (stacje, wejścia, strumienie, PTP), `GET /api/codecs` (dostępność i dozwolone ustawienia), logowanie jak w BounceAES Master.

## 8. Panel
- Ciemny, minimalistyczny styl BounceAES Master, logo bounce z dopiskiem „Encoder”.
- Zakładki: **Stacje** (lista z miernikami i stanem strumieni; dodaj/usuń/włącz), **Edycja stacji** (wejścia: SDP z pliku/wklejone, gain, opóźnienie; strumienie: wszystkie pola z §2), **Statystyki**, **Logi**.
- Pole bitrate pokazuje odpowiednik MP3 i podpowiedź kodeka:

| AAC | ≈ MP3 |
|---|---|
| HE-AACv2 32 kb/s | ~96 kb/s |
| HE-AAC 48–64 kb/s | ~128 kb/s |
| AAC-LC 96 kb/s | ~128–160 kb/s |
| AAC-LC 128 kb/s | ~192 kb/s |
| AAC-LC 192+ kb/s | ~256–320 kb/s |

Podpowiedź: HE-AACv2 do 48 kb/s, HE-AAC 48–96 kb/s, AAC-LC powyżej 96 kb/s.

## 9. Testy

| Obszar | Test |
|---|---|
| Bufor opóźnienia | dokładne przesunięcie (próbka w próbkę), zmiana opóźnienia bez skoku > rampy, 60 s pojemności |
| Miks stacji | suma z gainem, rampa przy zmianie, wyciszenie, saturacja |
| FLAC | zakoduj → zdekoduj libFLAC → **bit w bit** równe wejściu (16 bit); poprawne strony Ogg |
| MP3 | poprawne nagłówki ramek, CBR = zadany bitrate, czas trwania; po zdekodowaniu (mpg123 lub dekoder testowy) korelacja z wejściem > 0,99 przy sinusie |
| AAC / HE-AAC / HE-AACv2 | nagłówki ADTS (profil, częstotliwość, kanały); dekoder fdk-aac → poziom RMS i korelacja z wejściem; test pomijany z komunikatem, gdy `libfdk-aac` niedostępne |
| Resampler | 48 → 44,1 / 32 / 24 / 22,05 kHz: poziom sinusa zachowany, długość zgodna |
| Klient Icecast | atrapa serwera na 127.0.0.1: nagłówki, Basic auth, 401/403/odmowa, fallback `SOURCE`, ponawianie z odstępami, ciągłość danych po ponownym połączeniu |
| Cały tor | AES67 (atrapa nadawcy na 127.0.0.1) → opóźnienie 2 s → miks 2 wejść → FLAC → atrapa Icecast → dekodowanie → zgodność bit w bit i zmierzone opóźnienie |
| Konfiguracja | walidacja limitów i kombinacji kodek/bitrate/częstotliwość, maskowanie haseł |
| Panel | przeglądarka, tylko 127.0.0.1 |
| Ręcznie | prawdziwy Icecast, odsłuch strumieni w odtwarzaczu (VLC/przeglądarka), 24 h stabilności |

## 10. Ryzyka

| Ryzyko | Ograniczenie |
|---|---|
| Licencja/patenty AAC | fdk-aac jako osobna DLL, nie w repozytorium; dokumentacja „do użytku wewnętrznego” |
| Budowanie fdk-aac i LAME dla MinGW i MSVC | skrypt CI budujący ze źródeł; w razie problemu MinGW buduje oba, MSVC ładuje te same DLL (C ABI) |
| Wolny serwer / sieć | osobny wątek na strumień, kolejka 5 s, gubienie najstarszych danych liczone w statusie |
| Dryft zegara nadawcy bez PTP | free-run na zegarze własnym; przy długiej pracy bez PTP możliwe pojedyncze uzupełnienia/odrzucenia pakietów (widoczne w statystykach) |
| CPU przy wielu strumieniach | HE-AAC ~2–4 % rdzenia na strumień; 16 stacji × 8 strumieni = limit teoretyczny, w praktyce mniej — opisane w instrukcji |

## 11. Wersja
Bounce Encoder 1.0.0; konfiguracja `"version": 1`; BounceAES Master bez zmian.
