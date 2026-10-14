# 15: Buzz and pop up when a message arrives

**What to build:** A new SMS or MeshCore message vibrates the T-Deck Max and shows a pop-up with who it is from and the start of it, whatever app is open. Not for a message in the conversation the Messenger has on screen, and not at all when switched off in the Messenger's settings.

**Blocked by:** 14 (SMS and the message history).

**Status:** in-review; an SMS alert is yet to be seen on hardware

- [x] A haptic HAL interface, `hal_haptic_driver_t` (`hal/haptic.h`), appended to the registry so no existing field moves (ADR-0005), and exported to loadable drivers
- [x] A DRV2605 driver (`drv_haptic_drv2605.rs`): ERM, open loop, Library A, internal trigger, as LilyGo's `examples/motor` sets it up; in standby between plays
- [x] The T-Deck Max profile declares it: `com.thistle.drv.haptic-drv2605`, 0x5A, powered by XL9555 P05 (`en_expander`, `en_expander_pin`)
- [x] Every SMS and new MeshCore message goes through `msg_alert.rs`: a buzz (at most one per 5 s), and a pop-up the status bar shows, for the latest with a count of the rest
- [x] No alert for a message in the conversation on screen (`rs_msg_alert_set_viewing`), nor with `thistle_os.messenger.alerts` off (Messenger → gear → "Vibrate and pop up for new messages")
- [x] The simulator's T-Deck Max has a motor that prints what it plays
- [x] Verified on hardware for MeshCore messages
- [ ] Verified on hardware for an SMS

## Comments

### What it plays

`HAL_HAPTIC_MESSAGE` is Library A's Strong Buzz (14), 100 ms of nothing, and Strong Buzz again; `HAL_HAPTIC_CLICK` is Strong Click (1). Both from the datasheet's waveform table (11.2). The datasheet gives no lengths for them; if the buzz is too long, too short or too weak on the fitted motor, `sequence()` is the one place to change.

### Where it runs

The buzz is asked for where the message is received: the network coordinator's thread for an SMS, the kernel worker for MeshCore. A LoRa packet wakes the CPU without waking the UI (ticket 13), and the device should buzz all the same. The driver queues the play on the kernel worker, which writes the sequence and GO, and looks every 100 ms until GO clears before putting the part back in standby: standby stops a waveform at once (datasheet 7.4.1.4), and draws 1.9 µA against 0.6 mA ready (6.5).

The pop-up waits in the kernel for the status bar's once-a-second check on the LVGL task. One taken while the sleep screen is up is not shown (`toast_show` skips it there), though the envelope still counts its message. An SMS wakes the device (ticket 13), so it pops up; a mesh message heard in sleep buzzes, and pops up only if the UI has not taken it first.

### Simulator

`--sms "+4917612345678: Hallo"` on the tdeck-max: `[sim_haptic] message buzz` as the SMS is fetched, and the pop-up 0.2 s later. With the conversation opened before it arrived, neither. With `"alerts": false` in system.json, neither. `--mesh-direct "Heltec: are you there?" --mesh-message "Bob: hi all"`: one buzz, and "Bob in Public: hi all (+1 more)".

### What the hardware has to confirm

- The DRV2605 answers at 0x5A once P05 is raised, with a DEVICE_ID of 3 or 7. The log says `drv2605: ready (id N), in standby`, or why not.
- The buzz is felt, and is not too long.
- The board's `board.json` gets the new entry only when rewritten (`rm /spiffs/config/board.json`, then reboot), as for `"ri"` in ticket 14.

### On the hardware

After `board.json` was rewritten: `haptic driver registered: DRV2605`, then `drv2605: ready (id 7), in standby`, so a DRV2605L. Two MeshCore direct messages, 25 s apart, each buzzed (felt, and judged about right) and popped up, 0.7 s and 0.4 s after `meshcore: rx message`. No SMS arrived while the log was being recorded.
