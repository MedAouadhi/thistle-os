# Home

Status: in-progress (01 and 02 done in the simulator, not yet on hardware; 03 next, then 04 and 05)

Mockups: https://claude.ai/artifact/EizLLcJZzzajZjo8dFrv1Q (the user's chosen design; its third row is what 02 built)

An agent picking this up: read this spec, then `issues/03-what-is-new.md`, which has the files, the commands and what went wrong before. CLAUDE.md's Home section has the rules in short.

## Problem Statement

Home was a status bar and a dock of five icons over an empty canvas: 246 px
of an e-paper screen that stays on all day showed nothing. The status bar read
`. 20:15 Host Network:-30 BAT:72%`, which on 240 px cut the title to a dot.
Since then the kernel has come to hold what a home screen should show (unread
SMS and mesh messages, missed calls, the tailnet's state), and the device has a
keyboard that Home made no use of.

## Solution

A glance home, in four fixed bands, top to bottom:

1. **The clock and the date**, large (a 44 px digits-only Montserrat, one bit a
   pixel on e-paper). The status bar leaves out its own clock while Home is in
   front, and shows the network and battery as symbols.
2. **What is new**, at most two rows, each one tap from the thing it is about:
   unread messages, a missed call (with a call-back button), the tailnet
   waiting for a sign-in. With nothing new, "All caught up" and one line of
   status.
3. **Pinned apps**, six on a portrait screen (3 × 2), four on a landscape one.
   Positions do not move, so they can be learnt.
4. **Search and All apps.** Typing anywhere on Home searches apps and contacts.

A band the screen has no room for is left out: the T-Display loses what is
new, the Cardputer the clock as well, an OLED keeps only All apps. The bands
are computed in `home_core.c`, which needs no LVGL and is tested on the host.

### What is new reads the kernel, and nothing else

The rows are a fixed set inside the launcher, not a registry apps add to. Each
reads public, read-only kernel state, as the status bar and the call chip
already do, and a tap does what system UI already does. No app gets new code,
a new hook, or a change in behaviour.

| Row | Reads | Tap |
|---|---|---|
| New messages | `rs_sms_unread_count()`, `rs_mesh_unread_count()` | launch the Messenger, as the status bar's envelope does |
| Missed call | `rs_call_missed_count()`, `rs_call_log_get(0)`, `rs_phonebook_name_for()` | launch Phone, as the call chip does; the button calls back with `rs_call_dial()` |
| Tailscale waiting | `tailnet_get_status()` | launch Tailscale |
| Quiet: mesh on | `rs_mesh_is_running()`, `rs_mesh_get_contact_count()` | launch the Messenger |

Home must never call:

- `rs_msg_alert_take()`: it takes the pop-up, and the status bar would never
  show it.
- `rs_call_missed_seen()`: clearing the count is the Phone app's Recents'.
- The phonebook at boot: it loads on first use. Home asks it only when there
  is a missed call to name, or once the user types.

Left out because each would need code in an app: a podcast row, a Hermes reply,
"Resume" in search, "Ask Hermes" in search, and the sender's name and text on
the messages row (counts only). A sender's name could come later from a
read-only peek in the kernel's `msg_alert.rs`: a kernel change, not an app's.

### Search stays inside the same lines

Apps (`app_manager_list_apps()`) and contacts (`rs_phonebook_search()`). Call
is `rs_call_dial()`. Message is `rs_sms_request_compose()` and a launch of the
Messenger, the handoff the Phone app already makes and the Messenger already
takes.

### On e-paper

Black and white only: hierarchy from size and inversion, never grey. Focus
inverts. Nothing ticks: the clock is rewritten when the minute changes, a row
of what is new when what it says changes; no progress bars or countdowns.

### LVGL's pool

Home's widgets are built when it comes to the front and deleted when it
leaves, as the app drawer's are, so an app opened from Home has Home's share
of the 64 KB pool. A pinned app launches once its click is over, so its tile
is never deleted inside its own event. Tiles share their styles.

## Out of scope

The thistle-tk launcher (`tk_launcher.rs`) is unchanged: it hosts three apps,
and no board defaults to it.

## What the hardware has to confirm

- The minute's change refreshes only the clock's band on the T-Deck Max's
  panel, and a focus move only the two tiles.
- A tap on a pinned app, and on All apps, opens it the first time.
- The mono clock font reads well on the panel.
