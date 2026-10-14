# Haptics are a HAL interface, appended to the end of the registry

A new message should buzz the vibration motor, and nothing could drive it: the T-Deck Max carries a TI DRV2605 (I2C 0x5A, powered by XL9555 P05, T-Deck-MAX `docs/pinmap.md` section 7), and the HAL had no interface for it. `hal/board.h` held a TODO saying that adding one changes `hal_registry_t`, the syscall table and every consumer, a kernel ABI change that needed its own design. We added `hal_haptic_driver_t` (`hal/haptic.h`) and its two registry fields, `haptic` and `haptic_config`, after `board_name`, the last field. Every field that was there keeps its offset, so a driver or app built against the old header reads the registry as it did. `hal_haptic_register` is exported to loadable drivers through the syscall table.

The interface takes what the user should feel, not a waveform: `HAL_HAPTIC_CLICK` and `HAL_HAPTIC_MESSAGE`. A driver for a haptic chip picks from its library; one for a motor on a plain GPIO, as on the original T-Deck Pro (`BOARD_MOTOR`, GPIO 2), would time pulses itself. `play()` starts an effect and returns, so a caller on any thread never waits for a motor.

## Considered Options

**Placing the fields with the other drivers**, after `imu`, where the TODO sat, reads better but moves `storage` and every field after it. Anything built before then would read the wrong field.

**A kernel-private hook** (a function pointer outside the registry, set by the driver) would have avoided the ABI question altogether. It was not chosen because the registry is where every other part's driver is found, by the kernel and apps alike, and a second way would have to be learned for one part.

**Driving the DRV2605 from the message code directly** would put a register map in the kernel, which talks to hardware only through HAL drivers.

## Consequences

The DRV2605 driver (`drv_haptic_drv2605.rs`) is set up as LilyGo's examples set it up: ERM, open loop, Library A, internal trigger. It keeps the part in standby between plays, at 1.9 µA rather than the 0.6 mA it draws ready. Standby stops a waveform at once, so a job on the kernel worker looks every 100 ms until GO clears before setting it, rather than holding the worker for the length of a buzz.

A board whose stored `board.json` predates the `haptic-drv2605` entry has no motor until its profile is rewritten.
