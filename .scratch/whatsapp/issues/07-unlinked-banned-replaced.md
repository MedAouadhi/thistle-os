# 07: Unlinked, banned, replaced

**What to build:** When WhatsApp unlinks the device, the store is erased,
the background check stops, and the app says so and offers to link again.
When it bans the number for a while, the check stops until the ban's end,
a pop-up says so once, and the app shows until when. When another client
takes the connection over, it is logged and the next check goes on as
usual. Nothing retries sooner than that. What each event does is a pure
policy, host-tested.

**Blocked by:** 05

**Status:** ready-for-agent

- [ ] Host tests for each of the three events and for a check after each
- [ ] `test_whatsapp.py`: the fake unlinks: store erased, checks stop, the app shows the link page with a notice; the fake bans until a time: checks stop until then, one pop-up, the app shows the end time; replaced: logged, the next check runs
- [ ] No connection is attempted between an unlink and a new link, or before a ban ends

## Comments
