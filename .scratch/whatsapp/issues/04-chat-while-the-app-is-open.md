# 04: Chat while the app is open

**What to build:** While the WhatsApp app is open it holds a live
connection, holding the device awake, and the user can read and write. The
chat list shows chats most recent first, each with its name, last message
and unread count. A chat shows its recent messages, with senders' names in
groups, and a compose field (an ordinary text field, so dictation works)
that sends text. Names come from WhatsApp's push names, falling back to the
phonebook by number. Media, voice notes, stickers and other kinds show as
placeholders such as `[photo]`. Sending with no connection says so; nothing
is queued. It works over WiFi and mobile data.

Every message, in or out, is appended by `msg_log` under a new kind,
`whatsapp`, one file per chat, unless the Messenger's `save_history` is
off. The app reads the latest chats back after a restart. The Messenger
does not show them: `msg_log`'s and `msg_alert`'s transport lists gain a
third kind, and the Messenger's read-back keeps to SMS and mesh.

Lists are one `lv_table` each and message text is kept in PSRAM, because
of LVGL's 64 KB pool.

**Blocked by:** 03

**Status:** ready-for-agent

- [ ] `test_whatsapp.py`: messages from the fake appear at once in the list and the chat, direct and group, with names; a sent message reaches the fake; media shows as a placeholder; a send with the fake disconnected is refused with a notice
- [ ] `msg_log` writes `/sdcard/messages/whatsapp/<chat>.jsonl`, not with `save_history` off; the app shows the history after a restart; the Messenger does not
- [ ] A phonebook name is used when there is no push name
- [ ] A chat of 20 messages and a list of 12 chats fit in LVGL's pool, measured with `lv_mem_monitor()`
- [ ] On the device, with the spare number: send and receive in a direct chat and a group

## Comments
