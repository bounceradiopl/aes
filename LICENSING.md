# Licencjonowanie BounceAES Master i Bounce Encoder

BounceAES Master i Bounce Encoder są udostępniane na licencji **GPL-3.0-or-later** (plik
`LICENSE`). Ta licencja obowiązuje każdego odbiorcę i się nie zmienia.

## Osobna licencja dla Ricochetu

Jedynym autorem i właścicielem praw autorskich do kodu BounceAES Master i Bounce Encoder (poza
katalogiem `third_party/` oraz kodeków i Stereo Tool pobieranych przy budowaniu) jest Daniel Bobrowski. Jako
właściciel praw udziela on dodatkowej, osobnej licencji na ten kod dla systemu emisyjnego
**Ricochet** (Bounce):

- Ricochet może używać kodu BounceAES Master i Bounce Encoder (w tym `encoder/`,
  `encoder_service/`, `encoder_tray/`, `http/`), przenosić go (także przepisywać na inny język,
  np. Rust), zmieniać i rozpowszechniać na własnej, zamkniętej licencji Ricochetu, bez
  obowiązków z GPLv3;
- ta licencja nie zmienia licencji GPLv3 dla innych odbiorców BounceAES Master i Bounce Encoder;
- kod, który Ricochet przejmie, ma w Ricochecie notkę o pochodzeniu z BounceAES Master albo
  Bounce Encoder.

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

## Stereo Tool (processing Bounce Encoder 1.1)

Processing stacji w Bounce Encoder 1.1 używa biblioteki **Stereo Tool** (`libStereoTool`, wersja
11.07) firmy **Thimeo Holding B.V.** (© Thimeo Holding B.V.).

- To **biblioteka komercyjna, nie GPL** i bez dostępnego kodu źródłowego. Nie jest częścią kodu
  Bounce Encoder ani tego repozytorium.
- Bounce Encoder ładuje ją **w czasie pracy jako osobny plik** (`libStereoTool_64.dll`), tylko z
  folderu programu. Deklaracje używanych funkcji są w kodzie Bounce Encoder (`encoder/src/
  stereotool_api.hpp`); nagłówki Thimeo z SDK służą tylko do sprawdzenia przy budowaniu, że te
  deklaracje się zgadzają, i nie trafiają do repozytorium ani do instalatora.
- CMake pobiera SDK z https://download.thimeo.com/Stereo_Tool_Generic_plugin.zip i sprawdza sumy
  SHA-256 (`cmake/stereotool.cmake`): archiwum
  `ed07c086cd95523abf5e7d655fa1f32e556fca2539ff0e85b4e12ff3a4c3ec22`, `libStereoTool_64.dll`
  `48cbdc06f44a203f7fa2895b3ec79fc63557d61dcd687563c6a17388d6308c4e`.
- **Jedna licencja Stereo Tool na jedną instancję**: każda stacja z włączonym processingiem
  potrzebuje własnego klucza licencji od Thimeo.
- Biblioteka jest dołączona do instalatora Bounce Encoder **dla Bounce** (BOUNCE SP. Z O.O. jest
  licencjobiorcą Stereo Tool). **Przekazanie instalatora albo pliku `libStereoTool_64.dll` poza
  Bounce wymaga zgody Thimeo**, a odbiorca potrzebuje własnych licencji Stereo Tool. Bez tego pliku
  Bounce Encoder działa jak wersja 1.0: processing jest niedostępny, stacje nadają sygnał bez
  processingu.
- Instalator kopiuje notę `StereoTool-NOTICE.txt` (`cmake/stereotool/`) do `licencje\` w
  folderze programu.

## Dodatkowe uprawnienie dla FDK AAC i Stereo Tool (GPLv3, sekcja 7)

Licencja Fraunhofer FDK AAC i licencja Stereo Tool nie są zgodne z GPL. Żeby Bounce Encoder razem z
fdk-aac i ze Stereo Tool można było legalnie używać i rozpowszechniać, właściciel praw autorskich do
Bounce Encoder dodaje do jego licencji GPLv3 poniższe dodatkowe uprawnienia (sekcja 7 GPLv3).
Obowiązuje tekst angielski:

> **Additional permission under GNU GPL version 3 section 7**
>
> If you modify this Program, or any covered work, by linking or combining it with the Fraunhofer
> FDK AAC Codec Library (or a modified version of that library), containing parts covered by the
> terms of the Software License for The Fraunhofer FDK AAC Codec Library for Android, the licensors
> of this Program grant you additional permission to convey the resulting work. Corresponding Source
> for a non-source form of such a combination shall include the source code for the parts of the
> Fraunhofer FDK AAC Codec Library used as well as that of the covered work.
>
> If you modify this Program, or any covered work, by linking or combining it with libStereoTool,
> the Stereo Tool library of Thimeo Holding B.V. (or a modified version of that library), containing
> parts covered by the terms of Thimeo Holding B.V.'s licence for that library, the licensors of this
> Program grant you additional permission to convey the resulting work.

Po polsku: Bounce Encoder można łączyć z biblioteką Fraunhofer FDK AAC i przekazywać dalej razem z
nią; kod źródłowy do przekazanych plików obejmuje wtedy także źródła fdk-aac (archiwum z tabeli
wyżej). Tak samo Bounce Encoder można łączyć z biblioteką Stereo Tool (libStereoTool) i przekazywać
dalej razem z nią; kod źródłowy obejmuje wtedy tylko kod Bounce Encoder (kodu Stereo Tool nie ma).
To uprawnienie dotyczy tylko licencji Bounce Encoder: samo przekazanie Stereo Tool wymaga zgody
Thimeo (wyżej). Uprawnienie dotyczy kodu Bounce Encoder (katalogi `encoder/`, `encoder_service/`,
`encoder_tray/` i używany przez nie kod wspólny projektu). Kto zmienia Bounce Encoder, może to
uprawnienie zachować albo usunąć ze swojej wersji.

## Jak wolno używać plików Bounce Encoder

- **W Bounce** (wszystkie komputery i stacje firmy): bez ograniczeń, z kodekiem AAC. Processing
  Stereo Tool: z kluczem licencji Stereo Tool na każdą stację z processingiem.
- **Przekazanie poza Bounce** (np. stacjom partnerskim): licencje kodu na to pozwalają (GPLv3 z
  powyższym uprawnieniem, LGPL dla LAME, licencja Fraunhofera dla fdk-aac, BSD dla reszty), pod
  warunkiem dołączenia tekstów licencji (folder `licencje\`) i informacji, skąd pobrać kod źródłowy.
  **Patenty AAC to osobna sprawa:** licencja Fraunhofera wprost nie obejmuje patentów, a AAC i
  HE-AAC są objęte patentami (pula Via LA). Przed przekazaniem instalatora z `libfdk-aac-2.dll` poza
  firmę sprawdź licencję patentową albo przekaż pakiet bez tego pliku: wtedy MP3 i FLAC działają,
  a strumienie AAC są w panelu wyłączone z wyjaśnieniem.
  **Stereo Tool:** przekazanie poza Bounce instalatora z `libStereoTool_64.dll` wymaga zgody Thimeo,
  a odbiorca potrzebuje własnych licencji Stereo Tool. Bez zgody przekaż pakiet bez tego pliku:
  wtedy processing jest niedostępny, a reszta działa.

To opis przyjętych zasad, nie porada prawna.

Bez tego pliku Bounce Encoder działa z MP3 i FLAC, a strumienie AAC są wyłączone z wyjaśnieniem w
panelu.

Osobna licencja dla Ricochetu (wyżej) nie obejmuje kodeków ani Stereo Tool: mają one własne licencje (Stereo Tool: licencja Thimeo). Ricochet, który chce używać AAC, korzysta z fdk-aac na licencji Fraunhofera (z jej warunkami i bez licencji patentowej) albo z innego kodera AAC.
