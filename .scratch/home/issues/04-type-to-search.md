# 04: Typing on Home searches

**What to build:** The All apps bar becomes a search field and a small All apps button. A letter typed on Home focuses the field; results are apps and contacts, Enter opens the first, Esc closes. The mic key dictates into the field, as into any.

**Blocked by:** 02.

**Status:** open

- [ ] Matching (word prefixes, case-insensitive) in a host-tested function
- [ ] Contacts from `rs_phonebook_search()`, asked only once the user types
- [ ] Call through `rs_call_dial()`; Message through `rs_sms_request_compose()` and a launch of the Messenger
- [ ] Results kept under LVGL's pool: shared styles, a handful of rows
