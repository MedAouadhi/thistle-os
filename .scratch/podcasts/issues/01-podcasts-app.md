# 01: The Podcasts app

**What to build:** The app in `spec.md`: PodcastIndex search, trending and categories; a podcast's page and an episode's; streaming MP3 playback that goes on after the app is left; saving to the SD card and playing saved episodes with no network; Starred, Recently viewed, Continue listening and New episodes; the key and secret from `.env` or the card, never the source tree.

**Blocked by:** none.

**Status:** needs-hardware-check

- [x] `pod_sha1.c`, `pod_json.c`, `pod_text.c`, `pod_api.c`, `pod_library.c`, with host tests (`apps/podcasts/tests/run_tests.sh`, in CI)
- [x] `pod_cred.c` and `credentials.cmake`: `.env` into the build directory; the card's `config/podcasts.json`, with an optional `api_url`
- [x] `pod_net.c`: signed API requests; streams that follow redirects and ask for a byte range
- [x] `pod_player.c`: fetch and decode threads, a 512 KB ring in PSRAM, minimp3 with its scratch in PSRAM, seeking, reconnecting, parking after 10 minutes paused
- [x] `pod_download.c`: a queue, `.part` files resumed with a Range request, the library told
- [x] The pages (`podcasts_ui.c`, `pod_ui_player.c`), the quick panel's Podcast switch, the Back key
- [x] Registered for the firmware, both simulators and the launcher; permissions in `main.c`
- [x] Simulator: the HTTP shim streams for real and follows redirects as ESP-IDF does; the audio driver takes as long as the samples would; `test_podcasts.py` (6 cases, in CI)
- [x] Firmware builds
- [x] On the T-Deck Max: streaming over WiFi, through the speaker; a saved episode with WiFi off
- [ ] On the T-Deck Max: the decoder's and fetcher's stack high-water marks (logged when each ends)

## Comments

### Done

- **Host tests**: SHA-1 (FIPS vectors and the API's documented example), the JSON reader (escapes, surrogates, cuts between characters, malformed input), HTML to text, durations and dates, the API's paths and answers (shaped like real ones, ids past 2^32), the library's lists, pruning, file round trip and a save cut short. All under ASan and UBSan.
- **Simulator**: `test_podcasts.py` runs the app against a fake PodcastIndex and podcast host serving a generated MP3 (silent Layer III frames behind an ID3 tag) through a redirect: browse and play, signed requests, save to the card byte for byte and play it with the server gone, search, star, skip ahead (a Range request at the right byte), and play on to the end after the app is left. `test_app_matrix.py` opens it on every device profile; the launcher's drawer hashes moved for its icon. The same flows ran clean under an ASan/UBSan build of the simulator.
- **Live**: in the simulator, against the real PodcastIndex with the `.env` key: Trending, search ("radiolab": 40 podcasts), a podcast, and a real episode streamed through its host's redirects at 48 kHz stereo.
- **Firmware**: builds; the app adds 55 KB of flash and 269 bytes of internal `.bss` (its state is in PSRAM). The image's `ota_0` size warning was there before it.

### Seen on the T-Deck Max

Flashed and tried by hand: the app works. The console was not captured while it ran, so the player threads' stack high-water marks are still to read.

Its lists had no line between entries, nor had the Reader's: LVGL draws a table cell's bottom border a pixel down, where the next row's background covers it. `theme_rule_table_rows()` (`components/ui/src/theme.c`) gives each row a top border as well, which is drawn after the row above, and drops it from the row against the title bar. The lists made of row objects (Settings, Files, Messenger, Notes, WiFi Scanner, App Store) already ruled each row.

### Seen in the simulator

On the tdeck-max (e-paper, 240 x 320) and the tdeck (LCD, 320 x 240): every page fits; the Now playing page's times move every 10 s on e-paper and every second on LCD.

### Known limits

- MP3 only: AAC episodes are marked "not MP3" and do not play.
- Titles in scripts the fonts lack (CJK) show as boxes, as in the Reader.
- The browser simulator (WASM) has not linked since the quick panel came in: `manager.c` calls `quick_panel_*`, and `wasm/CMakeLists.txt` does not build `quick_panel.c`, `quick_panel_tiles.c` or `pull_gesture.c`. The app's switch adds one more call to the same.
