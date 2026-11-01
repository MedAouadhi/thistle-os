# 05: WhatsApp background check and alerts

**What to build:** Once linked, messages come in while the app is closed.
WhatsApp registers a background check at the interval in the app's
settings: 5, 15, 30 or 60 minutes, or Off, 15 by default
(`thistle_os.whatsapp` in `system.json`). The check connects, takes what
the server kept until the offline queue is done or a timeout, hands each
message to the same handling as the live app, and disconnects. While the
app is open the check is paused. A direct message, or a group message that
mentions the user or replies to them, buzzes and pops up through
`msg_alert`, unless the app has that chat on screen. Unread WhatsApp
messages count in the status bar's envelope and Home's new-messages row.
Which messages alert is a pure policy, host-tested.

**Blocked by:** 02, 04

**Status:** ready-for-agent

- [ ] Host tests: a direct message alerts; a group message alerts only when it mentions or replies to the user; a chat on screen does not alert
- [ ] `test_whatsapp.py`: with the app closed, queued messages from the fake are taken by a check, logged, alerted, and counted in the envelope and on Home; group chatter with no mention is logged but not alerted
- [ ] The check is paused while the app is open and resumes when it closes
- [ ] The interval setting changes the check's interval; Off stops it
- [ ] A check is logged with what it took, how long, and the internal RAM free
- [ ] On the device: a message sent while the device sleeps arrives within one interval, from light sleep

## Comments
