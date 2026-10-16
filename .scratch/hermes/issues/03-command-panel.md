# 03: The Command panel

Status: in-review

**What to build:** A panel listing the commands from `CHAT_COMMANDS`;
choosing one writes `/name ` into the draft at the cursor and sends
nothing. It opens with the session down too, and says when there are none.

**Blocked by:** 02

## Comments

Sixteen rows at most, so the panel stays small in LVGL's pool; the rest are
named as a count. In the simulator, choosing /new and typing x leaves
"/new x" in the draft and sends nothing.
