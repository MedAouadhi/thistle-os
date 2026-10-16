# 02: Device Chat

Status: in-review

**What to build:** The conversation: the transcript the bridge syncs,
Hermes's replies and their edits, activity rows, the compose field, Stop
and New conversation.

- [x] `hermes_chat.c`: the transcript (rows by id, parts, groups removed
      for an edit, notes that move no position), activity rows by id and
      their presentation at each visibility, commands, sessions,
      Scheduled Runs, host-tested
- [x] Every server frame applied (`hermes_proto.c`), host-tested
- [x] Markdown: md4c and hermes_esp32's `hermes_markdown.c`; blocks drawn
      in LVGL; tables as a grid or as `Header: value` groups
- [x] Characters the fonts lack written as ASCII, symbols, or dropped
- [x] Enter sends while the session is up; otherwise the draft stays and a
      toast says why
- [x] Stop while a turn runs; New conversation (`CHAT_RESET`)
- [x] The view follows the newest content unless scrolled back
- [x] Held awake while a turn is in flight, ten minutes at most

**Blocked by:** 01

## Comments

`test_hermes_proto.c` applies a sync, replies in parts and their edits,
activity rows and each visibility, errors as notes that move no position,
and a reset. `test_hermes_glyphs.c` covers what the fonts lack.

In the simulator: a typed question goes as CHAT_SUBMIT with a 16-digit
request id, and the transcript ends as the history, the question, the
tool's collapsed row and the reply, which arrived as a message, then two
edits. A table that fits is a grid, a wider one `Header: value` lines; the
Arabic paragraph is joined and right-aligned. Stop sends `CHAT_CONTROL
stop` while a tool runs; New conversation resets to the hint.

The draft is one line, as the Messenger's and the Assistant's are: Enter
sends it. The simulator takes 16 keys at once, so its tests type short.
