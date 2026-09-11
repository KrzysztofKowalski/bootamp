# Gieres Radio Radio — API archiwum (orgonity / echelon / live) dla bootampa

Dokumentacja endpointów serwera "Radio Radio" (Rails, `gieres-www`, trzyma na
nim Giereś swoje archiwum audycji). Dla agenta budującego przeglądarkę
archiwum w bootampie. Wszystko lokalnie, bez autoryzacji:

**Base URL:** `http://192.168.1.154:13080` (LAN, Mac mini; zalecany — testowany)
**Base URL (publiczny):** `https://gieres.cytr.us` (ten sam Rails przez HTTP/2 —
działa też, ale po LAN jest szybciej; tylko audio `audio_url` bywa na nim droższe).

**Wybór endpointa w bootampie (kolejność):** `~/.config/bootamp/gieres.ini`
(`base_url = …`; przykładowy plik: `scripts/gieres.ini.test`) > klucz
`gieres_base_url` w bootamp.toml > default LAN. Na wierzchu tego działa
sticky fallback: błąd transportowy (dial/connect/timeout — nie HTTP 4xx/5xx)
przy próbie na wybranym endpoincie próbuje raz `https://gieres.cytr.us`
i zapamiętuje zwycięzcę na czas sesji (także ctrl+r; stopka ekranu pokazuje
endpoint, który faktycznie odpowiedział).

Źródło prawdy: `/Users/k/projekty/gieres-www` na Mac mini (plik
`docs/orgonity-api.md` tamtego repo + `config/routes.rb`). Notatki na Mac mini
w `~/projekty/radia/gieres` (symlink → `../gieres-www`).

---

## 1. Orgonity — lista wszystkich nagrań (paginacja, JSON)

```
GET /orgonity.json?page=1&q=...&sort=date
```

| Param   | Wartości                                   | Domyślnie |
|---------|--------------------------------------------|-----------|
| `page`  | 1..`total_pages` (50 nagrań na stronę)     | 1         |
| `q`     | szukany ciąg w tytule (ASCII only, patrz niżej) | —    |
| `sort`  | `date` \| `title` \| `duration`            | `date`    |

Odpowiedź (JSON):

```json
{
  "page": 1,
  "total_pages": 34,
  "total_count": 1688,
  "audio_count": 1688,
  "all_count": 1691,
  "recording_dates": ["2010-04-13", "2010-05-11", "..."],
  "broadcasts": [
    {
      "id": 1900,
      "external_id": "ooOquVcjqg7Q",
      "title": "Teoria Chaosu Live. 2010.04.13. Wprowadzenie",
      "recording_date": "2010-04-13",
      "aired_at": "2010-04-12T22:00:00Z",
      "duration_seconds": 3584,
      "has_audio": true,
      "audio_url": "/broadcasts/1900/audio.mp3"
    }
  ]
}
```

Uwagi (zweryfikowane curl-em 2026-09-11):
- `recording_dates` — wszystkie dni z nagraniami (posortowane rosnąco). Gotowa
  baza na tekstowy "kalendarz"/indeks dat: nawigacja dzień wcześniej/później =
  poprzedni/następny element tej listy. Pole jest pełne na dowolnej stronie.
- `audio_count` / `all_count` — nagrania z audio / wszystkie (część bez pliku,
  patrz `has_audio`); filtrować po `has_audio`.
- Sortowanie po `date` używa `recording_date` (data z TYTUŁU, parsowana
  `\b20\d{2}\.\d{2}\.\d{2}\b`), a gdy brak daty w tytule — data uploadu
  (`aired_at`). `recording_date` w JSON jest zawsze `YYYY-MM-DD`.
- Sanityzacja: `q` jest czyszczone do `[A-Za-z0-9 _-]`, `sort` whitelistowany.
  Nie trzeba escapingować, ale nie licz na znaki spoza ASCII.

## 2. Orgonity — nagrania z danego dnia / miesiąca / roku (JSON)

```
GET /orgonity/by_date/:date.json?q=...&sort=date
```

| Format       | Znaczenie        | Przykład                       |
|--------------|------------------|--------------------------------|
| `YYYY`       | cały rok         | `/orgonity/by_date/2021.json`  |
| `YYYY-MM`    | cały miesiąc     | `/orgonity/by_date/2021-11.json` |
| `YYYY-MM-DD` | konkretny dzień  | `/orgonity/by_date/2021-11-19.json` |

Odpowiedź: `{"date": "2021-11-19", "count": 3, "broadcasts": [ ...jak wyżej... ]}`.
Błędna data → `400` `{"error": "Nieprawidłowa data"}`.

## 3. Audio (strumień pliku, VOD)

```
GET /broadcasts/:id/audio.mp3
GET /broadcasts/:id/audio          (bez legacy rozszerzenia, to samo)
```

- Mimo rozszerzenia `.mp3` treść to **AAC/M4A** (`Content-Type: audio/mp4`),
  rozmiar np. ~408 MB dla nagrania 3h.
- Obsługuje HTTP **Range** (`206 Partial Content`, `accept-ranges: bytes`) —
  seek po przesunięciu bajtowym działa; `Cache-Control: max-age=3600, public`.
- Nagranie bez pliku (`has_audio: false`) → `404`.
- To NIE jest strumień ICY — klasyfikacja w `audio/radio_pipeline.hpp`
  (live = nagłówki `icy-*`) zakwalifikuje to jako zwykły plik HTTP.

## 4. Echelon — segmenty oś czasu (JSON)

```
GET /echelon/segments?start=<ISO8601 UTC>[&end=<ISO8601 UTC>]
```

- `start` bez `end` → segmenty od startu w przód (okno strony echelon).
  Z `end` → tylko w oknie. Zweryfikowane: `start=2026-08-30T20:00:00Z&end=…21:00Z`
  zwraca 3 segmenty (20:00, 20:30, 21:00).
- Segmenty to **30-minutowe plasterki** ciągłej os czasu live ("Muzyka w Radiu").

```json
{"segments":[{"id":280905,"title":"Muzyka w Radiu","aired_at":"2026-08-30T20:00:00Z",
  "duration":1803,"url":"/broadcasts/280905/audio.mp3",
  "tracks":[{"title":"Rockett Radio — Most Racist Song Ever (Parody)","position":0},
            {"title":"Thievery Corporation — 33 Degree","position":180}]}]}
```

- `position` w `tracks[]` — sekundy od początku segmentu (spis utworów).
- `duration` w sekundach; `url` = audio jak w sekcji 3 (Range działa, więc
  cięcie między segmentami można robić seekiem: koniec segmentu = kolejny).

## 5. Now-playing + eksport fragmentu

```
GET /now-playing.json      → {"title":"Radio Radio ..::.. Pink Floyd \"Hey You\""}
GET /echelon/export?broadcast_id=<id>&start=mm:ss&end=mm:ss&fmt=mp4|webm[&name=nazwa]
```

- Export zwraca blob MP4/WebM fragmentu (ffmpeg po stronie serwera, limit 3
  równoległych, `Retry-After` przy 429). Działa też dla orgonity. Bootamp w MVP
  **nie używa** exportu — po prostu streamuje audio przez Range.

## 6. Live stream (radio na żywo)

```
https://c16.radioboss.fm:18014/stream   (HTTPS, audio/mpeg)  — preferowany
http://c16.radioboss.fm:8014/stream     (HTTP)              — fallback
```

- Zewnętrzny RadioBoss (nie na Mac mini). ICY Icecast-compatible → istniejący
  pipeline radiowy bootampa (`audio/radio_pipeline.hpp`, `audio/icy.hpp`) gra
  to bez zmian. Playlisty: `https://c16.radioboss.fm/playlist/14/stream.m3u`
  (zawiera `https://c16.radioboss.fm:8014/stream`).
- `crossorigin="anonymous"` na stronie; `audio/mpeg` (MP3), nie m4a.

## 7. Strony HTML (kontekst, nie używamy do parsowania — są JSON-y)

- `GET /orgonity?page=N&q=&sort=&date=&id=&t=` — pełne UI (lista w `#broadcasts-list`,
  wiersze mają `data-broadcast-id` / `data-audio-url` / `data-title` /
  `data-recording-date` / `data-duration`).
- `GET /echelon?s=<ISO>&t=<sek>` — oś czasu; `s` = start, `t` = pozycja w sek.
- `GET /ramowka`, `/radio`, `/aktualnosci`, `/linki`, `/teoriachaosu`,
  `GET /teoriachaosu/upcoming` (JSON nadchodzących audycji live).

### Pułapki

- `aired_at` to data uploadu (UTC, północ warszawska), NIE data nagrania —
  data nagrania to `recording_date` (z tytułu); w echelon `aired_at` jest
  dokładnym czasem emisji segmentu.
- `q` po stronie serwera jest sanityzowane — znaki spoza `[A-Za-z0-9 _-]` są
  po cichu usuwane (`q=Most#2` wyszuka `Most2`).
- `[...]` w nazwach plików to `external_id` (BitChute) — w API posługujemy się
  `id` (DB) i `audio_url` z JSON-a.

---

## 8. Plan integracji w bootampie (MVP)

1. **Klient HTTP+JSON**: mały klient na `audio/http_socket.hpp` (GET, odczyt
   całej odpowiedzi JSON; audio przez istniejący pipeline). Parsowanie JSON —
   minimalny parser (tylko pola z sekcji 1/2/4) w stylu istniejących parserów.
2. **Karta przeglądarki** (`ui/screens/gieres.*`): trzy widoki w jednej
   zakładce — LIVE (live stream jak stacja radiowa), ORGONITY (lista dat
   z `recording_dates` → lista nagrań dnia → Enter gra `audio_url`), ECHELON
   (wybór dnia → segmenty 30-min → graj segment; next/prev segment).
3. **Konfiguracja**: base URL w configu (domyślnie LAN `http://192.168.1.154:13080`),
   nadpisywany w `~/.config/bootamp/…`; live URL stały z sekcji 6.
4. **Odtwarzanie**: `audio_url` leci przez istniejący non-ICY rozgałęzienie
   `radio_pipeline` (Range → ffmpeg); live przez istniejący ICY pipeline.