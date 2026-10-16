# 04: Hermes Sessions in the drawer

Status: in-review

**What to build:** The drawer asks for `SESSION_LIST` as it opens and
draws three states: loading, a list (or an empty history), and needing
Hermes. Choosing a row sends `SESSION_SELECT`; the row in use is inert.
The drawer also holds New conversation, Scheduled Runs and the bridge's
address.

**Blocked by:** 02

## Comments

In the simulator: the drawer asks once as it opens, marks the current
session and does not let it be chosen, and choosing another sends its id;
the bridge's reset and sync then draw the other session's messages.
