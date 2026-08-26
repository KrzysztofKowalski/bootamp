# bootamp — terminal music player

C++23 + SIMD rewrite of [cliamp](https://github.com/KrzysztofKowalski/cliamp) (Go + Bubbletea terminal music player): fast, tiny CPU/memory footprint.

On an old Intel MacBook, bootamp uses ~4% CPU where the Go original (cliamp) used 40%+.

## Features

- **Local files** — FLAC, MP3, OGG, WAV and more via native decoders (libsndfile, libFLAC, libvorbis) with an FFmpeg fallback for other formats
- **Internet radio** — HTTP(S) streams with ICY title metadata and live-stream underrun resilience
- **YouTube / YouTube Music / SoundCloud / Bilibili / Bandcamp** via yt-dlp
- **Gapless playback** — the next track is preloaded and swapped in with zero gap
- **10-band EQ** with 16 built-in presets
- **WSOLA time-stretching** — speed control from 0.25x to 2.0x without pitch change
- **Volume / mono** controls
- **31 visualizer modes** with fullscreen mode
- **Radio browser** with catalog search, YouTube search and favorites
- Playlist management: shuffle, repeat, queue, bookmarks, M3U/PLS playlists

## Dependencies

C++23 compiler (GCC 16+), CMake, miniaudio, FFTW3, FFmpeg (libswresample + the `ffmpeg` binary), libsndfile, libvorbis, opus, flac, TagLib, spdlog, fmt, nlohmann-json, tomlplusplus, CLI11, curl, yt-dlp (runtime), FTXUI (AUR on Arch). OpenSSL is required for HTTPS radio streams.

### Arch Linux

```sh
sudo pacman -S --needed base-devel cmake pkgconf miniaudio fftw yt-dlp ffmpeg nlohmann-json tomlplusplus libsndfile libvorbis opus flac spdlog fmt catch2 cli11 curl taglib
paru -S ftxui  # AUR
```

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Or use `./build.sh` (installs missing deps, supports `--tsan`, `--debug`, `--clean`).

## Usage

```sh
./build/bootamp <file|url|radio>
```

Examples:

```sh
./build/bootamp ~/Music/track.flac
./build/bootamp https://radio.cliamp.stream/lofi/stream
./build/bootamp https://www.youtube.com/watch?v=...
```

With no arguments the default radio station list is queued. `bootamp play <file>` and `bootamp search <query>` shorthands are supported; see `bootamp --help` for all flags (`--vol`, `--speed`, `--eq`, `--vis`, `--shuffle`, `--repeat`, `--mono`, ...).

## Keybindings

Global:

| Key | Action |
|---|---|
| `Space` | Play / pause |
| `←` / `→` | Seek ±5s |
| `Shift+←` / `Shift+→` | Seek ±large step |
| `↑` / `↓` (or `+` / `-`) | Volume up / down |
| `n` (or `>`, `.`) | Next track |
| `p` (or `<`, `,`) | Previous track |
| `r` | Cycle repeat |
| `s` (or `z`) | Toggle shuffle |
| `f` | Toggle bookmark/favorite |
| `v` | Next visualizer |
| `V` | Fullscreen visualizer |
| `l` / `Tab` | Queue screen |
| `R` (or `b`) | Radio browser |
| `e` | EQ overlay |
| `?` (or `h`, `Ctrl+K`) | Help |
| `d` | Audio device picker |
| `Esc` | Back to visualizer |
| `q` / `Ctrl+C` | Quit |

Screen keys (when a screen is open): `↑`/`↓` navigate, `Enter` play/select, `Esc` close. Queue: `Shift+↑`/`Shift+↓` move track, `d` remove, `c` clear, `s` shuffle, `r` repeat. Browse: `/` catalog search, `Ctrl+F` YouTube search, `f` favorite, `r` refresh. EQ: `←`/`→` adjust band, `0` reset band, `e` close.

## License

MIT — see [LICENSE](LICENSE).
