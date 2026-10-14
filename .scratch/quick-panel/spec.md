# Quick panel

Status: ready-for-agent

## Problem Statement

Switching the radios, the lights or the sound on a ThistleOS device means
opening Settings and finding the page each one lives on: WiFi and Mobile data
on their own pages, the screen light under Drivers → Display, the key light
under Drivers → the keyboard, GPS under Drivers → GPS. LoRa cannot be
switched off at all short of editing system.json, and the volume cannot be
set from anywhere ("Volume control: future work" on the Audio page).

Some of these also have no single owner holding their state. GPS is switched
by whoever calls the driver, and Settings keeps a flag of its own that starts
out saying "on" while the receiver is off. Nothing remembers the volume or
whether the device is muted. The light levels are written to system.json but
nothing can say what they are now.

## Solution

A quick panel, as a phone has: drag down from the status bar and a panel of
switches drops over whatever app is open. A tap switches one; Back, Home, a
tap below the panel or a drag back up closes it. On the T-Deck Max the middle
touch key below the screen opens and closes it too.

The panel holds:

- **Switches** (tiles, inverted while on): WiFi, Mobile data, GPS, LoRa, Key
  light, Mute.
- **Levels** (sliders): Screen light, Volume.
- A way into Settings.

A tile or a slider shows only on a board that has the hardware behind it.

The panel is a registry: each switch and level is a small struct of
callbacks, and the built-in ones are registered the same way any later
feature would add its own (`quick_panel_add_toggle()`,
`quick_panel_add_level()`).

Each switch's state is owned by the kernel, not the panel, so the panel,
Settings and apps agree on it, and every choice survives a reboot:

| Switch | Owner | system.json |
|---|---|---|
| WiFi | `wifi_manager_set_enabled` (exists) | `wifi.radio_off` |
| Mobile data | `net_cellular_set_enabled` (exists) | `cellular.enabled` |
| GPS | new `gps_manager.rs` | `gps.enabled` |
| LoRa | `mesh_manager.rs`, which can now stop the node | `mesh.enabled` |
| Key light, Screen light | `light_prefs.rs`, which now keeps the levels | `keyboard.light`, `display.light` |
| Mute, Volume | new `audio_volume.rs` | `audio.muted`, `audio.volume` |

## User Stories

1. As a user, I want to drag down from the top of the screen and see my
   switches, so that I do not have to dig through Settings.
2. As a user, I want to switch WiFi, Mobile data, GPS and LoRa on and off with
   one tap each, so that I can save battery or go quiet quickly.
3. As a user, I want to switch the key light on and off and set the screen
   light, so that I can read in the dark.
4. As a user, I want to mute and set the volume, so that the device is quiet
   when it should be.
5. As a user, I want what I switched to stay switched after a reboot.
6. As a user on an e-paper device, I want the panel to cost as few refreshes
   as possible.
7. As a T-Deck Max user, I want a key that opens the panel, so that I can
   reach it without dragging.
8. As a user with a keyboard, I want to move through the panel with the keys
   and close it with Esc.
9. As a developer, I want to add a switch to the panel without touching the
   panel's code.

## Implementation Decisions

### Opening and closing

The gesture is recognised in the UI manager's HAL input callback, from the
raw touch stream, by a pure state machine (`pull_gesture.c`) that has host
tests. A touch that goes down inside the status bar and moves down by at
least an eighth of the screen's height (32 px at least), more down than
sideways, opens the panel. It opens as soon as the threshold is crossed,
without waiting for the finger to lift: on e-paper the refresh is the slow
part, and starting it early is what makes the panel feel quick. While the
panel is open, a touch anywhere that moves up by the same amount, more up
than sideways, closes it; a sideways drag on a slider does not.

The callback only records what it saw; an LVGL timer acts on it, as it does
for Home and Back, so all LVGL work happens with the LVGL lock held.

`HAL_KEY_QUICK` (0x0102) toggles the panel. The CST66xx driver reports it for
the key its board profile names `quick_key`; the T-Deck Max's middle key,
key 1, which nothing used. The driver defaults `quick_key` to 1 when the
profile does not name it, as it does `home_key` and `back_key`, so a Max
whose stored `board.json` predates this does not need it rewritten.

Back and Esc close the panel before anything else; Home closes it and goes
home.

### Drawing

The panel is built when opened and deleted when closed, so it costs LVGL's
64 KB pool nothing while shut. It hangs below the status bar, which stays
visible, and scrolls if a small landscape screen cannot fit it. A clear
backdrop over the rest of the screen takes the tap that closes it (clear, not
dimmed: a dimmed screen dithers on e-paper). Tiles share `lv_style_t`s, and
"on" is LVGL's checked state, drawn inverted, which reads in black and white.

While open, a 1 s timer asks every tile for its state and changes only the
tiles whose state changed, so on e-paper a quiet second costs no refresh.

A slider moves the hardware as it is dragged and saves once, when let go.

### The registry

```c
typedef struct {
    const char *symbol;               /* drawn above the label */
    const char *label;
    bool (*available)(void);          /* NULL: always */
    bool (*is_on)(void);
    const char *(*set_on)(bool on);   /* NULL on success, else what to tell the user */
} quick_toggle_t;

typedef struct {
    const char *symbol;
    const char *label;
    bool (*available)(void);
    uint8_t min;                      /* lowest level the slider offers */
    uint8_t (*get)(void);
    const char *(*set)(uint8_t percent, bool save);
} quick_level_t;
```

Registration order is display order. `set_on` returns its own message rather
than an error code, because only it knows what went wrong in words a user
can act on ("No modem on this board", "Not saved for next boot").

### Kernel owners

- **GPS** (`gps_manager.rs`, `thistle/gps_manager.h`): `rs_gps_set_enabled`,
  `rs_gps_is_enabled`, restored at boot after the drivers start. On unless
  saved off: the HAL registry starts the reader at boot (5684319). The `thistle_gps_enable`
  syscall goes through it, unsaved, so the panel sees an app's switch.
  Settings' GPS page uses it in place of its own flag.
- **LoRa** (`mesh_manager.rs`, `meshcore_shim.cpp`): `meshcore_stop()` cancels
  the node's job on the kernel worker and puts the radio to sleep; starting it
  again wakes the radio and puts it back on the mesh's channel without loading
  the node a second time. `rs_mesh_set_enabled` saves `mesh.enabled` and
  starts or stops; at boot a node switched off leaves the radio asleep.
- **Volume** (`audio_volume.rs`, `thistle/audio_volume.h`): the level and the
  mute, saved, applied to the driver at boot and on every change. What the
  driver gets is 0 while muted and the level otherwise. The
  `thistle_audio_set_volume` syscall goes through it, unsaved.
- **Lights** (`light_prefs.rs`): keeps the level each light is at, and
  whether the board has it. A light is taken to be there until its driver says
  `ESP_ERR_NOT_SUPPORTED`; at boot each is set to its saved level, or to the
  dark it starts in on a reflective screen and the keys, which settles that
  without lighting anything. Settings sets them through it.

## Testing Decisions

- `pull_gesture.c`: host tests in `simulator/tests/` (opens on a drag down
  from the bar; not from below it; not for a sideways drag; once per touch;
  closes on a drag up only while open; a slider drag does not close).
- Rust policy functions, each with host tests: the effective volume, reading
  each new setting from system.json, which lights a board has.
- Simulator: `--drag X1,Y1,X2,Y2` and `--tap quick`, and
  `simulator/tests/test_quick_panel.py`: the panel opens on a drag on the
  T-Deck Max (e-paper) and the T-Deck (LCD), opens on the quick key, a tap on
  a tile switches it and the choice reaches system.json, a drag up closes it.
- Hardware, on the T-Deck Max: the gesture, the middle key, each switch.

## Out of Scope

- thistle-tk. It has its own WM and input path; the registry is C and could
  later be drawn there.
- Bluetooth: no Bluetooth stack runs on the boards with a panel today.
- A keyboard shortcut on boards with neither touch nor a spare key.
- Long-press on a tile to open its Settings page.
- Animation that follows the finger.
- Apps adding tiles through the syscall table.
