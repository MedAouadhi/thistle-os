# 03: Link and unlink from the WhatsApp app

**What to build:** In a WhatsApp build, the WhatsApp app appears and links
the device to the user's account as a **linked device**. Its link page
shows WhatsApp's QR code, redrawn as each new one comes, with "Link with
phone number" as the alternative: the user types their number and the
page shows a code to type on the phone. Once linked it shows the account's
name and number, and stays linked across restarts, sleeps and power-offs.
Unlink, in the app's settings, unlinks and forgets it.

This brings in the two pieces the rest builds on:
- **The WhatsApp client interface**, the only place whatsapp-rust is
  called from. In: link by QR or code, connect, disconnect, unlink, send.
  Out, as events: QR code, pairing code, linked, message, receipt, typing,
  offline queue done, unlinked, temporarily banned, replaced,
  disconnected. The device's implementation drives whatsapp-rust on its
  own task. The simulator's is a fake chosen by `THISTLE_SIM_WHATSAPP`.
- **The linked device's store**, whatsapp-rust's store traits kept in
  files under `/sdcard/whatsapp/`, each encrypted and authenticated with a
  random 256-bit key made at linking and kept in internal NVS (until
  ADR-0012's credential partition takes it). A file that does not
  authenticate reads as missing, never as plain text. Unlink erases the
  files and the key.

**Blocked by:** 01

**Status:** ready-for-agent

- [ ] The app is registered only in a WhatsApp build
- [ ] Host tests of the store: what is saved reads back; a wrong key or a damaged file reads as missing; erase removes every file and the key
- [ ] `test_whatsapp.py`: linking by QR and by code against the fake; the QR redraws; the linked page shows the account; it is still linked after a restart; unlink returns to the link page and empties `/sdcard/whatsapp/`
- [ ] On the device, with the spare number: link both ways, restart, still linked

## Comments
