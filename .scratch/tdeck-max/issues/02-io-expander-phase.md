# 02: IO expander phase in board profiles

**What to build:** Board profiles gain the ability to declare IO expanders and the pin states to apply to them, so a board whose peripheral power rails sit behind an I²C expander can be brought up from configuration alone, with no kernel recompilation.

The boot sequence becomes: board-level GPIO outputs, buses, **IO expanders**, **expander outputs**, drivers. Expanders are initialised after buses because they sit on a bus; their pins are driven before drivers load because those pins carry the rails that drivers probe for.

Direction is declared explicitly per pin, not inferred. The XL9555's configuration registers reset to all-inputs, so an entry that sets a level without setting direction leaves the pin floating and the rail off — a board that silently fails to power up rather than a visible error. The schema shape is in the spec.

See ADR-0001 for why expander pins are a declared phase rather than GPIO numbers with a high bit set, which is the approach the vendor's own header takes.

**Blocked by:** 01 (Extract the board profile boot sequence behind injected effects).

**Status:** in-review

- [x] Board profiles may declare IO expanders and expander outputs; both sections are optional and bounded like the existing bus and GPIO output arrays
- [x] An expander output entry addresses its expander by index into the declared array, mirroring how driver entries reference a bus index rather than a host or port number
- [x] Expanders are initialised after buses and before drivers load
- [x] Each expander output entry sets pin direction before setting level
- [x] A declared expander that fails to initialise aborts boot before drivers load, matching the existing behaviour for a failed bus
- [x] A board profile with no expander sections behaves exactly as before, verified against existing boards
- [x] Declared delays on expander output entries are honoured in order
- [x] An XL9555 driver drives expander pins over I²C, using the standard eight-register layout, polling only — its interrupt is not routed to the SoC on this hardware
- [x] The board catalogue validator rejects malformed entries, including a missing direction and an out-of-range pin or expander index
- [ ] The simulator provides a fake expander that records every write, and a test asserts boot ordering from the recorded log

## Comments

The last item is half done. `simulator/devices/dev_xl9555.c` exists and records every
register write in order, and `simulator/tests/test_xl9555.c` covers what the log
guarantees — writes land in order with the right register and value, the input ports
are read-only, a pin write leaves the rest of its port alone, and a driven pin reads
back at the level it is held at.

What is *not* delivered is a test that asserts the ordering **the kernel** produced.
The simulator never reaches the board profile boot sequence: `board_config_init` looks
for `/spiffs/config/board.json`, which does not exist under the simulator, so
`board_init()` in `simulator/board_simulator.c` runs instead and registers HAL
drivers directly. There is no simulator device that boots from a board profile, and
no `tdeck-max` device entry to register the expander against — both arrive with
ticket 03. Writing the ordering assertion now would only have asserted that the test's
own helper writes the configuration register before the output register.

Boot ordering is therefore guarded by seam 1 for now, in
`components/kernel_rs/src/board_config.rs`, where a recorder is injected and the phase
order asserted directly. Ticket 03 should register `dev_xl9555_register()` in
`board_simulator.c` for the `tdeck-max` device and close this item end to end.

Two decisions made during implementation, both narrower than the ticket allowed:

- `direction` accepts only `"output"`. The schema requires the field so that omitting
  it is a validation error rather than a rail that silently stays down; nothing in the
  spec asks for expander pins configured as inputs, so that is not supported and the
  driver has no pin-read path.
- A *malformed* expander declaration (no id, no address) does not stop the boot — it
  keeps its index and is marked unaddressable, so pins naming it are dropped while
  later expanders keep their declared indices. Only a declared expander that cannot be
  *reached* aborts before drivers load, which is what the ticket asks for.

### After ticket 03

Ticket 03 registered `dev_xl9555_register()` for the new `tdeck-max` simulator
device, so the expander now exists on the simulator's virtual bus. The open item
is still not closed end to end, and for the reason above rather than a new one:
the simulator does not run the board profile boot sequence at all.
`board_config_init` looks for `/spiffs/config/board.json`, which no simulator
device provides, so `board_init()` in `simulator/board_simulator.c` registers HAL
drivers directly and `run_boot_sequence` never executes.

Closing it needs a `BootEffects` implementation backed by the simulator's virtual
buses, and a simulator device that boots from a profile rather than from
compiled-in registration. That is a piece of work in its own right, not a rider
on a board bring-up. Until then the ordering is guarded at seam 1 in
`components/kernel_rs/src/board_config.rs`, which ticket 03 extended to assert
against the shipped `tdeck-max.json` itself.
