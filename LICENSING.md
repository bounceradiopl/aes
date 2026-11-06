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

Źródła kodeków (te same archiwa dla każdego buildu; sumy z `cmake/codecs.cmake`):

| Biblioteka | Archiwum źródłowe | SHA-256 |
|---|---|---|
| LAME 3.100 | https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz | `ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e` |
| fdk-aac 2.0.3 | https://github.com/mstorsjo/fdk-aac/archive/refs/tags/v2.0.3.tar.gz | `e25671cd96b10bad896aa42ab91a695a9e573395262baed4e4a2ff178d6a3a78` |
| libogg 1.3.5 | https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz | `0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664` |
| libFLAC 1.4.3 | https://downloads.xiph.org/releases/flac/flac-1.4.3.tar.xz | `6c58e69cd22348f441b861092b825e591d0b822e106de6eb0ee4d05d27205b70` |

LAME jest budowany przez `cmake/lame/` (bez zmian w kodzie LAME), pozostałe biblioteki ich własnym
CMake. **Kod źródłowy dla odbiorców plików binarnych:** każdy, kto otrzymał instalator albo pliki
Bounce Encoder, może otrzymać odpowiadający im pełny kod źródłowy: to repozytorium
(https://github.com/bounceradiopl/aes) w wersji, z której zbudowano pliki, oraz powyższe archiwa
kodeków. Kto przekazuje pliki binarne dalej, przekazuje razem z nimi ten kod albo informację, skąd
go pobrać.

**FDK AAC.** Fakty:
- licencja Fraunhofer FDK AAC nie jest zgodna z GPL;
- licencja Fraunhofera nie obejmuje patentów: AAC (także HE-AAC) jest objęty patentami, a licencję
  patentową trzeba uzyskać osobno;
- w Bounce Encoder fdk-aac jest osobnym plikiem `libfdk-aac-2.dll`, ładowanym w czasie pracy; to
  nie usuwa niezgodności licencji z GPL;
- instalator i pliki z `libfdk-aac-2.dll` są rozpowszechniane wyłącznie do użytku wewnętrznego
  właściciela projektu. Przekazanie ich poza firmę wymaga decyzji właściciela projektu (licencje
  kodu i patentów).

Bez tego pliku Bounce Encoder działa z MP3 i FLAC, a strumienie AAC są wyłączone z wyjaśnieniem w
panelu.

Osobna licencja dla Ricochetu (wyżej) nie obejmuje kodeków: mają one własne licencje z tabeli.
