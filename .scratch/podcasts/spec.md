# Podcasts

Status: ready-for-agent

## Problem Statement

A ThistleOS device with WiFi or LTE, a speaker and an SD card has
everything a podcast player needs, and no app that plays one. Nothing on the
device can find a podcast, stream an episode, or keep one to play later where
there is no signal.

## Solution

A Podcasts app, built on the PodcastIndex API
(https://podcastindex-org.github.io/docs-api/), with the sections a phone's
podcast app has:

- **Search**, by term, title, person or music, with an option to leave out
  explicit shows.
- **Trending**, and **Categories**, each category's trending shows.
- **New episodes** from the starred podcasts.
- **Starred**: podcasts and episodes.
- **Saved**: episodes downloaded to the SD card, which play with no network.
- **Recently viewed**: podcasts and episodes opened.
- **Continue listening**: episodes played, newest first, each resuming where
  it was left.

A podcast's page shows what it is and its episodes, newest first, and stars
it. An episode's page shows what it is, plays it (or resumes it), stars it,
and saves it to the SD card or deletes it from there.

**Now playing** shows the episode, a bar that seeks, the time gone and left,
play/pause, back 15 s and forward 30 s, the volume, star and save. Playback
goes on after leaving the app; the home page's first row goes back to it, and
the quick panel gets a Podcast switch that pauses and resumes it from any
app.

## User Stories

1. As a listener, I want to search PodcastIndex by name, title or person, so
   that I can find a show I know of.
2. As a listener, I want to browse what is trending, overall and by category,
   so that I can find something new.
3. As a listener, I want to open a podcast and see its episodes, newest
   first, with how long each is and whether I have played, starred or saved
   it.
4. As a listener, I want to tap Play and hear an episode within seconds,
   streamed, without waiting for it to download.
5. As a listener, I want to pause, seek, skip back 15 s and forward 30 s, and
   change the volume, from obvious buttons and from the keyboard.
6. As a listener, I want an episode to resume where I left it, even after a
   reboot.
7. As a listener, I want to save an episode to the SD card and play it later
   with no network.
8. As a listener, I want to star podcasts and episodes and find them again
   in one place.
9. As a listener, I want to see the podcasts and episodes I opened last.
10. As a listener, I want the newest episodes of the podcasts I starred in one
    list.
11. As a listener, I want playback to go on when I open another app, and to
    pause it without going back to Podcasts.
12. As a developer, I want the API key and secret kept out of the repository.

## Implementation Decisions

### Where it lives

A built-in LVGL app, `apps/podcasts/`, ID `com.thistle.podcasts`, registered
like the Reader. Built-in rather than an `.app.elf`, because it needs real
LVGL (lists, a slider, text input) and its own threads.

### Credentials

PodcastIndex signs each request with
`Authorization: sha1(key + secret + X-Auth-Date)`. The key and secret come
from, first, `/sdcard/config/podcasts.json`
(`{"api_key": "…", "api_secret": "…"}`), which a setup page in the app
writes, then from a header CMake generates in the *build* directory from the
repository's git-ignored `.env` (`PODCASTINDEX_API_KEY`,
`PODCASTINDEX_SECRET`), when there is one. Nothing with a key in it is ever
written inside the source tree: the simulator's SD card is a directory in the
repository, so its `config/podcasts.json` is git-ignored as well.

The date header must be within 3 minutes of PodcastIndex's clock, so with
the clock not set (`time_manager_get_status().known`) the app says so rather
than sending requests that will be refused.

### Threads and memory

CLAUDE.md's rules on internal RAM shape this:

- **API requests** run one at a time on a thread of their own (8 KB stack),
  the Assistant's way: the job has an atomic state, the UI polls it from an
  `lv_timer`, and abandons it when the page goes. Answers go to PSRAM and are
  parsed in place by `pod_json.c`, a reader that allocates nothing, since a
  tree of the answer would be hundreds of small allocations in internal RAM.
- **The player** has two threads while an episode is loaded: one that fetches
  (HTTP, or the file on the SD card) into a 512 KB ring in PSRAM, and one that
  decodes with minimp3 and writes PCM to the audio HAL. minimp3's 16 KB
  scratch is in PSRAM, so the decoder's stack is small. Both end when nothing
  is loaded, and after 10 minutes paused.
- **Downloads** run one at a time on a thread of their own, a queue of up to
  16, to `<episode>.mp3.part` beside where the file goes, resumed with a
  Range request if cut short, and renamed when whole.
- Each holds the device awake (`power_manager_hold`) while it works: the
  player while playing or buffering, not while paused.

### The player

- **Source**: the saved file when there is one, else the enclosure URL. The
  stream follows redirects itself (ESP-IDF's open/read API does not), asks
  for a byte range to start or seek mid-file, and on a dropped connection
  reopens where it was, a few times, before giving up.
- **Seeking** goes to the byte that far through the audio, by the episode's
  length and duration, after skipping any ID3v2 tag (podcast tags often hold
  hundreds of KB of artwork).
- **Underruns**: the audio driver closes the codec after 500 ms without
  data, which pops; while the ring is empty the decoder writes silence, so a
  stall is a gap, not a pop.
- **Format**: MP3 only (minimp3; CC0). AAC needs a decoder with a licence
  this project can take, and none is vendored; an episode in another format
  says so instead of playing.
- **Position** is saved to the library every 30 s while playing, on pause and
  at the end; an episode played to within 30 s of its end counts as finished.

### The library

`/sdcard/data/podcasts/library.json`: the podcasts and episodes the app
remembers, each with what the lists show (title, author, enclosure, length,
duration, a short description) and when it was starred, viewed, played and
saved, with the position. A podcast is kept while starred or among the 20
viewed last; an episode while starred, saved, among the 20 viewed last or
the 30 played last. It is written whole, to a `.tmp` file renamed over the
old one (FAT does not rename over a file, so the old one goes first; a load
that finds only the `.tmp` takes it).

Saved episodes are `/sdcard/podcasts/<feed id>/<episode id>.mp3`.

### The UI

Pages on a stack, only the top one built: going back rebuilds the page below
from what it had fetched, with the same row selected. Lists are one
`lv_table` each, as in the Reader, a row costing one string of LVGL's 64 KB
pool. Text is in the Reader's DejaVu font (Latin, Latin Extended-A,
punctuation, Arabic shaped by `arabic_shape.c`), its 1-bit cut on e-paper,
falling back to a ★/☆ font of the app's own and then Montserrat for LVGL's
symbols.

On e-paper, Now playing changes its time labels every 10 s rather than every
second, since each change is a refresh.

Back (the T-Deck Max's key, `ui_manager_set_back_handler`), the title bar
and `q` go up a page.

## Testing Decisions

- Host tests under sanitizers (`apps/podcasts/tests/run_tests.sh`, in CI):
  SHA-1 and the auth header; the JSON reader; parsing real PodcastIndex
  answers into feeds and episodes; HTML to plain text, durations and dates;
  the library's lists, pruning, and saving and loading, including a save cut
  short.
- The simulator: the streaming HTTP shim (now real streaming); every app on
  every device profile (`test_app_matrix.py`); `test_podcasts.py` against a
  local fake PodcastIndex: home, search, a podcast, an episode, play, save to
  the card, and play the saved episode with the network gone.
- Hardware, on the T-Deck Max: playback through the speaker, streaming over
  WiFi, a saved episode with WiFi off.

## Out of Scope

- AAC/M4A episodes, video podcasts, live items.
- Artwork.
- Playback speed.
- Playing the next episode on its own.
- Subscriptions that download new episodes in the background.
- thistle-tk.
