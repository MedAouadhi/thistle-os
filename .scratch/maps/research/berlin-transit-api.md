# The Berlin transit API: findings and recommendation

Ticket 07 of `maps`. Read and probed on 2026-11-10, from the primary sources
listed under each heading; the sizes and headers are from requests made that
day (Berlin-Mitte, about 16:40 local time, a weekday afternoon).

## Recommendation

**The Berlin Provider uses `https://v6.vbb.transport.rest`** (vbb-rest 6.0.7,
a REST front for VBB's own app API): no key, 100 requests a minute, and
stops near a point, departures for a stop and vehicle positions in a
bounding box, each as JSON with latitude and longitude. The base URL comes
from the card's `config/transit.json` (`base_url`), so the same Provider can
be pointed at a self-hosted copy (`derhuerst/vbb-rest:6`, Docker) on the
homelab, over the tailnet, when the public one is down, limits us, or should
not see our position.

- **Calls, per refresh of an open Transit app:** one `GET /radar` for the
  map's bounding box, and one `GET /stops/:id/departures` while a stop's card
  is open. Two requests every 30 s is 4 a minute, far inside 100.
- **Minimum interval 30 s, not 20 s.** The server marks `/radar` and
  `/stops/:id/departures` `max-age: 30` (and `/locations/nearby` 300), and
  sits behind a cache (`X-Cache`); asking sooner can only return the same
  data. The spec's 20 s is a ceiling on panel redraws; the Provider's
  minimum interval should be 30 s. Refresh forces a fetch at once.
- **Stops and lines come from the Pack** (ticket 06), not from the API. The
  API is for what moves. `/locations/nearby` is there for a stop the Pack
  lacks, and is cacheable for 5 minutes.
- **VBB's official HAFAS ReST API is the second choice**, for when the
  community service goes away: it needs an access ID from VBB (below) and
  answers in another format, so it would be a second Provider, not a
  fallback inside this one.
- **VBB's GTFS-RT feed is not for the device**: 2.5 MB gzipped, 11 MB raw,
  for the whole network at every fetch, TripUpdates only (no vehicle
  positions), 7 requests a minute shared, and missing some data since
  2026-06-04. It suits a server that slices it, not an ESP32.

### Limits

| | v6.vbb.transport.rest | VBB HAFAS ReST (official) | VBB GTFS-RT |
|---|---|---|---|
| Key | none | `accessId`, on request | none |
| Rate limit | 100 requests/minute | none published; VBB may disable an ID that loads the system | 7 requests/minute (from 2026-11-02) |
| Vehicle positions | `/radar` (bbox) | `journeypos` (bbox) | none in the feed |
| Departures | `/stops/:id/departures` | `departureBoard` | TripUpdates, by GTFS trip |
| Stops near a point | `/locations/nearby` | `location.nearbystops` | static GTFS only |
| Format | JSON | XML or JSON (`format=json`) | protobuf, whole network |
| Data licence | not stated (VBB's data) | VBB's usage rules: name VBB as the source | CC BY 4.0 |
| Guarantee | none stated | none ("Ein Anspruch auf die Nutzung der API besteht nicht") | none |

The 100 a minute is per client address, as far as the page says. Over
mobile data the device shares the carrier's NAT address with strangers, so
a 429 is possible there through no fault of ours; the Provider backs off as
below.

### When it is unavailable

A failed fetch (no network, DNS, TLS, timeout, 429, 5xx, JSON that does not
parse) never clears the map:

1. **Keep the last result on screen, marked stale with its age** ("Live data
   3 min old"), from `realtimeDataUpdatedAt` (a UNIX time in every
   departures and radar answer) or the fetch time. After 10 minutes, take
   the vehicles off: a bus where it was ten minutes ago misleads. The
   stops and lines from the Pack stay; they need no network.
2. **Back off**: the next try after 30 s, then 60, 120, 240, capped at
   5 minutes, and at `Retry-After` when a 429 or 503 gives one. Refresh tries
   at once and resets the back-off on success.
3. **Say why once**, as Hermes does: "No network", "Transit service not
   answering", "Too many requests, trying again in 2 min". In the log,
   `transit berlin: fetch failed: HTTP 429` with the next try's delay.
4. **Departures without realtime still answer**: the API returns
   `plannedWhen` with `when` null or equal and `delay` null. Show the planned
   time, unmarked as live.
5. **Switch the base URL to a self-hosted vbb-rest** in `config/transit.json`
   if the public service is gone for good; the protocol is the same.

### Keys

The recommended API needs none, so nothing is compiled in and nothing is on
the card for it.

If the official VBB API is ever added, its access ID follows the Podcasts
key's rules: `VBB_ACCESS_ID` in the repository's `.env` (git-ignored), read by
a `credentials.cmake` like `apps/podcasts/credentials.cmake` into a header in
the *build* directory, never the source tree; the card's
`config/transit.json` (`access_id`) takes its place, and the simulator's
copy of that file is git-ignored. It is never logged; the request URL that
carries it (`?accessId=...`) is logged without its query. Release builds have
no `.env`, so a released firmware carries no ID and the user puts theirs on
the card. VBB issues an ID per project and may disable one that loads the
system, so one ID must not be shared beyond the user's own devices.

## Findings

### v6.vbb.transport.rest (community, recommended)

Sources: <https://v6.vbb.transport.rest/>,
<https://v6.vbb.transport.rest/api.html>,
<https://v6.vbb.transport.rest/getting-started.html>,
<https://github.com/derhuerst/vbb-rest> (branch 6, ISC),
<https://github.com/public-transport/hafas-client/blob/6/p/vbb/base.json>.

- **What it is.** vbb-rest, Jannis Redmann's (`derhuerst`) open-source
  server around `hafas-client@6`. Upstream is "the API for VBB's mobile
  app": `https://fahrinfo.vbb.de/bin/mgate.exe`, as the client
  `hafas-vbb-webapp` ("VBB WebApp"). It "returns whatever data the VBB app
  shows, including realtime delays and disruptions", local traffic and some
  long-distance trains. The page names no operator, no terms of use and no
  data licence; it is not VBB's service and promises nothing. A 2018
  outage of its v2 is on record in the project's issues.
- **Access.** "You can just use the API without authentication. There's a
  rate limit of 100 requests/minute set up." CORS is on; ETag and
  Cache-Control are sent. HTTPS, a Let's Encrypt certificate, so ESP-IDF's
  bundle verifies it; the device's clock must be set first, as for Podcasts.
  `hafas-client` asks every user for a User-Agent naming the project; send
  `ThistleOS/<version> (+https://github.com/MedAouadhi/thistle-os)`.
- **Stops near a point.** `GET /locations/nearby?latitude=&longitude=&results=8&distance=`
  returns stops: `id` ("900100003"), `name`, `location.latitude/longitude`,
  `products` (suburban, subway, tram, bus, ferry, express, regional as
  booleans), `ids.ifopt` and `stationDHID` ("de:11000:900100003"), and
  `distance` in metres. `Cache-Control: max-age: 300`.
- **Departures.** `GET /stops/:id/departures?duration=20&results=8&remarks=false&pretty=false`
  returns `{departures: [...], realtimeDataUpdatedAt}`. Each departure:
  `tripId`, `stop` (the exact stop point, which may be a sibling of the one
  asked for: Alexanderplatz's 900100003 answered with bus stop 900100031),
  `when`, `plannedWhen`, `delay` (seconds), `platform`, `plannedPlatform`,
  `prognosisType`, `direction`, `line` (`name` "200", `product` "bus",
  `mode`, `operator`), `destination`, `remarks`. A `Link: rel=next` header
  gives the next page. `max-age: 30`. Eight departures were 11 KB raw, 1.4 KB
  gzipped.
- **Vehicle positions.** `GET /radar?north=&west=&south=&east=&results=&frames=&duration=&polylines=false&pretty=false`
  returns `{movements: [...], realtimeDataUpdatedAt}`. Each movement:
  `tripId`, `direction`, `line` (with `color.fg`/`color.bg` for U-Bahn and
  S-Bahn), `location.latitude/longitude`, `nextStopovers` (each with stop,
  planned and live departure, delay, platform), and `frames` (interpolated
  positions over `duration`). `results` defaults to 256, the Overlay cap.
  `max-age: 30`. Each vehicle is about 4.8 KB of JSON, most of it
  `nextStopovers`, which no parameter leaves out: 15 vehicles in a
  1 km by 0.9 km box were 72 KB raw, 4.1 KB gzipped; 34 in 1.4 km by 2.2 km
  were 163 KB raw, 8.3 KB gzipped. `frames=0` saves nothing.
- **Stop identifiers.** HAFAS ids ("900100003") are the last part of the
  DHID/IFOPT ("de:11000:900100003") that VBB's GTFS and vbb-stations use
  ("de:11000:900017101::1" for a stop point of a station). Ticket 06 should
  keep the HAFAS id, or the DHID, for each baked stop, so a tap on a baked
  stop can ask for its departures.

### VBB's own API (HAFAS ReST, key needed)

Sources: <https://unternehmen.vbb.de/digitale-services/api/>,
<https://unternehmen.vbb.de/digitale-services/api/zugang-produktivsystem/>
(the usage rules), <https://fahrinfo.vbb.de/restproxy/2.32/> (service list,
each with a WADL). VBB's PDF of the interface
(`vbb.de/fileadmin/.../hafas-api-dokumentation.pdf`) answers 404 now.

- **Access.** "Senden Sie Ihre Anfrage und eine kurze Projektbeschreibung an
  api@vbb.de": a reply with access to a test server (higher latency,
  altered timetable data), then, after agreeing to the usage rules by a form,
  the production system. Every request carries `accessId`; without it the
  production server answers 400 `Missing value for required param accessId`.
  No cost is mentioned.
- **Usage rules** (the form's four conditions): no warranty for the data's
  correctness; no right to the API and no warranty against downtime;
  "Anfragen an die API sind effizient zu stellen und unnötige Last zu
  vermeiden" or VBB may disable the ID for a while; and VBB is named as the
  source of the timetable data "an geeigneter Stelle, etwa in den AGB oder im
  Impressum" (for us, the Transit app's About line). No numeric rate limit
  is published.
- **Services** at `https://fahrinfo.vbb.de/restproxy/2.32/` (server 2.32.4):
  `location.nearbystops` (`originCoordLat`, `originCoordLong`, `r` metres,
  `maxNo`), `departureBoard` (`id` or `extId`, `duration`, `maxJourneys`,
  `products`, `lines`), `journeypos` (`llLat`, `llLon`, `urLat`, `urLon`,
  `maxJny`, `products`, `positionMode`), `arrivalBoard`, `journeyDetail`,
  `trip`, `himsearch`, `location.name`, `gisroute`, `recon`. `format=json`
  gives JSON. Field names of the answers were not read: that needs an ID.

### VBB's GTFS-RT feed

Sources: <https://production.gtfsrt.vbb.de/>,
<https://production.gtfsrt.vbb.de/news.atom.xml>,
<https://unternehmen.vbb.de/digitale-services/datensaetze/> (which names it:
"Für Echtzeitinformationen stellen wir neben der API einen GTFS RT feed zur
Verfügung"), <https://github.com/OpenDataVBB/gtfs-rt-feed> (ISC; built from
VDV-453/454 data, matched against GTFS Schedule).

- **Access.** `https://production.gtfsrt.vbb.de/data`, no key, "up to a limit
  of 7 requests per minute" since 2026-11-02, "due to excessive traffic", in
  a "traffic budget shared by all consumers". An informative User-Agent with
  contact information is asked for, and generic ones have been blocked;
  compression (gzip, zstd) is asked for. CC BY 4.0. Announcements in the
  Atom feed above.
- **Content**, fetched and decoded: one `FULL_DATASET` FeedMessage, GTFS-RT
  2.0, of 10,681 entities, every one a TripUpdate: no VehiclePositions, no
  Alerts. 2,558,144 bytes gzipped, 11,301,009 raw. Using it needs the
  static GTFS (weekly, CC BY 4.0) to map trip and stop ids to anything
  shown.
- **Reliability.** "The feed has been lacking some data since 2026-06-04
  16:00" with no estimate (still on the page today); offline from 2026-03-10,
  for a DNS error 2026-02-17 to 18, and maintenance windows.

## What the device needs

- **WiFi or mobile data, not the tailnet.** The API is on the internet, and
  internet traffic does not go through the tailnet. The tailnet matters only
  for a self-hosted vbb-rest on the homelab, named in `config/transit.json`
  by its 100.x address or MagicDNS name (plain `http://` is then fine, as for
  Hermes).
- **HTTPS with the certificate bundle and a set clock.** One host, no
  redirects seen.
- **A body in PSRAM.** Ask `pretty=false`, `polylines=false`, `remarks=false`
  and a `results` cap (64 is plenty on 240x320; 256 is the Overlay cap). At 64
  vehicles expect about 300 KB raw. Either send `Accept-Encoding: gzip` and
  inflate (about 20 KB on the wire; the ROM's inflater, or the Reader's
  uzlib) or take it plain into a PSRAM buffer; both are fine on WiFi, gzip is
  kinder on LTE. Parse with the Podcasts app's `pod_json.c`, keeping only
  what the Overlay needs: line name, product, direction, latitude,
  longitude, trip id, and for departures the times, delay and platform.
- **A User-Agent** naming ThistleOS and its repository.
- **Privacy.** A radar request carries the map's bounding box, close to
  where the user is, and the device's address, to a service run by a private
  person with no published terms. Fetch only while the Transit app is open
  (the spec's rule already), ask for the box on screen, not the position;
  the self-hosted option keeps it home.

## Open points

- Whether VBB answers a request for an access ID for a personal device
  project, and the official API's JSON field names, are unknown until
  someone writes to api@vbb.de.
- vbb-rest's data licence: VBB's open datasets are CC BY 4.0, but the app
  API it wraps is under VBB's API rules, which ask for VBB to be named as the
  source. The Transit app's About line should name VBB and vbb.transport.rest
  either way.
- VBB's static GTFS stop ids were not opened here (the zip is large); the
  id shapes above are from vbb-rest's answers and vbb-stations.
