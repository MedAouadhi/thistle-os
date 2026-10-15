# File Transfer

Status: built; to be confirmed on hardware

## Problem Statement

Getting a file onto a ThistleOS device means taking the SD card out, or
flashing a SPIFFS image, which also wipes the board profile and WiFi
settings. Getting one off it, a GPX track or a log, means the same. Fixing a
typo in `system.json` or a board profile needs a computer with the card in
it. The device has WiFi the whole time.

## Solution

A File Transfer app. Opened, it serves a web page on port 80 to the WiFi
network the device is on, and shows on its screen where to point a browser
and a password:

- `http://thistledev.local`, answered by mDNS (`espressif/mdns`), or the
  device's address where mDNS does not reach;
- a password such as `ABCD-EFGH`, made afresh each time the server starts.

Signed in, the page lists `/sdcard` and `/spiffs` and their folders, and
uploads (picked, or dropped on the page), downloads, renames, deletes, makes
folders, and opens a text file in an editor: line numbers, Tab indents,
Ctrl+S saves, CRLF kept, and a `.json` file checked before it is saved.
Pictures open in the page.

The server runs while the app is open. Leaving the app stops it, and so do
Stop and 15 minutes without a request. While it runs it holds the device
awake, since sleep stops WiFi.

## Security

Anyone on the same network can reach the device, so:

- **A password from the device's screen**, 8 characters from 32 (no 0, 1,
  O or I): 40 bits. Five wrong ones in a row stop sign-ins for 30 s, each
  further one doubles that, up to 10 min. "New password" on the device ends
  the lock and every session. Compared in constant time.
- **A session cookie**: 128 random bits, `HttpOnly; SameSite=Strict`, at
  most four at once. A request that changes something must also carry
  `X-Thistle-Request`; a page on another site cannot add it without a CORS
  preflight, which the server never answers.
- **Basic authentication for scripts**, with the same password and the same
  lock. A 401 never carries `WWW-Authenticate`, so no browser offers it or
  keeps it.
- **Only on WiFi.** esp_http_server listens on every interface, the modem's
  PPP link included when mobile data is on; a connection whose local
  address is not the WiFi address is closed before it is read.
- **Every path is checked** (`xfer_path_parse()`): absolute, in `/sdcard`
  or `/spiffs`, no `.` or `..`, no control characters, nothing FAT forbids.
- **Files are served inert**: text, `.html` included, as `text/plain`,
  pictures as themselves but not SVG, everything with `nosniff` and
  `Content-Security-Policy: sandbox`. The page itself allows nothing from
  elsewhere.

Plain HTTP: the password and the files cross the WiFi unencrypted, as they
would to any device on a home network without a certificate the browser
trusts. Nothing is logged that holds the password, except by the simulator.

## Decisions

- **One request at a time**, on esp_http_server's task (8 KB stack,
  internal RAM, only while the server runs). Buffers are in PSRAM; the
  password and sessions are not.
- **Uploads go to a temporary file** at the top of their root, then replace
  the old file, so one cut short leaves the old one. The free space is
  checked first, with 32 KB spare on SPIFFS, which slows badly when full.
- **SPIFFS has no folders**: a name is a whole path of at most 31 bytes.
  The listing makes folders of the names' first parts; making a folder does
  nothing until a file is put in it; a folder cannot be renamed.
- **Downloads send their length** (raw `httpd_send()`), so a browser shows
  progress.
- **The page is one file**, `web/index.html`, embedded by `web.cmake`.

## Out of scope

HTTPS; moving files between folders; uploading folders; editing files over
2 MB or not UTF-8; a QR code on the device's screen (LVGL's `lv_qrcode` is
not enabled).

## What the hardware has to confirm

- [ ] `http://thistledev.local` resolves from macOS, Windows, Linux, Android
      and iOS on the same WiFi.
- [ ] Uploading a few MB to the SD card and to SPIFFS, and downloading
      them, at what speed; the screen meanwhile.
- [ ] Internal RAM free while the server runs (the boot line's figure,
      after opening the app) and after leaving it.
- [ ] With mobile data on and WiFi off, the app waits for WiFi; with both
      on, the server answers on WiFi only.
- [ ] WiFi dropping and coming back: the same password works again.
- [ ] No SD card: `/sdcard` is not listed and uploads to it are refused.
- [ ] The device stays awake while the app is open and sleeps after
      leaving it.
