# 04: Hardware detection distinguishes a T-Deck Max from a T-Deck Pro

**What to build:** Flash a T-Deck Max with no board profile present and have it identify itself correctly, instead of writing the T-Deck Pro's profile and booting as a Pro.

This is the bug that makes the Max worse than unsupported today. The Pro's fingerprint is a keyboard controller plus an IMU on a given pin pair — a signature the Max matches exactly, because the Max is the Pro's third hardware revision and shares both parts on the same pins. The result is a board that comes up confidently wrong, with no error.

The IO expander is the discriminator: it is absent from every other board in the catalogue, and is the marker the vendor themselves document for telling these boards apart. The Pro's rule is also narrowed so that an ambiguous match declines to guess rather than guessing wrong — a detector that returns "unidentified" is more useful than one that returns the wrong board.

Detection writes an embedded fallback profile, so the Max needs one. These embedded profiles duplicate the board profile files and two have already drifted from their originals with nothing catching it. Since the embedded profile is the only configuration a freshly flashed board ever sees, drift there is a first-boot failure.

**Blocked by:** 03 (T-Deck Max boots to a usable system).

**Status:** in-review

- [x] The IO expander's address is added to the probe set
- [x] A Max fingerprint rule matches before the Pro rule
- [x] The Pro's fingerprint is narrowed so a keyboard controller plus IMU alone no longer resolves to a Pro
- [x] An ambiguous match resolves to unidentified rather than to a guessed board
- [x] An embedded fallback profile exists for the Max
- [x] A freshly flashed Max with no profile present boots as a Max
- [x] A T-Deck Pro still detects correctly and is unaffected
- [x] CI asserts that every embedded fallback profile matches its board profile file
- [x] The two existing drifts between embedded profiles and their files are resolved

## Comments

The matching rules and the profiles behind them moved out of
`components/kernel/src/board_fallback.c` into `components/kernel/src/board_fingerprint.c`,
which depends on nothing but `stdbool`/`stddef`. `board_fallback.c` keeps the I²C
probing and the write to SPIFFS. That split is what makes the rules assertable:
they used to be compiled out of every test build, and the acceptance criteria here
are all statements about which board a given scan names. The tests are
`simulator/tests/test_board_fingerprint.c`, run by the existing simulator unit-test
job in CI.

**The Max rule is keyed on the expander alone**, as the spec asks, and not on the
expander plus the parts the Pro also carries. Ticket 09 records that the Max's IMU
sits on a 1.8 V rail behind this very expander, and detection runs long before
anything has driven it — so a rule requiring the IMU would fail on exactly the
board it was written for, and the Max would come up unidentified. The touch
controller's reset moves to an expander pin too. `fingerprint_a_max_is_recognised_before_its_rails_are_raised`
holds this: the scan it is given has nothing on it but the expander.

**How the Pro's rule was narrowed.** There is no positive I²C marker the Pro has
and the Max lacks — the Max is the Pro's third revision and carries the same
keyboard controller, IMU and touch controller at the same addresses. So the
narrowing is two extra conditions rather than an extra device: the Pro's rule now
requires the expander's *absence* explicitly, and requires the SDA 13 / SCL 14 pin
pair. Writing the expander's absence into the rule rather than relying on the Max
rule being tried first is deliberate: a Max whose expander did not answer then
comes up unidentified instead of confidently as a Pro, and the rules stay correct
if someone reorders them.

**What else became ambiguous.** Where boards share a part, the rule for each now
names its pin pair — which is what the glossary's definition of a fingerprint
already says it is. That resolves three places where detection was guessing:

- A TCA8418 answering on its own resolved to a T-Deck whatever pins it was found
  on. On the Pro's pins that is a Pro or a Max whose other parts stayed quiet, so
  the T-Deck rule now names SDA 18 / SCL 8. A T-Deck whose touch controller does
  not answer still detects, as it did before.
- An SSD1306 on any pin pair other than Heltec's resolved to a T3-S3. Both boards
  now require their own pin pair.
- Devices answered but no rule claimed them used to write the RAK3312 profile,
  the same profile used for "no I²C devices at all". Only the genuinely silent
  case assumes headless now; the unclaimed case writes nothing and returns
  `ESP_ERR_NOT_FOUND`, which `load_config` already handles by falling through to
  safe mode.

No board already in the catalogue loses detection: the only pin pairs probed are
the six in `I2C_PIN_COMBOS`, and every board reachable on one of them still
matches its own rule. `c3-mini`, `cyd-*`, `twatch-ultra` and the Waveshare board
sit on pin pairs that were never probed, before or after.

**Drift.** Both known drifts are fixed (`tdeck` was missing its power driver,
`tdeck-pro` its `gpio_outputs`), and `tools/validate_board_catalog.py` now reads
the embedded copies out of the C source and compares each against its board
profile file. Each copy also carries the board id it is for, so a match can be
logged and asserted by name; the validator checks that label against the id inside
the JSON too.

**One thing outside the ticket.** `recovery/src/recovery_web.rs` listed every board
in the catalogue except `tdeck-max`, so the Max could not be picked during recovery
provisioning either. Added.

**Not verified on hardware.** No T-Deck Max or T-Deck Pro was available. The
firmware build was not run either — ESP-IDF is not installed in this environment,
so `board_fallback.c`'s compile is left to CI. `board_fingerprint.c` itself compiles
and its tests pass under the host toolchain. The probing half stays compiled out of
test builds: that it probes `BOARD_ADDR_XL9555` is readable in source and nothing
else.

**Recorded, not done.** The embedded fallback profiles remain duplicates of the
board profiles they copy, policed by CI, because that is what the spec asks for
("A CI check asserts that each embedded fallback profile matches its corresponding
board profile file"). ESP-IDF's `EMBED_TXTFILES`, or `include_str!` if this moved
to `kernel_rs`, would make drift impossible rather than merely detectable, and
would delete both the copies and the validator code that guards them. Worth
revisiting when something else forces this file open — it is also the obvious
moment to move the matching rules into `kernel_rs`, where the architecture says
kernel logic belongs and where the existing 1399-test suite runs.
