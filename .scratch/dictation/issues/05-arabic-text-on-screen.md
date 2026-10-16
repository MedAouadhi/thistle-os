# 05: Arabic text on screen

Status: in-review

**What to build:** An Arabic transcript reads as Arabic in a text field, as
does Arabic anywhere else in the UI. It was inserted correctly (UTF-8, as
Cohere sends it) but drawn as boxes: the only fonts compiled in are
Montserrat's, Latin only, and LVGL is built without
`LV_USE_ARABIC_PERSIAN_CHARS` (letters joined in their contextual forms).
`LV_USE_BIDI` was on, for the Reader.

- [x] A font with Arabic glyphs, as small as the T-Deck Max's flash allows:
      DejaVu Sans' Arabic at 14, 18 and 22 px, the sizes of the three
      Montserrat fonts the UI uses, antialiased and one bit a pixel for
      e-paper (`components/ui/src/fonts/`, from
      `components/ui/tools/gen_arabic_fonts.sh`), about 165 KB of flash for
      all six. A glyph a font lacks in the Arabic blocks is taken from the one
      nearest its size.
- [x] Arabic shaping and BiDi switched on, and their cost measured: the
      Reader's shaper moved to `components/ui/src/arabic_shape.c` and is run
      as LVGL draws and measures text (`arabic_text.c`, by wrapping LVGL's
      functions at link time); LVGL's own stays off, for the reason it always
      did. Every text finds its own direction (`base_dir` auto on the screen
      and layers): Arabic alone is right-aligned. A label's text is shaped
      afresh for each draw and measure, a copy on the heap, and one in
      LVGL's pool for the draw; text with no Arabic letter costs a scan.
- [x] A text area with Arabic and English mixed, and the cursor moving
      through it: the cursor, taps and up/down find their letter in the
      shaped text. `test_dictation.py`'s `arabic_is_drawn_as_arabic` checks
      a note: glyphs, joined letters narrower than the same kept apart, both
      lines at the right, the cursor where the text ends.
- [x] Received SMS and mesh messages in Arabic read too: the same fix, as
      it is beneath every widget.

**Hardware to confirm on the T-Deck Max:**

- [x] An Arabic dictation reads as Arabic on the panel, where it was boxes:
      the user's report after flashing. Whether the cursor sits at its left
      end and every dot is whole was not looked at closely.
- [ ] Arabic with its descenders (ي، ع، ج at the end of a word) in a one-line
      field: Montserrat 14's line is 16 px, shorter than DejaVu's, so a
      descender may lose a pixel at the field's bottom edge.
- [ ] A long Arabic note scrolls and edits without slowing the keyboard.

**Not done:** the "..." of a label cut short (`LV_LABEL_LONG_DOT`) is placed
by LVGL's label code calling its own functions, which the wraps do not
catch, so it comes a little early in Arabic. The Reader, Phone and Podcasts
still shape their own text and set it in the Reader's 16 px font; that is
drawn as it is.

**Blocked by:** nothing; found by ticket 02's hardware pass.
