# 05: Arabic text on screen

Status: needs-triage

**What to build:** An Arabic transcript reads as Arabic in a text field. It
is inserted correctly now (UTF-8, as Cohere sends it), but drawn as boxes:
the only font compiled in is Montserrat, Latin only, and LVGL is built
without `LV_USE_ARABIC_PERSIAN_CHARS` (letters joined in their contextual
forms) or `LV_USE_BIDI` (right-to-left runs in a left-to-right line).

- [ ] A font with Arabic glyphs, as small as the T-Deck Max's flash allows
- [ ] LVGL's Arabic shaping and BiDi switched on, and their cost measured
- [ ] A text area with Arabic and English mixed, and the cursor moving through it
- [ ] Received SMS and mesh messages in Arabic read too: the same fix

**Blocked by:** nothing; found by ticket 02's hardware pass.
