# Transit Providers: one city's live data on the Transit app's map

The Transit app (`apps/transit/`) shows the card's Map Pack with a city's
stops and moving vehicles over it, and a tapped stop's departures. What it
knows of a city is its **Provider**: where the data comes from and how to
read it. Berlin is the first (`apps/transit/transit/providers/berlin.c`);
another city is one new file. The header, `transit/transit_provider.h`, is
the reference; `.scratch/maps/research/berlin-transit-api.md` has why Berlin
uses vbb-rest.

## The interface

A Provider is a `const transit_provider_t`:

| Field | What it is |
|-------|------------|
| `id` | `"berlin"`: what `config/transit.json`'s `provider` names |
| `name` | `"Berlin"`, for the log and to open on |
| `credit` | who the data is from, on the About line under the map's credit |
| `base_url` | where it fetches from, unless `config/transit.json` says |
| `lat`, `lon` | the city's centre: the app opens there when the Pack covers it and the GPS fix is elsewhere |
| `min_interval_s` | the shortest time between fetches it allows, in seconds |
| `state_size` | bytes of its own state kept between fetches, zeroed when the app opens, in PSRAM |
| `fetch` | the one call, below |

```c
bool fetch(void *state, const transit_request_t *request, transit_result_t *out,
           transit_error_t *error);
```

`request` holds the **box** to cover (the view grown by half of itself each
way, so a pan step lands on things already fetched), the **stop** whose
departures to fetch too (`""` for none), the **base URL** to use, and
**now** (UNIX seconds, 0 while the clock is not set). `fetch` fills `out`,
which comes zeroed: up to 64 stops (`id`, `name`, position), 64 vehicles
(`line`, `direction`, position) and 8 departures of the stop asked for
(`when` as `"16:45"` in the city's time, `line`, `direction`, `live` and
`delay_min`), with `departures_for` set to that stop's id. It returns true,
or false with `error->words` for the user ("No network") and the HTTP
status, if any, for the log.

It runs on the app's own task, `transit`, with an 8 KB internal stack: keep
big buffers in `state` or in PSRAM, not on the stack. It may block on the
network; it is never called twice at once, and never while the app is
closed. Its helpers:

- `transit_net_get(url, &len, &error)` (`transit/transit_net.h`): a GET
  following redirects, the body in PSRAM (768 KB at most), `error` filled
  for no network, no answer or a status other than 200. The URL is logged
  without its query, so a key in it stays out of the log.
- The Podcasts app's `pod_json.h`: a JSON reader that walks the answer in
  place and allocates nothing.

What it caches is its own business: Berlin asks for stops again only once
the box has moved a quarter of itself or after 5 minutes, and for the radar
every fetch.

## What the app does with it

`transit_session.c` runs the fetching; the Provider does none of this:

- **Only while the app is open.** Opening it starts the task; leaving
  destroys the app and stops it. A fetch under way finishes, and none
  starts after. There is no background check and no syscall.
- **Held awake** for each fetch (`power_manager_hold`), not between them.
- **When:** at once on opening, then every interval: `min_interval_s`, or
  `config/transit.json`'s, 5 s at least. Refresh (the button, or `r`) and a
  tapped stop fetch at once, but not within 2 s of the last.
- **A failure** keeps what is shown and backs off: the interval, then twice
  it, up to 5 minutes; Refresh tries at once. The status line says why
  ("No network", "Transit service not answering"); vehicles older than
  10 minutes are taken off, the stops stay.
- **On the map**, stops are squares with their names, vehicles triangles
  with their line, each an overlay (`docs/maps/overlay-api.md`). The first
  result and Refresh's are drawn at once; other changes under the 20 s rule,
  only the rectangle that changed.
- **Saved as GeoJSON**, with `"save_geojson": true`: each result is written
  to `/sdcard/maps/overlays/transit-<id>.geojson` (vehicles as triangles,
  stops as squares), which the Maps app shows as an overlay of its own.

The log says, for example:

    (transit) berlin: fetching every 30 s while the app is open
    (transit) berlin: fetched 12 stops, 9 vehicles, 0 departures in 840 ms, for the app opening
    (transit) berlin: fetch failed: HTTP 429, Too many requests to the transit service; trying again in 60 s
    (transit) card: Alexanderplatz; 16:47 M10 S+U Hauptbahnhof +2; 16:52 200 Zoo
    (transit) berlin: stopped fetching

## The card's config

`/sdcard/config/transit.json`, all optional:

```json
{"provider": "berlin", "base_url": "http://homelab:3000",
 "min_interval_s": 30, "save_geojson": false}
```

`provider` picks the city (the first in the list without it); `base_url`
points it at another server speaking its protocol, a self-hosted vbb-rest
on the tailnet say. A Provider that needs a key reads it from its own
fields here, as the Podcasts app reads `config/podcasts.json`, or from a
header generated from `.env` in the build directory: never the source tree
(CLAUDE.md).

## Adding a city

1. Write `apps/transit/transit/providers/<city>.c` defining
   `const transit_provider_t transit_provider_<city>`, with its `fetch`.
   `providers/berlin.c` is the model. The build finds the file by itself
   (`apps/transit/sources.cmake` globs the folder).
2. Add its name to `TRANSIT_PROVIDERS` in `transit/transit_provider.h`:
   `X(berlin) X(<city>)`.
3. Put `{"provider": "<city>"}` in `config/transit.json`.
4. Copy a case of `simulator/tests/test_transit.py` with a fake of its
   server, as `fake_transit_provider.py` fakes vbb-rest.

Name its data's source in `credit`, as its licence asks: Berlin's is
"Live data: VBB, via v6.vbb.transport.rest".
