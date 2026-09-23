# T-Deck Max board support

Status: ready-for-agent

## Problem Statement

The LilyGo T-Deck Max is unsupported by ThistleOS, and worse than unsupported: it is actively misidentified.

The Max is LilyGo's third revision of the T-Deck Pro (their own version table reads Pro V1.0 → Pro V1.1 → Pro MAX). It shares the Pro's e-paper panel, keyboard controller, IMU, GNSS module and LoRa radio, and sits on the same I²C pins. The kernel's hardware fingerprinting identifies a T-Deck Pro by the presence of a TCA8418 keyboard controller and a BHI260AP IMU on SDA 13 / SCL 14 — a signature the Max matches exactly.

The consequence is that plugging in a T-Deck Max today causes ThistleOS to write the T-Deck Pro board profile to SPIFFS and boot as a Pro. The user gets a board that comes up confidently wrong: wrong power driver, wrong audio driver, and a display whose reset line is never driven. Nothing reports an error.

The Max also introduces a hardware pattern ThistleOS has never encountered. Eleven peripheral control lines — including all four peripheral power rails — sit behind an XL9555 I²C IO expander rather than on native GPIOs. The current boot sequence applies board-level GPIO outputs *before* initialising buses, which means the Max's power-up sequence cannot be expressed at all: the component that powers the peripherals is itself a peripheral on a bus that does not yet exist.

## Solution

Add the T-Deck Max as a first-class supported board, under the board id `tdeck-max`.

Three things have to happen for that to be true.

**The boot sequence gains an IO-expander phase.** A new pair of board profile sections declares IO expanders and the pin states to apply to them. Expanders are initialised after buses come up and before drivers load, and their pins are driven immediately after. A single syscall lets a driver set an expander pin at runtime, so a driver that owns a line — notably the audio driver, which gates its own speaker amplifier — is not dependent on a board profile having done it at boot.

**Hardware detection learns to tell the boards apart.** The XL9555 is the discriminator LilyGo themselves document for this purpose, and it is absent from every other board in the catalogue. The Pro's fingerprint is narrowed so an ambiguous match declines to guess rather than guessing wrong, and a Max rule is added above it. The Max also gains an embedded fallback board profile, since detection is worthless without a profile to write, and a CI check that the embedded profiles have not drifted from the board profile files they duplicate.

**Three new drivers, and one existing driver corrected.** The Max needs an IO expander driver, a power driver, and an audio codec driver. The power driver also fixes a live bug on the T-Deck Pro, whose board profile currently declares a charger the board does not have and reads battery voltage from an ADC pin that is physically the LoRa reset line.

Everything else the Max needs already exists and is reused with new pin configuration.

## User Stories

1. As a T-Deck Max owner, I want ThistleOS to recognise my board as a T-Deck Max, so that I do not silently get a T-Deck Pro's configuration.
2. As a T-Deck Max owner, I want the kernel to decline to guess when it cannot identify my hardware, so that an unknown board fails visibly instead of booting wrong.
3. As a T-Deck Max owner, I want my board to boot to a usable system from a fresh flash with no SD card, so that first-run provisioning works without manual configuration.
4. As a T-Deck Max owner, I want the e-paper display to render, so that I can see the system at all.
5. As a T-Deck Max owner, I want the display's reset line driven at init, so that the panel recovers from a bad state rather than needing a power cycle.
6. As a T-Deck Max owner, I want the display front-light to be controllable, so that I can read the screen in the dark.
7. As a T-Deck Max owner, I want the keyboard to work, so that I can type.
8. As a T-Deck Max owner, I want the keyboard to be reset at boot, so that it starts from a known state even after an unclean shutdown.
9. As a T-Deck Max owner, I want all 35 keys of the 4×10 matrix to register, so that no key is silently dead.
10. As a T-Deck Max owner, I want the keyboard backlight to be controllable, so that I can type in the dark.
11. As a T-Deck Max owner, I want the touch panel to work, so that I can interact by touch as well as by key.
12. As a T-Deck Max owner, I want the touch panel to be reset at boot via the IO expander, so that it initialises reliably.
13. As a T-Deck Max owner, I want WiFi and Bluetooth to work, so that the board can reach a network.
14. As a T-Deck Max owner, I want the SD card to mount, so that I can load apps, drivers and themes from removable storage.
15. As a T-Deck Max owner, I want an accurate battery percentage, so that I know when to charge.
16. As a T-Deck Max owner, I want an accurate battery voltage reading, so that diagnostics are meaningful.
17. As a T-Deck Max owner, I want to see whether the board is charging, so that I can tell a working charger from a dead one.
18. As a T-Deck Max owner, I want battery reporting to work whether my unit has the SY6970 or the BQ25896 charger, so that a hardware revision I did not choose does not break my device.
19. As a T-Deck Max owner, I want the charger's configuration to survive, so that charge current and input limits are not silently reset by a watchdog while the system idles.
20. As a T-Deck Max owner, I want GNSS to work, so that location-aware apps function.
21. As a T-Deck Max owner, I want the GNSS module to be powered on at boot, so that it acquires a fix without manual intervention.
22. As a T-Deck Max owner, I want LoRa to work at 868 MHz, so that I can use long-range radio apps.
23. As a T-Deck Max owner, I want the LoRa module to be powered on at boot, so that the radio driver finds hardware when it initialises.
24. As a T-Deck Max owner, I want a defined LoRa antenna selection at boot, so that transmission is not routed to an unconnected port.
25. As a T-Deck Max owner, I want audio playback through the speaker, so that apps can make sound.
26. As a T-Deck Max owner, I want the speaker amplifier disabled when nothing is playing, so that the board does not hiss or waste battery at idle.
27. As a T-Deck Max owner, I want audio to route to the codec rather than the modem, so that app audio is what I hear.
28. As a T-Deck Max owner, I want IMU data available, so that orientation-aware apps function.
29. As a T-Deck Max owner, I want the IMU's 1.8 V rail enabled at boot, so that the sensor is present when its driver probes.
30. As a T-Deck Max owner, I want peripherals whose hardware is absent to be cleanly unregistered rather than faked, so that apps can detect real capabilities.
31. As a ThistleOS developer, I want board profiles to declare IO expanders declaratively, so that adding an expander-based board needs no kernel recompilation.
32. As a ThistleOS developer, I want expander pin entries to carry direction as well as level, so that pins are configured as outputs rather than left floating as inputs.
33. As a ThistleOS developer, I want expanders initialised after buses and before drivers, so that power rails are up before the drivers that depend on them probe.
34. As a ThistleOS developer, I want a driver to be able to set an expander pin at runtime, so that a driver owning a control line does not depend on board profile configuration having done it.
35. As a ThistleOS developer, I want the boot sequence's ordering to be assertable in a test, so that an ordering regression is caught before it reaches hardware.
36. As a ThistleOS developer, I want a single charger driver covering both the SY6970 and the BQ25896, so that two register-compatible parts do not become two codebases.
37. As a ThistleOS developer, I want the charger variant identified from its part-number register rather than its revision register, so that a die revision does not break detection.
38. As a ThistleOS developer, I want battery percentage sourced from the fuel gauge rather than a voltage curve, so that reporting is accurate across the discharge range.
39. As a ThistleOS developer, I want the T-Deck Pro's incorrect power driver replaced, so that the Pro stops reading battery voltage from its LoRa reset pin.
40. As a ThistleOS developer, I want the corrected power driver shared between the Pro and the Max, so that the fix benefits both boards.
41. As a ThistleOS developer, I want the audio codec driver to wrap the maintained first-party component, so that a long register init sequence is not reimplemented and maintained here.
42. As a ThistleOS developer, I want embedded fallback board profiles checked against their board profile files in CI, so that the only configuration a freshly flashed board ever sees cannot drift.
43. As a ThistleOS developer, I want the new board profile sections schema-validated in CI, so that a malformed profile is caught before it ships.
44. As a ThistleOS developer, I want hardware detection to have a discriminating probe per board, so that boards sharing a chip signature are distinguishable.
45. As a ThistleOS developer adding a future expander-based board, I want the T-Deck Max to serve as the worked example, so that I do not have to rediscover the pattern.
46. As a developer without a T-Deck Max on my desk, I want the board to run in the simulator, so that I can work on it without hardware.
47. As an agent implementing this feature, I want a simulated IO expander that records writes, so that I can verify boot ordering without a flash cycle.
48. As an agent implementing this feature, I want the register decoding of the power driver to be testable without hardware, so that the arithmetic is verified before bring-up.
49. As a user provisioning a board through Recovery, I want to select the T-Deck Max from the catalogue, so that I can install the right profile deliberately.
50. As a user provisioning a board, I want the published board catalogue to include the T-Deck Max, so that provisioning offers it.

## Implementation Decisions

### Board profile schema

Two new optional top-level sections. Both are parsed by the board configuration module and validated by the board catalogue validator.

The shape, which encodes the decisions that prose states imprecisely:

```json
"io_expanders": [
  { "id": "com.thistle.drv.ioexp-xl9555", "i2c_bus": 0, "i2c_addr": "0x20" }
],
"expander_outputs": [
  { "expander": 0, "pin": 1, "direction": "output", "level": 1, "delay_ms": 0 }
]
```

- `expander` is an index into the `io_expanders` array, mirroring how `spi_bus` and `i2c_bus` index into the registered bus arrays rather than naming a host or port.
- `direction` is required, not inferred. The XL9555's configuration registers reset to all-inputs, so an entry that sets a level without setting direction leaves the pin floating and the rail off. Making direction explicit in the schema means the failure is a validation error rather than a board that silently does not power up.
- Both arrays are bounded like the existing `gpio_outputs` and bus arrays.

### Boot sequence

The sequence becomes: board-level GPIO outputs → buses → **IO expanders** → **expander outputs** → drivers.

IO expanders are initialised after buses because they sit on a bus. Expander outputs are applied immediately after, and before drivers load, because they carry the peripheral power rails that drivers probe for. A failure to initialise a declared expander aborts before driver load, matching the existing behaviour for a failed bus.

### Runtime expander access

One syscall, exposed through the syscall table, sets an expander pin at runtime. It is deliberately minimal — a single setter, not a HAL interface.

This exists because two lines cannot be static: the speaker amplifier enable, which the audio driver must gate so the board does not hiss at idle, and the motor rail. Without it, the amplifier would sit permanently enabled.

This is the seam from which a full IO-expander HAL interface will grow when the 4G modem is implemented, since the modem's power key must be *pulsed* by its driver rather than set once at boot. That evolution is documented in ADR-0001 rather than built now.

### Board identity

Board id `tdeck-max`, matching the existing `tdeck-pro` / `tdeck-plus` naming and the product name. The firmware self-reports `T-Deck-Pro Max V0.1`; the shorter id is preferred as the catalogue identifier. The validator requires the board profile filename stem to equal the board id.

### Hardware detection

The Pro's fingerprint rule is narrowed so that a keyboard controller plus IMU signature alone no longer resolves to a T-Deck Pro; an ambiguous match falls through to unidentified. A Max rule is added, keyed on the XL9555's I²C address, which is absent from every other board in the catalogue and is the discriminator LilyGo documents for exactly this purpose. The expander's address is added to the probe list.

The Max gains an embedded fallback board profile, because detection writes an embedded profile and a board with no profile to write cannot be detected usefully.

A CI check asserts that each embedded fallback profile matches its corresponding board profile file. Two embedded profiles have already drifted from their files, with nothing catching it; the Max's profile is the only configuration a freshly flashed Max ever sees, so drift there is a first-boot failure.

### Drivers

**New — IO expander (XL9555).** A standard 16-bit expander with the PCA9535-compatible eight-register layout: input, output, polarity and configuration registers, two ports each. Configuration semantics are `0 = output, 1 = input`, resetting to all-inputs. Handled by the board layer rather than registered as a HAL vtable, since it is not a peripheral class the HAL models. Its interrupt line is not routed to the SoC on this board, so the driver is poll-only by necessity — there is no interrupt path to design.

**New — power (SY6970 / BQ25896 charger + BQ27220 fuel gauge).** One driver, registering one power HAL vtable, reading two chips.

The two chargers are register-map clones: the same register map, the same scaling constants, the same bit-level decode for battery voltage, charge status and bus status. They differ in I²C address, maximum charge current, and a few extra bus-status codes. The driver probes both addresses and identifies the variant from the part-number field of the device identification register — not the revision field, which the vendor datasheet documents as starting from zero and which a die revision could therefore invalidate.

Battery percentage comes from the fuel gauge, which requires no unsealing for the reads involved; a voltage curve is the fallback when the gauge is absent. Battery voltage and charge state come from the charger.

Two initialisation requirements that are not optional on this board: the charger's I²C watchdog must be disabled or fed, since on expiry it resets the charger's configuration registers to defaults and silently undoes charge current and input limit configuration; and continuous ADC conversion must be enabled, since the converter does not free-run by default and the voltage register reads stale until a conversion occurs. The charger's interrupt is not routed to the SoC, so polling is the only option.

**New — audio codec (ES8311).** Wraps the maintained first-party ESP-IDF codec component rather than reimplementing the register init sequence. This follows the existing precedent for the two C drivers in the tree, both cases where a mature vendor library was preferred to a port. The driver owns its amplifier enable line through the runtime expander setter.

**Reused with new configuration.** The e-paper display driver (reset line now on a native GPIO where it was absent on the Pro, plus a front-light PWM line), the keyboard controller driver (reset moves to an expander pin), the touch driver (reset moves to an expander pin), the GNSS driver (UART pins move, as the Pro's GNSS pins are the Max's console UART), the LoRa driver (pins unchanged from the Pro; 868 MHz), and the SD card driver.

**Corrected.** The T-Deck Pro's board profile is switched from its incorrect charger driver to the new power driver. The Pro has the same fuel gauge and a register-compatible charger, so the fix is shared rather than Max-specific.

**Absent hardware.** The Max has no RTC and no ambient light sensor. Neither is declared; the RTC HAL falls back to the system tick as designed.

### Deferred capability, recorded not built

The DRV2605 haptic driver is present on the board but there is no haptic HAL interface to register it against. Adding one is a kernel ABI change touching the registry structure, the syscall table and every consumer — its own piece of work with its own design discussion, not a rider on a board bring-up. Recorded as a TODO. It would serve two boards, since the Pro V1.1 also carries the part.

Audio capture is the same shape: the codec has a microphone input, but the audio HAL has no capture path. Playback only, with the gap recorded.

## Testing Decisions

A good test here asserts externally observable behaviour — what the boot sequence *did*, in what order, and what a driver computed from a given register value — not how the code is structured internally. Tests that assert a particular function was called, or that a particular intermediate variable holds a value, are not wanted.

### Seam 1: the board configuration boot sequence

The primary seam, and the one that guards the feature's most likely failure mode.

The boot sequence is extracted into a single function generic over its side effects: set GPIO output, initialise SPI bus, initialise I²C bus, initialise IO expander, set expander pin, load driver. Production supplies the real implementations; tests supply a recorder.

This is a refactor of the existing path, not an addition alongside it. Bus initialisation is already generic over injected closures and tested that way; board-level GPIO outputs and driver loading are not — they call their externs directly and are compiled out of test builds entirely, which is why the existing tests in this module can verify parsing but cannot observe sequencing at all. Extending the existing pattern to cover the whole sequence subsumes the current bus seam rather than adding a second one.

Prior art to follow: the existing bus initialisation test, which injects counting closures and asserts both the number of calls and that a failure stops initialisation before dependent buses are touched.

What to assert at this seam:

- Expanders are initialised after buses and before drivers.
- Expander outputs are applied after expanders are initialised and before drivers load.
- An expander pin entry sets direction before level.
- A failed expander initialisation aborts before drivers load.
- A board profile with no expander sections behaves exactly as before, so existing boards are unaffected.
- Delays declared on expander output entries are honoured in order.
- Malformed and out-of-range entries are rejected without aborting the boot.

### Seam 2: power driver register decoding

Pure functions from register bytes to values: battery voltage from the charger's ADC register, charge state from its status register, variant from the part-number field, and the fuel gauge's command words to percentage, voltage and current. These cannot live behind seam 1 — different layer — so this is a second seam by necessity rather than preference.

Assert the decode arithmetic against values taken from the datasheets, both variants of the part-number field, the signed handling of fuel gauge current, and the documented behaviour that charge current latches its last value when the charger disconnects.

### Validation harness: the simulator

Not a test seam but the loop an agent uses to verify work without hardware. The simulator gains a `tdeck-max` device entry and a simulated XL9555 that records every write. Because the simulator runs real kernel code, this exercises the same boot sequence as hardware and makes the recorded write log assertable end to end. It overlaps seam 1 deliberately: seam 1 is the guard, the simulator is the confirmation.

### Extended: board catalogue validation

The existing catalogue validation CI job is extended to validate the new board profile sections, and to check embedded fallback profiles against their board profile files. No new CI job.

## Out of Scope

- **The A7682E 4G modem.** Cellular data is a subsystem, not a driver: it needs PPP, SIM handling, and a network HAL path that no board currently exercises from a board profile. Deferred to its own effort. Its power rail and power key are declared but left unasserted.
- **A haptic HAL interface and the DRV2605 driver.** Recorded as a TODO; a kernel ABI change deserving its own design.
- **Audio capture.** No HAL path exists. Playback only.
- **Modem audio routing.** The audio multiplexer is set statically to the codec. It becomes meaningful only when the modem is in scope.
- **A full IO-expander HAL interface.** The runtime setter is the minimum that lets a driver own its own line. The full interface arrives with the modem.
- **Board profile inheritance.** The Max duplicates what it shares with the Pro rather than extending it. Revisited when a third board in the lineage exists.
- **Recovery OS peripheral probing.** Recovery is catalogue-driven by design and deliberately avoids driving board-specific pins before a board is selected. Unchanged.
- **Fixing the T-Deck Pro beyond its power driver.** Other Pro discrepancies found while mapping the code are noted but not addressed.
- **The bus frequency field**, which is parsed and then ignored. A latent surprise, not this feature's problem.

## Further Notes

**Hardware facts were established from three independent sources** and agree: the board's own schematic, LilyGo's published board header, and an I²C bus scan captured from the physical device. The scan confirmed eight devices at the predicted addresses and, importantly, resolved which charger this unit carries — the schematic shows one part, the shipping unit has the other, and both exist in the wild.

**Two hardware risks worth knowing during bring-up**, neither blocking:

The modem's USB lines appear to share nets with the SoC's native USB and the USB-C connector. If that is literal rather than schematic shorthand, enumerating the modem over USB and using the native USB console are mutually exclusive. This only matters when the modem comes into scope, but it will present as a baffling USB failure rather than an error.

LoRa RF is routed out through the USB-C sideband pins, with an RF switch selecting internal versus external antenna. Antenna selection is genuinely software-controlled on this board — an unusual arrangement worth not being surprised by.

**One unverified reuse.** The touch controller responds at the expected address, but the Max ships with either of two parts at that address depending on revision, and LilyGo's own documentation contradicts itself about which. The existing touch driver targets one of them. Whether it works unmodified is a bring-up question to settle on hardware, not a design question.

**One pre-existing inconsistency to resolve during bring-up.** The IMU has both a C driver and a Rust driver in the tree, and the Rust one is a stub that returns not-supported. Which one links is not established. The IMU is in scope, so this needs settling.

**Milestones.** M1 is boots-and-usable: board profile, detection, display, keyboard, touch, WiFi/BLE, SD card, power. M2 completes the HAL surface: GNSS, LoRa, audio playback, IMU.
