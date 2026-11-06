# Licencjonowanie BounceAES Master i Bounce Encoder

BounceAES Master i Bounce Encoder są udostępniane na licencji **GPL-3.0-or-later** (plik
`LICENSE`). Ta licencja obowiązuje każdego odbiorcę i się nie zmienia.

## Osobna licencja dla Ricochetu

Jedynym autorem i właścicielem praw autorskich do kodu BounceAES Master (poza katalogiem
`third_party/`) jest Daniel Bobrowski. Jako właściciel praw udziela on dodatkowej, osobnej
licencji na ten kod dla systemu emisyjnego **Ricochet** (Bounce):

- Ricochet może używać kodu BounceAES Master, przenosić go (także przepisywać na inny język,
  np. Rust), zmieniać i rozpowszechniać na własnej, zamkniętej licencji Ricochetu, bez
  obowiązków z GPLv3;
- ta licencja nie zmienia licencji GPLv3 dla innych odbiorców BounceAES Master;
- kod, który Ricochet przejmie, ma w Ricochecie notkę o pochodzeniu z BounceAES Master.

## Kod firm trzecich (`third_party/`)

Osobna licencja nie obejmuje kodu firm trzecich, który ma własne licencje:

| Katalog | Licencja |
|---|---|
| `third_party/doctest` | MIT |
| `third_party/httplib` | MIT |
| `third_party/nlohmann` | MIT |
| `third_party/libsamplerate` | BSD-2-Clause |
| `third_party/asiosdk` | Steinberg ASIO License albo GPLv3 (do wyboru) |

ASIO SDK w BounceAES Master jest używane na GPLv3. Ricochet, który nie jest na GPL, może
używać ASIO tylko po podpisaniu (bezpłatnej) licencji Steinberg ASIO i bez rozpowszechniania
plików SDK.

## Kodeki Bounce Encoder

Bounce Encoder (katalogi `encoder/`, `encoder_service/`, `encoder_tray/`) jest na GPLv3 jak reszta
projektu. Kodeki nie są w repozytorium: CMake pobiera je z oficjalnych archiwów źródłowych,
sprawdza sumy SHA-256 (`cmake/codecs.cmake`) i buduje razem z projektem. Teksty licencji trafiają
przy budowaniu do folderu `encoder-licenses/` obok programów, a instalator kopiuje je do
`licencje\` w folderze programu.

| Biblioteka | Wersja | Licencja | Jak jest dołączona |
|---|---|---|---|
| LAME (MP3) | 3.100 | LGPL-2.0-or-later | osobna biblioteka `libmp3lame.dll`, ładowana w czasie pracy (można ją wymienić na własną wersję) |
| Fraunhofer FDK AAC (AAC-LC, HE-AAC, HE-AACv2) | 2.0.3 | Software License for The Fraunhofer FDK AAC Codec Library for Android | osobna biblioteka `libfdk-aac-2.dll`, ładowana w czasie pracy |
| libogg | 1.3.5 | BSD-3-Clause | wkompilowana statycznie |
| libFLAC | 1.4.3 | BSD-3-Clause | wkompilowana statycznie |
| libsamplerate | 0.2.2 | BSD-2-Clause | wkompilowana statycznie (`third_party/libsamplerate`) |

**FDK AAC.** Licencja Fraunhofera nie jest zgodna z GPL i nie obejmuje patentów: AAC (w tym HE-AAC)
jest objęty patentami, a licencję patentową trzeba uzyskać osobno (np. przez Via LA). Dlatego
fdk-aac jest osobnym plikiem ładowanym w czasie pracy, a nie częścią programu, a instalator
Bounce Encoder z `libfdk-aac-2.dll` jest przeznaczony **wyłącznie do użytku wewnętrznego**
(decyzja właściciela projektu). Nie rozpowszechniaj go publicznie. Bez tego pliku Bounce Encoder
działa z MP3 i FLAC, a strumienie AAC są wyłączone z wyjaśnieniem w panelu.

Osobna licencja dla Ricochetu (wyżej) nie obejmuje kodeków: mają one własne licencje z tabeli.
