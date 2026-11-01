# Licencjonowanie BounceAES Master

BounceAES Master jest udostępniany na licencji **GPL-3.0-or-later** (plik `LICENSE`). Ta
licencja obowiązuje każdego odbiorcę i się nie zmienia.

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
