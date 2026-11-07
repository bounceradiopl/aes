# Bounce Encoder: instalacja i konfiguracja

Bounce Encoder to osobny program Windows, który odbiera strumienie **AES67**, sumuje je w sygnał stacji i wysyła do serwerów **Icecast** w wybranych kodekach (MP3, AAC-LC, HE-AAC, HE-AACv2, FLAC). Obsługuje do 16 stacji naraz. Nie przetwarza dźwięku (bez kompresji, limitera, AGC) i nie wysyła metadanych ani RDS.

```
AES67 ─► wejście 1 (gain, opóźnienie) ─┐
AES67 ─► wejście 2 (gain, opóźnienie) ─┼─► miks stacji 48 kHz ─┬─► strumień 1: gain → resampler → kodek → Icecast
AES67 ─► wejście 3 (gain, opóźnienie) ─┘                       ├─► strumień 2: …
                                                               └─► … (do 8 strumieni)
```

BounceAES Master (sterownik ASIO) i Bounce Encoder to dwa niezależne programy. Mogą działać na jednym komputerze: panel BounceAES Master ma port 8067, panel Bounce Encoder port 8070.

## 1. Wymagania

- Windows 10 lub 11, **64-bit**.
- Karta sieciowa podłączona do sieci AoIP (najlepiej osobna, w VLAN-ie AoIP), a na przełączniku IGMP snooping z querierem.
- Zegar PTPv2 w sieci (Orban, Omnia, konsola albo BounceAES Master w roli mastera), w tej samej domenie co źródła. Bez zsynchronizowanego PTP Bounce Encoder odbiera tylko źródła, które liczą czas tym samym zegarem co on, w praktyce prawie żadne (rozdział 11).
- Dostęp do serwera Icecast 2.4 (albo nowszego) z hasłem źródła.

## 2. Instalacja

1. Uruchom `BounceEncoder-setup.exe` i potwierdź uprawnienia administratora.
2. Instalator:
   - kopiuje pliki do `C:\Program Files\Bounce Encoder`, razem z bibliotekami kodeków `libmp3lame.dll` (MP3) i `libfdk-aac-2.dll` (AAC). Program ładuje kodeki **tylko ze swojego folderu**. Folder spoza `Program Files` (nowy, pusty albo z poprzednią instalacją) dostaje takie same uprawnienia jak `Program Files`: administratorzy i SYSTEM pełna kontrola, Użytkownicy tylko odczyt i uruchamianie, bez dziedziczenia, bo usługa działa jako SYSTEM i nikt inny nie może podmienić jej plików. Uprawnień istniejącego folderu, w którym są już inne pliki (a nie ma Bounce Encodera), instalator nie zmienia (ostrzega). Jeśli ustawienie uprawnień się nie uda, instalator też ostrzega. Katalog główny dysku nie może być folderem programu;
   - instaluje usługę **Bounce Encoder** (`BounceEncoder`): start automatyczny razem z Windows, restart po awarii po 5 s;
   - rejestruje źródło **Bounce Encoder** w Dzienniku zdarzeń Windows;
   - dodaje regułę zapory dla programu `BounceEncoder.exe`: odbiór AES67 (RTP), PTP 319/320 i panel 8070. Reguła dotyczy tylko tego programu, nie otwiera portów dla innych, ale obowiązuje we **wszystkich profilach sieci** (także „Publiczna”: sieci AoIP Windows zwykle tak klasyfikuje, a bez tego odbiór AES67 by nie działał). Panel jest więc osiągalny z każdej sieci, w której nasłuchuje: ustaw jego adres na kartę sieci administracyjnej (rozdział 4);
   - zakłada folder `C:\ProgramData\Bounce Encoder` z uprawnieniami **tylko dla administratorów i usługi** (SYSTEM), bo `config.json` zawiera hasła strumieni i skrót hasła panelu. Podfolder `public` mogą czytać wszyscy użytkownicy: jest w nim tylko `panel.json` (adres i port panelu dla ikony w zasobniku);
   - dodaje ikonę w zasobniku do autostartu (przy każdym logowaniu);
   - tworzy skrót **Bounce Encoder** na pulpicie i w menu Start (uruchamia ikonę w zasobniku i otwiera panel) oraz w menu Start *Panel Bounce Encoder* (otwiera tylko panel), *Instrukcja* i *Odinstaluj Bounce Encoder*.
3. Pasek zadań: Windows nie pozwala instalatorom przypinać programów. Kliknij prawym przyciskiem skrót na pulpicie → *Pokaż więcej opcji* → *Przypnij do paska zadań*.

Pliki w `C:\ProgramData\Bounce Encoder\`:

| Plik | Zawartość |
|---|---|
| `config.json` | stacje, wejścia, strumienie z hasłami Icecast, ustawienia wspólne, skrót hasła panelu |
| `logs\bounce-encoder.log` | log usługi (5 plików po 10 MB, najstarszy jest usuwany) |
| `public\panel.json` | adres i port panelu dla ikony w zasobniku (bez danych poufnych), zapisywany przy każdym starcie usługi |
| `config.json.bad` | kopia pliku, którego nie dało się w całości wczytać (rozdział 12) |

Ostrzeżenia i błędy trafiają też do Dziennika zdarzeń Windows (Aplikacja, źródło „Bounce Encoder”).

**Uprawnienia folderu ustawione ręcznie.** Jeśli instalator zgłosił, że nie mógł ustawić uprawnień, albo folder powstał bez instalatora (np. założył go inny użytkownik), zatrzymaj usługę i uruchom w PowerShellu jako administrator:

```powershell
$d = 'C:\ProgramData\Bounce Encoder'
takeown /F $d /A
icacls $d /setowner '*S-1-5-32-544' /T /C /Q
$a = New-Object System.Security.AccessControl.DirectorySecurity
$a.SetSecurityDescriptorSddlForm('D:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)', 'Access')
(Get-Item -LiteralPath $d).SetAccessControl($a)
icacls "$d\*" /reset /T /C /Q
New-Item -ItemType Directory -Force "$d\public" | Out-Null
icacls "$d\public" /grant:r '*S-1-5-32-545:(OI)(CI)RX'
```

Kolejno:
1. Administratorzy przejmują folder i wszystko w nim (właściciel folderu może zawsze zmienić jego uprawnienia).
2. Folder dostaje jednym poleceniem docelowe uprawnienia (zapisywana jest tylko lista uprawnień, bez właściciela): tylko Administratorzy (`BA`) i SYSTEM (`SY`), pełna kontrola dziedziczona przez wszystko w środku, bez dziedziczenia z `C:\ProgramData`. Folder ani na chwilę nie dziedziczy dostępu Użytkowników z `C:\ProgramData`.
3. Wszystko w folderze traci uprawnienia dodane ręcznie i dziedziczy tylko te z punktu 2.
4. Podfolder `public` dostaje odczyt dla Użytkowników.

`*S-1-5-32-544` to Administratorzy, `*S-1-5-32-545` to Użytkownicy (identyfikatory działają w każdej wersji językowej Windows). Instalator robi to samo przy każdej instalacji i aktualizacji. Jeśli folder zniknie (np. ktoś go usunął), usługa przy starcie zakłada go od razu z tymi samymi uprawnieniami (i `public` z odczytem dla Użytkowników), nigdy z uprawnieniami odziedziczonymi z `C:\ProgramData`.

## 3. Pierwsze uruchomienie panelu

1. Kliknij dwukrotnie ikonę Bounce Encoder w zasobniku albo skrót na pulpicie. Możesz też otworzyć `http://127.0.0.1:8070` w przeglądarce na tym komputerze.
2. Ustaw hasło panelu (co najmniej 8 znaków). Pierwsze hasło można ustawić **tylko z tego komputera**, w panelu otwartym pod adresem `http://127.0.0.1:8070` albo `http://localhost:8070` (nie przez nazwę komputera ani jego adres w sieci: to chroni przed stronami WWW, które podszywają się pod adres tego komputera). W innym przypadku panel pokaże tylko tę informację.
3. Potem panel jest dostępny również z innych komputerów pod `http://<adres-komputera>:8070`, po zalogowaniu. Jeśli panel ma być dostępny spoza sieci firmowej, wystaw go przez odwrotne proxy z HTTPS. Proxy na tym samym komputerze sprawia, że dla usługi każde żądanie przychodzi z tego komputera: pierwsze hasło dałoby się wtedy ustawić z zewnątrz (ustaw je przed uruchomieniem proxy), a nieudane logowania wszystkich klientów liczą się razem, więc blokada po błędnych hasłach dotyczy wszystkich naraz.
4. Otwórz **Ustawienia…** (rozdział 4) i wybierz kartę sieciową AoIP.

**Zmień hasło** (u góry panelu) zmienia hasło. Inne otwarte panele, także na innych komputerach, trzeba wtedy zalogować ponownie.

**Zapomniane hasło.**
1. Zatrzymaj usługę (`services.msc` → Bounce Encoder → Zatrzymaj).
2. Jako administrator otwórz `C:\ProgramData\Bounce Encoder\config.json` w Notatniku. Zmień wartość `"password_hash"` na `""`, a `"http_bind"` na `"127.0.0.1"` (zapamiętaj poprzednią wartość) i zapisz. Pierwsze hasło przyjmuje tylko połączenie z adresu `127.0.0.1`: gdy panel nasłuchuje na adresie karty, przeglądarka na tym komputerze łączy się z adresu tej karty i usługa uzna ją za komputer zdalny.
3. Uruchom usługę, otwórz `http://127.0.0.1:8070` na tym komputerze i ustaw nowe hasło.
4. W **Ustawienia…** wpisz z powrotem poprzedni adres panelu, zapisz i uruchom usługę ponownie.

Stacje i strumienie zostają bez zmian.

Ikona w zasobniku ma litery **BE** (BounceAES Master: **BA**) i kolorową kropkę w prawym dolnym rogu:

| Kolor | Znaczenie |
|---|---|
| zielony | usługa działa, każdy włączony strumień nadaje i każde włączone wejście ma sygnał |
| żółty | usługa działa, ale któryś strumień nie nadaje albo któreś wejście nie ma sygnału |
| czerwony | usługa nie odpowiada |

Podpowiedź ikony pokazuje liczbę nadających strumieni, wejść z sygnałem i stan PTP. Menu ikony ma pozycje *Otwórz panel*, *Statystyki* i *Zakończ*. *Zakończ* zamyka tylko ikonę: usługa i strumienie działają dalej.

## 4. Ustawienia wspólne (przycisk „Ustawienia…”)

![Okno Ustawienia: karta sieciowa AoIP, domena PTP, port i adres panelu](images/encoder/ustawienia.png)

| Pole | Opis |
|---|---|
| Karta sieciowa AoIP | karta, na której usługa odbiera strumienie AES67 i zegar PTP. „Automatycznie” wybiera pierwszą aktywną kartę i sprawdza ją co około 2 s: jeśli karta dostanie adres dopiero po starcie usługi albo go zmieni, wejścia i PTP same przechodzą na nowy adres (strumienie nadają dalej). Przy kilku kartach wybierz właściwą ręcznie. Zmiana działa od razu |
| Domena PTP | taka jak w grandmasterze (w SDP z Orbana: `a=clock-domain:PTPv2 0`). Zmiana działa od razu |
| Adres panelu | `0.0.0.0`: panel dostępny ze wszystkich kart sieciowych; `127.0.0.1`: tylko z tego komputera; adres jednej karty: tylko z tej sieci |
| Port panelu | domyślnie 8070 |

Adres i port panelu zmieniają się po ponownym uruchomieniu usługi (`services.msc` → Bounce Encoder → Uruchom ponownie). Ikona w zasobniku i skrót *Panel Bounce Encoder* w menu Start same odczytają nowy adres (z `public\panel.json`). Link na ostatniej stronie instalatora prowadzi zawsze do `http://127.0.0.1:8070`.

**Zalecenie:** ustaw adres panelu na kartę sieci administracyjnej (biurowej), a nie na kartę AoIP ani `0.0.0.0`. Panel ma logowanie, ale powinien być dostępny tylko z zaufanej sieci (rozdział 13).

## 5. Stacje (zakładka Stacje)

![Lista stacji z miernikami wejść i strumieni](images/encoder/stacje.png)

Stacja to jeden sygnał (np. jeden program radiowy): do 3 wejść AES67, które są sumowane, i do 8 strumieni Icecast z tym samym dźwiękiem w różnych kodekach albo na różnych serwerach. Bounce Encoder obsługuje do **16 stacji**.

Lista pokazuje dla każdej stacji:
- przełącznik włączenia: działa od razu, bez zapisywania. Wyłączona stacja rozłącza wszystkie swoje strumienie;
- stan („działa” i czas działania), miernik sygnału stacji i szczyt w dBFS;
- wejścia: nazwa, adres strumienia AES67, gain i opóźnienie, miernik, stan („sygnał”, „brak sygnału” albo błąd);
- strumienie: kodek, bitrate i częstotliwość, serwer i mountpoint, gain, miernik, stan (lampka **NA ANTENIE** z bitrate'em, „łączenie…”, komunikat błędu z rozdziału 9).

**Dodaj stację** otwiera edytor nowej stacji. **Edytuj** otwiera edytor istniejącej, **Usuń** usuwa stację od razu (po potwierdzeniu) i rozłącza jej strumienie.

### Edycja stacji
Zakładka **Edycja stacji** ma pole wyboru stacji, nazwę, przełącznik „stacja włączona”, wejścia i strumienie. Zmiany wysyła przycisk **Zapisz stację** na dole ekranu (**Odrzuć** je cofa). Zapis dotyczy tylko edytowanej stacji, inne stacje grają dalej bez przerwy. W edytowanej stacji od nowa łączą się tylko strumienie, które się zmieniły. Wyjątek: **zmiana nazwy stacji** łączy od nowa wszystkie jej włączone strumienie (nazwa stacji jest nazwą strumienia `Ice-Name` na serwerze), więc słuchacze usłyszą krótką przerwę; panel ostrzega o tym na pasku zapisu. Błędy (np. mountpoint bez „/”) panel pokazuje czerwonym kolorem przy polu i nic nie zapisuje, dopóki ich nie poprawisz.

Jeśli ktoś w tym czasie zmieni tę samą stację w innym panelu, panel zapyta przed zapisem, czy zastąpić tamte zmiany.

## 6. Wejścia AES67

![Edytor stacji: dwa wejścia AES67 z SDP, gainem i opóźnieniem](images/encoder/wejscia.png)

Każde wejście to jeden odbierany strumień AES67: L16 albo L24, 48 kHz, mono lub stereo, multicast. Wejścia stacji są sumowane, każde z własnym gainem.

| Pole | Opis |
|---|---|
| włączone | wyłączone wejście nie trafia do miksu (płynnie, rampa 5 ms) |
| SDP źródła | opis strumienia: wklej tekst SDP albo kliknij **Wczytaj plik SDP…** (plik `.sdp`, do 64 KB). Pod polem panel pokazuje odczytany adres, format, częstotliwość, kanały i ptime |
| Nazwa | nazwa wejścia w panelu; puste pole oznacza nazwę sesji z SDP |
| Gain wejścia | −60…+12 dB, co 0,5 dB. Zmiana jest płynna (rampa 5 ms) |
| Opóźnienie | 0–60 s, co 0,1 s: przesuń suwak albo wpisz wartość w pole obok |
| Opóźnienie odbioru | bufor na jitter sieci, w ms. Co najmniej ptime źródła + 1 ms (dla Orbana z ptime 1,333 ms: 2,334 ms), najwyżej 1000 ms. Domyślnie 5 ms |

Skąd wziąć SDP:
- **Orban Optimod**: w ustawieniach wyjścia AES67 (przycisk pobrania SDP). Przykład jest w folderze programu: `przyklad-orban.sdp`.
- **Konsola** albo inne urządzenie AES67: plik SDP strumienia z jego panelu.
- **BounceAES Master**: przycisk **SDP** przy wyjściu AoIP (*Kopiuj* albo *Pobierz plik .sdp*).

**Opóźnienie** przesuwa wejście w czasie, np. żeby zgrać studio z nadajnikiem albo dać czas na reakcję. Zmiana w trakcie pracy przechodzi płynnie (wyciszenie i podgłośnienie 5 ms, bez trzasku): zwiększenie opóźnienia wstawia ciszę o długości różnicy, zmniejszenie przeskakuje do przodu. Każde włączone wejście ma bufor na pełne 60 s (około 23 MB pamięci).

Jedno źródło (ten sam adres multicast, port i nadawca) może być tylko w jednym włączonym wejściu stacji: dwa takie wejścia zsumowałyby ten sam dźwięk dwa razy (+6 dB), więc panel nie zapisze takiej stacji.

Zmiana samego opóźnienia odbioru nie kasuje opóźnienia wejścia. Zmiana SDP to nowe źródło: bufor opóźnienia zaczyna się od ciszy.

Brak pakietów na wejściu oznacza ciszę, a nie przerwę w strumieniach: słuchacze słyszą ciszę, połączenia z Icecastem trwają. Stacja bez wejść też nadaje ciszę.

## 7. Strumienie Icecast

![Strumienie: kodek, częstotliwość, bitrate z odpowiednikiem MP3, serwer, mountpoint, użytkownik, hasło](images/encoder/strumienie-bitrate.png)

Każdy strumień wysyła sygnał stacji do jednego mountpointu na serwerze Icecast.

| Pole | Opis |
|---|---|
| włączony | wyłączony strumień rozłącza się z serwerem |
| Kodek | MP3, AAC-LC, HE-AAC, HE-AACv2 albo FLAC (rozdział 8). Niedostępny kodek jest wyszarzony z opisem przyczyny |
| Częstotliwość | 48 / 44,1 / 32 / 24 / 22,05 kHz, zależnie od kodeka |
| Bitrate | lista zawiera tylko wartości dozwolone dla kodeka i częstotliwości. Przy AAC pod polem jest odpowiednik MP3 i podpowiedź profilu. FLAC nie ma bitrate'u |
| Gain strumienia | −60…+12 dB, co 0,5 dB, tylko dla tego strumienia. Sygnał powyżej 0 dBFS jest przycinany, nie zawija się |
| Serwer Icecast | nazwa serwera albo adres IPv4, bez `http://` i bez portu |
| Port | port serwera Icecast, zwykle 8000 |
| Mountpoint | zaczyna się od „/”, bez spacji, „?” i „#”, np. `/radio.mp3`. Ten sam serwer i mountpoint może mieć tylko jeden strumień, który nadaje: porównywane są tylko włączone strumienie włączonych stacji. Wyłączony strumień i wyłączona stacja (np. zapasowa) mogą mieć te same mountpointy |
| Użytkownik | zwykle `source` (puste pole oznacza `source`) |
| Hasło | hasło źródła Icecast. Wymagane dla włączonego strumienia |

**Hasła.** Panel nigdy nie pokazuje zapisanych haseł. Puste pole oznacza „bez zmian” (podpowiedź „zapisane (puste = bez zmian)”). Po zmianie serwera albo portu panel prosi: **„po zmianie serwera wpisz hasło ponownie”**. Bounce Encoder nie wyśle zapisanego hasła do innego serwera, więc bez nowego hasła strumienia nie da się zapisać.

**Zmiana strumienia w trakcie pracy** (np. bitrate'u): po zapisaniu ten strumień zamyka połączenie i łączy się od nowa, z nowymi ustawieniami. Inne strumienie i stacje nadają bez przerwy.

Każdy strumień ma własny wątek i dwie kolejki, więc wolny serwer albo sieć nie wstrzymuje innych strumieni ani stacji:
- **dźwięk przed kodekiem** (5 s): gdy kodek nie nadąża (przeciążony procesor), najstarsze ramki są odrzucane i liczone w Statystykach („Ramki: kolejka”);
- **dane do serwera** (5 s przy ustawionym bitrate, ale co najmniej 64 KB, więc przy niskich bitrate'ach dłużej: np. 64 KB to około 16 s przy 32 kb/s; FLAC: 5 s dźwięku przed kompresją): gdy serwer albo łącze nie nadąża, cała zaległa kolejka jest odrzucana (liczona w „Odrzucone”), a kodek zaczyna strumień od nowa od nowej ramki (FLAC: od nowego nagłówka). Słuchacze słyszą krótką przerwę, ale strumień zostaje poprawny i połączenie trwa.

## 8. Kodeki i bitrate

| Kodek | Biblioteka | Bitrate (kb/s) | Częstotliwości | Content-Type |
|---|---|---|---|---|
| MP3 | LAME, CBR, joint stereo | 32–320 przy 48 / 44,1 / 32 kHz (32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320); 32–160 przy 24 / 22,05 kHz (32 … 128, 144, 160) | 48 000, 44 100, 32 000, 24 000, 22 050 | `audio/mpeg` |
| AAC-LC | Fraunhofer FDK AAC, ADTS | 32–320 (przy 24 kHz najwyżej 288, przy 22,05 kHz najwyżej 264) | 48 000, 44 100, 32 000, 24 000, 22 050 | `audio/aac` |
| HE-AAC (v1) | FDK AAC, ADTS | 24–128 | 48 000, 44 100, 32 000 | `audio/aacp` |
| HE-AACv2 | FDK AAC, ADTS | 16–64 | 48 000, 44 100, 32 000 | `audio/aacp` |
| FLAC | libFLAC w kontenerze Ogg, poziom 5, 16 bit | bezstratny (zwykle 700–1000 kb/s przy 48 kHz, zależnie od materiału) | 48 000, 44 100, 32 000, 24 000, 22 050 | `audio/ogg` |

Wszystkie strumienie są stereo. Sygnał stacji ma 48 kHz; inne częstotliwości powstają przez resampling (libsamplerate). Panel odrzuca kombinacje, przy których kodek po cichu nadawałby z niższym bitrate'em niż ustawiony (np. MP3 192 kb/s przy 22,05 kHz).

### AAC a MP3
Przy tym samym brzmieniu AAC potrzebuje mniejszego bitrate'u niż MP3. Panel pokazuje odpowiednik pod polem Bitrate, a pełną tabelę pod przyciskiem *Tabela: AAC a MP3*:

| AAC | ≈ MP3 |
|---|---|
| HE-AACv2 32 kb/s | ~96 kb/s |
| HE-AAC 48–64 kb/s | ~128 kb/s |
| AAC-LC 96 kb/s | ~128–160 kb/s |
| AAC-LC 128 kb/s | ~192 kb/s |
| AAC-LC 192+ kb/s | ~256–320 kb/s |

**Który profil AAC:** HE-AACv2 do 48 kb/s, HE-AAC 48–96 kb/s, AAC-LC powyżej 96 kb/s. Gdy wybrany profil nie pasuje do bitrate'u, panel podpowiada lepszy (np. „przy 128 kb/s lepiej: AAC-LC”).

Wskazówki:
- Słuchacze w przeglądarkach i aplikacjach mobilnych: AAC-LC 128 kb/s albo HE-AAC 64 kb/s. Starsze odtwarzacze i urządzenia: MP3 128 kb/s.
- Słaby zasięg mobilny: HE-AACv2 32–48 kb/s.
- Przekaz do innego studia albo nadajnika, archiwum: FLAC. Ogg FLAC odtwarzają m.in. VLC i foobar2000; przeglądarki różnie, więc dla słuchaczy wybierz MP3 albo AAC. Bounce Encoder wysyła stronę Ogg co najmniej co ~250 ms dźwięku, więc Icecast nie rozłącza źródła nawet przy ciszy.
- Obciążenie procesora: HE-AAC to około 2–4% jednego rdzenia na strumień. 16 stacji × 8 strumieni to limit konfiguracji, a nie zalecenie: przy wielu strumieniach sprawdź obciążenie w Menedżerze zadań.

**Kodek niedostępny.** Bez pliku `libfdk-aac-2.dll` obok programu AAC, HE-AAC i HE-AACv2 są w panelu wyszarzone („kodek … niedostępny: brak biblioteki libfdk-aac obok programu”), a takie strumienie mają stan „kodek niedostępny”. MP3 (`libmp3lame.dll`) i FLAC działają dalej. Zainstaluj program ponownie, żeby przywrócić pliki.

## 9. Serwer Icecast i komunikaty błędów

### Ustawienia po stronie Icecasta (`icecast.xml`)
- **Hasło źródła**: `<authentication><source-password>` dla wszystkich mountpointów albo `<mount>` z własnym `<username>` i `<password>` dla jednego mountpointu. Te same dane wpisz w polach Użytkownik i Hasło.
- **Limit źródeł**: `<limits><sources>` musi być co najmniej równy liczbie strumieni wysyłanych na ten serwer (ze wszystkich enkoderów).
- **Source timeout**: `<limits><source-timeout>` (domyślnie 10 s) może zostać. Bounce Encoder wysyła dane bez przerwy, także przy ciszy.
- Bounce Encoder łączy się metodą `PUT` (Icecast 2.4 i nowsze), z `Expect: 100-continue`. Starszy serwer, który odrzuci `PUT`, dostaje automatycznie starsze żądanie `SOURCE`.
- Strumienie są wysyłane z `Ice-Public: 0`, więc nie trafiają do publicznych katalogów stacji. Nazwa strumienia (`Ice-Name`) to nazwa stacji.
- Połączenie z Icecastem jest zwykłym TCP (HTTP), bez TLS.

### Ponawianie połączenia
Po błędzie strumień próbuje ponownie po 1, 2, 5 i 10 s, potem co 10 s, bez końca. Stan w panelu to wtedy komunikat błędu, a licznik „Ponowienia” w Statystykach rośnie. Po każdym połączeniu kodek zaczyna od nowej ramki (FLAC: od nowego nagłówka strumienia), więc odtwarzacze dostają poprawny strumień.

### Komunikaty
| Stan strumienia | Znaczenie | Co sprawdzić |
|---|---|---|
| nadaje · N kb/s | strumień działa, N to rzeczywisty bitrate | — |
| łączenie… | trwa łączenie z serwerem | — |
| złe hasło lub użytkownik | Icecast odpowiedział 401 | hasło i użytkownik źródła w `icecast.xml` (także w sekcji `<mount>`), wielkość liter |
| mountpoint zajęty lub zabroniony | Icecast odpowiedział 403 | czy inny enkoder nie nadaje na ten mountpoint; limit `<sources>`; czy mountpoint jest dozwolony w konfiguracji serwera. Po zmianie strumienia Icecast bywa, że zwalnia mountpoint chwilę później: kolejna próba się uda |
| serwer nieosiągalny | brak połączenia TCP w ciągu ~10 s | nazwa i port serwera, zapora (wychodzące połączenia), czy Icecast działa |
| nie można znaleźć serwera … | nazwa serwera nie istnieje w DNS | pisownia nazwy, DNS komputera; można wpisać adres IP |
| serwer nie odpowiada | połączenie jest, ale serwer nie odpowiedział w ciągu 5 s | czy pod tym portem działa Icecast, obciążenie serwera |
| nieprawidłowa odpowiedź serwera | odpowiedź nie wygląda jak Icecast | port (np. port HTTPS albo innej usługi), odwrotne proxy przed Icecastem |
| serwer zamknął połączenie | serwer rozłączył się przed odpowiedzią | logi Icecasta (`error.log`) |
| serwer odrzucił połączenie (HTTP N) | inny kod błędu HTTP | logi Icecasta |
| połączenie przerwane | połączenie zerwane w trakcie nadawania | sieć, restart Icecasta; strumień wraca sam |
| serwer nie odbiera danych | przez 10 s serwer nie przyjął żadnych danych | sieć, przeciążony serwer |
| kodek niedostępny | brak biblioteki kodeka | rozdział 8 |
| błąd kodera: … | kodek nie przyjął ustawień albo danych | logi; zgłoś błąd z treścią komunikatu |

## 10. Statystyki i logi

**Statystyki** (także z menu ikony w zasobniku):
- **Zegar PTP**: stan, zegar (PTP albo własny), domena, grandmaster, odchyłka, opóźnienie ścieżki, korekta częstotliwości, utraty synchronizacji;
- **Usługa**: wersja, czas działania, karta sieciowa, liczby stacji, wejść i strumieni;
- **Stacje**: stan, czas działania, pominięte ramki (powinny zostać na 0);
- **Wejścia**: pakiety, bajty, niedobory, przepełnienia (pakiety odrzucone, m.in. spoza okna czasu: bez synchronizacji PTP rosną stale, rozdział 11), błędy sekwencji, jitter, sygnał;
- **Strumienie**: stan, wysłane bajty, kb/s, ponowienia, odrzucone dane, ramki odrzucone w kolejce kodera („Ramki: kolejka”) i przez resampler („Ramki: resampler”: błąd albo brak miejsca przy przeliczaniu częstotliwości, powinny zostać na 0), czas obecnego połączenia z serwerem („Połączony”: liczony od nowa po każdym ponownym połączeniu).

**Logi**: ostatnie 500 wpisów usługi, najnowsze na górze. Pełny log jest w `C:\ProgramData\Bounce Encoder\logs\bounce-encoder.log` (tylko dla administratorów).

## 11. PTP, sieć i strojenie Windows

- Bounce Encoder jest zawsze **PTP slave**: podąża za grandmasterem w wybranej domenie i nigdy nie nadaje zegara. Pasek stanu na dole panelu pokazuje „PTP: zsynchronizowany”, „synchronizacja…” albo „zegar własny”.
- **Bez synchronizacji PTP odbiór zwykle nie działa.** Bez mastera (albo zanim zegar się zsynchronizuje) program liczy czas mediów z własnego zegara komputera (czas systemowy w skali TAI, „zegar własny”). Pakiety AES67 niosą znaczniki czasu na zegarze nadawcy, a wejście przyjmuje tylko pakiety, których czas wypada w oknie od „teraz − opóźnienie odbioru” do „teraz + 0,5 s”. Nadawca na innym zegarze (urządzenie zsynchronizowane z grandmasterem, którego Bounce Encoder nie widzi, albo z własnym zegarem) jest więc odrzucany w całości: w Statystykach rosną **Przepełnienia**, wejście ma stan „brak sygnału”, a stacja nadaje ciszę.
- Zegar własny działa tylko wtedy, gdy nadawca liczy czas tym samym zegarem, np. BounceAES Master bez PTP (też na zegarze własnym) na komputerze z czasem z NTP, a różnica czasu obu komputerów stale mieści się w tym oknie: nadawca najwyżej o opóźnienie odbioru za Bounce Encoderem i najwyżej 0,5 s przed nim. Nawet wtedy przy długiej pracy mogą się zdarzyć pojedyncze uzupełnienia albo odrzucenia pakietów (widoczne w Statystykach). Do pracy na antenie potrzebny jest PTP.
- Gdy pasek pokazuje „zegar własny”, choć master jest w sieci, sprawdź: domenę PTP (Ustawienia… i grandmaster muszą mieć tę samą), wybraną kartę sieciową AoIP (Ustawienia…: zła karta to brak komunikatów PTP), klienta PTP Windows (w32time, poniżej) i zaporę dla portów UDP 319/320.
- **Klient PTP Windows** (w32time) zająłby porty 319/320. Instalator ostrzega, jeśli jest włączony. Wyłączenie (wiersz poleceń jako administrator):
  `reg add HKLM\SYSTEM\CurrentControlSet\Services\W32Time\TimeProviders\PtpClient /v Enabled /t REG_DWORD /d 0 /f`, potem `net stop w32time && net start w32time`.
- Karta sieciowa AoIP (Menedżer urządzeń → Właściwości → Zaawansowane): *Interrupt Moderation*, *Energy Efficient Ethernet* i *Flow Control* wyłączone; w zarządzaniu energią odznacz „Zezwalaj komputerowi na wyłączanie tego urządzenia”. Plan zasilania: „Wysoka wydajność”.
- Wyłącz Wi-Fi i inne karty w tej samej podsieci co AoIP.

## 12. Diagnostyka

| Objaw | Sprawdź |
|---|---|
| Ikona czerwona, panel się nie otwiera | `services.msc` → Bounce Encoder → Uruchom; log `C:\ProgramData\Bounce Encoder\logs\bounce-encoder.log`; Dziennik zdarzeń (Aplikacja, źródło „Bounce Encoder”) |
| Ikona żółta | panel → Stacje: który strumień nie nadaje albo które wejście nie ma sygnału |
| Wejście „brak sygnału” | czy źródło nadaje; SDP (adres, port); wybrana karta sieciowa AoIP (Ustawienia…); IGMP na przełączniku; zapora. Jeśli w Statystykach rosną **Przepełnienia** (pakiety przychodzą, ale są odrzucane): PTP niezsynchronizowany, więc czas źródła i Bounce Encodera się nie zgadza (rozdział 11): domena PTP, karta AoIP, w32time zajmujący porty 319/320, zapora |
| Wejście z błędem „nieprawidłowy SDP” | SDP musi opisywać L16/L24, 48 kHz, 1–2 kanały, adres multicast; wklej cały plik |
| Wejście z błędem „dołączenie do grupy …” albo „gniazdo odbiorcze: …” | karta sieciowa AoIP (Ustawienia…) jest rozłączona albo ma inny adres; usługa ponawia próbę sama |
| Trzaski, rośnie licznik niedoborów | zwiększ opóźnienie odbioru wejścia; ustawienia karty sieciowej (rozdział 11) |
| PTP „zegar własny”, choć master jest w sieci | domena PTP (ta sama co w grandmasterze), karta AoIP (Ustawienia…), w32time zajmujący porty 319/320 (rozdział 11), zapora (UDP 319/320) |
| Strumień z komunikatem błędu | tabela w rozdziale 9 |
| AAC niedostępny | brak `libfdk-aac-2.dll` w folderze programu: zainstaluj ponownie |
| Rosną „Odrzucone” w strumieniu | serwer albo łącze nie nadąża: przepustowość wysyłania, obciążenie serwera |
| Rosną „Pominięte ramki” stacji | komputer nie nadąża: obciążenie CPU, liczba strumieni HE-AAC, plan zasilania |
| Panel niedostępny z innego komputera | adres panelu (Ustawienia…, po zmianie restart usługi), zapora, czy to ten sam port (8070) |
| Po starcie część stacji jest wyłączona | plik `config.json` był zmieniany ręcznie i miał błędy: usługa wyłączyła błędne stacje (opis w logu), popraw je w panelu. Plik, którego nie dało się wczytać, ma kopię `config.json.bad` |

Uruchomienie usługi w oknie konsoli (do diagnostyki, po zatrzymaniu usługi Windows, w wierszu poleceń jako administrator):
`"C:\Program Files\Bounce Encoder\BounceEncoder.exe" --console`

Windows czeka na zatrzymanie usługi do 10 s. Jeśli przy zatrzymaniu pojawia się błąd przekroczenia czasu, zapisz, ile trwało, i dołącz log (TESTY.md, rozdział 3).

## 13. Znane ograniczenia

- **Panel tylko w zaufanej sieci.** Biblioteka HTTP panelu czyta bardzo długie pierwsze wiersze żądań w całości, zanim sprawdzi logowanie. Komputer z dostępem do portu panelu może w ten sposób zająć dużo pamięci usługi. Ustaw adres panelu na kartę sieci administracyjnej albo `127.0.0.1` (rozdział 4) i nie wystawiaj portu 8070 do internetu (z zewnątrz tylko przez odwrotne proxy z HTTPS i własnym uwierzytelnieniem). Poprawka przyjdzie z aktualizacją biblioteki, wspólną dla BounceAES Master i Bounce Encoder.
- Panel obsługuje jedno żądanie na połączenie (zabezpieczenie przed przemycaniem żądań); dla panelu nie ma to odczuwalnego kosztu.
- Brak TLS do Icecasta, brak metadanych ICY (tytułów), brak processingu i RDS: to robią inne systemy.
- Serwer Icecast: nazwa albo adres IPv4 (bez IPv6).
- Adres i port panelu zmieniają się dopiero po restarcie usługi.
- Brak kodeka AAC (pliku `libfdk-aac-2.dll`) wyłącza tylko strumienie AAC (rozdział 8).

## 14. Licencje kodeków

Bounce Encoder jest na licencji GPL-3.0-or-later. Kodeki MP3 i AAC to osobne biblioteki ładowane w czasie pracy; libogg, libFLAC i libsamplerate są wkompilowane w program:

| Biblioteka | Licencja | Plik |
|---|---|---|
| LAME 3.100 (MP3) | LGPL-2.0-or-later | `libmp3lame.dll` |
| Fraunhofer FDK AAC 2.0.3 (AAC, HE-AAC, HE-AACv2) | licencja Fraunhofer FDK AAC: niezgodna z GPL; AAC jest objęty patentami | `libfdk-aac-2.dll` |
| libogg 1.3.5, libFLAC 1.4.3 | BSD-3-Clause | wkompilowane w `BounceEncoder.exe` |
| libsamplerate 0.2.2 | BSD-2-Clause | wkompilowane |

Licencja FDK AAC nie jest zgodna z GPL, dlatego licencja Bounce Encoder ma dodatkowe uprawnienie (GPLv3, sekcja 7), które pozwala łączyć go z fdk-aac i przekazywać razem (LICENCJE.md). **W Bounce instalatora z AAC używasz bez ograniczeń.** Przed przekazaniem go poza firmę sprawdź licencję patentową AAC (licencja Fraunhofera nie obejmuje patentów) albo przekaż pakiet bez `libfdk-aac-2.dll`: MP3 i FLAC działają dalej. Pełne teksty licencji są w folderze programu (`licencje\`) i w pliku `LICENCJE.md`.

## 15. Aktualizacja, odinstalowanie, podpisywanie

**Aktualizacja:** uruchom nowy instalator. Zatrzyma usługę i ikonę, wymieni pliki i uruchomi usługę ponownie. Konfiguracja zostaje.

**Co nowego w 1.0.4:** poprawki bezpieczeństwa panelu, wspólne z BounceAES Master 1.2.5. Panel odrzuca złośliwie zbudowane zapytania, zanim ktokolwiek się zaloguje: JSON zagnieżdżony głębiej niż 16 poziomów, z więcej niż 10 000 obiektów i tablic, 20 000 wartości albo 2 000 pól w jednym obiekcie dostaje od razu błąd 400 (wcześniej takie zapytanie przed zalogowaniem zajmowało procesor na kilka sekund i dużo pamięci, a po zalogowaniu mogło zatrzymać usługę). Bardzo długie hasła nie obciążają już procesora przy logowaniu. Zapisane hasło, stacje i strumienie zostają bez zmian.

**Co nowego w 1.0.3:** własna ikona z literami **BE** na czarnym kwadracie z różową kropką, żeby nie mylić Bounce Encodera z BounceAES Master (ten ma litery **BA**). Ta sama ikona jest w zasobniku, na skrótach, w instalatorze, na liście aplikacji i w karcie panelu w przeglądarce; plik ikony w folderze programu to teraz `encoder.ico`. W zasobniku stan pokazuje kolorowa kropka w prawym dolnym rogu, z tymi samymi kolorami co wcześniej (rozdział 3). Bez zmian w działaniu.

**Co nowego w 1.0.2:** nowy wygląd panelu, wspólny z BounceAES Master 1.2.3; bez zmian w działaniu. Stan usługi (PTP, wejścia, strumienie, karta sieciowa, czas działania) jest w pasku stanu na dole, nadający strumień ma lampkę **NA ANTENIE**, mierniki są segmentowe, ze skalą dBFS i zapamiętanym szczytem.

**Co nowego w 1.0.1:** blokada logowania działa też przy równoległych próbach: hasła sprawdzane w tej samej chwili liczą się do limitu, więc wiele prób wysłanych naraz nie obejdzie blokady po 5 błędnych hasłach (ani ogólnego limitu 20 błędnych haseł z sieci na minutę).

**Odinstalowanie:** *Ustawienia → Aplikacje → Bounce Encoder → Odinstaluj* albo skrót *Odinstaluj Bounce Encoder* w menu Start. Deinstalator zatrzymuje i usuwa usługę, regułę zapory, źródło Dziennika zdarzeń, autostart, skróty i pliki programu. Na koniec pyta, czy usunąć konfigurację i logi (`C:\ProgramData\Bounce Encoder`); domyślnie (i przy deinstalacji cichej `/S`) zostają.

**Podpisywanie plików:** jak w BounceAES Master (INSTALL.md, rozdział 11): `installer\sign.ps1 -Path <folder>` podpisuje pliki `.exe` i `.dll` (także biblioteki kodeków) i instalator własnym certyfikatem firmy. Nigdy nie używaj certyfikatu ani nazwy innego producenta.

**Budowanie ze źródeł** (README.md, rozdział o kompilacji): konfiguracja 64-bitowego buildu z kodekami pobiera archiwa źródeł LAME, fdk-aac, libogg i libFLAC (sourceforge.net, github.com, xiph.org, sprawdzane po SHA-256), więc potrzebuje dostępu do sieci, chyba że kodeki są wyłączone (`-DBAM_WITH_CODECS=OFF`, wtedy bez Bounce Encodera). `-DBAM_CODEC_CACHE=<folder>` zachowuje archiwa i przy kolejnych konfiguracjach używa ich bez sieci.
