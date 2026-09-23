# IO expander pins are declared as a board profile phase, not encoded as virtual GPIOs

The T-Deck Max puts eleven peripheral control lines — including all four peripheral power rails — behind an XL9555 I²C IO expander, which the boot sequence cannot express: board-level GPIO outputs are applied before buses are initialised, so the component that powers the peripherals is itself a peripheral on a bus that does not yet exist. We added an `io_expanders` / `expander_outputs` phase to the board profile schema, initialised after buses and before drivers, plus a single runtime syscall for setting an expander pin, rather than adopting LilyGo's virtual-GPIO encoding where an expander pin is passed as an ordinary pin number with a high bit set.

## Considered Options

**Virtual GPIO encoding** (LilyGo's approach, visible in their published board header as `XL9555_GPIO(pin) = 0x100 + pin`) was rejected despite being the obvious path and the one a reader will find in the vendor's own code. It is cheap in the board profile, but it makes every driver's pin handling expander-aware and hides an I²C transaction behind a value that all existing code treats as a plain integer. The first time such a "GPIO" is toggled from an interrupt handler, it fails in a way that is hard to diagnose. It also requires initialising the expander before board-level GPIO outputs, inverting the existing boot order for every board rather than adding a phase to it.

**A full IO-expander HAL interface** was rejected as premature. It is the right long-term shape, but it is a kernel ABI change — the registry structure, the syscall table, and every consumer — and only two lines on this board genuinely need runtime control under the current scope.

## Consequences

The runtime syscall is deliberately minimal: one setter, no HAL vtable. It exists because the speaker amplifier must be gated by its own driver rather than left permanently enabled by a board profile.

It is also the seam from which the full HAL interface will grow. The 4G modem, currently out of scope, needs its power key *pulsed* by its driver rather than set once at boot, which the declarative phase cannot express. When the modem is implemented, the expected evolution is to promote the setter into a proper interface, with the declarative phase retained for boot-time rail configuration.
