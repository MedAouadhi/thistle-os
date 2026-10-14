# ThistleOS

A portable ESP32-family operating system with an immutable kernel and loadable drivers, apps and window managers. This glossary fixes the vocabulary for hardware configuration and the driver layer, where several terms have drifted.

## Language

### Boards and configuration

**Board profile**:
The JSON document describing one board's hardware — its buses, pin assignments, and the drivers to load. Exists as a file in the board catalogue and as a copy embedded in the kernel.
_Avoid_: board config, board definition, board.json, board file

**Board id**:
The catalogue identifier for a board, always lowercase and hyphenated. The board profile's filename stem must equal it.
_Avoid_: board name, board slug

**Board name**:
The human-readable label for a board, distinct from its board id. Boards frequently self-report a name that differs from both.

**Embedded fallback profile**:
A board profile duplicated as a string literal inside the kernel, written to storage when hardware detection identifies a board with no profile present. The only configuration a freshly flashed board ever sees.
_Avoid_: fallback JSON, builtin board

**Fingerprint**:
The set of I²C addresses whose presence on a given pin pair identifies a board. Detection matches fingerprints in order, first match winning.
_Avoid_: signature, probe result

### Buses and pins

**Bus index**:
The position of a bus in the board profile's declared bus array, and the value drivers use to request a bus. Distinct from the hardware host or port number, which appears inside the bus declaration and is not what drivers reference.
_Avoid_: bus number, host, port

**Bus handle**:
The opaque token a driver receives when it requests a bus by index, and through which it performs transfers.

**Expander pin**:
A control line on an IO expander rather than on the SoC. Addressed by expander index and pin number, never by a GPIO number, and reachable only after the bus carrying its expander is up.
_Avoid_: virtual GPIO, extended GPIO, expander GPIO

### Drivers

**HAL interface**:
One of the vtable contracts a driver can implement — display, input, radio, GPS, audio, power, IMU, storage, network, crypto, RTC. A board populates some subset; absent hardware leaves an interface unregistered rather than stubbed.
_Avoid_: HAL driver, HAL type, HAL vtable

**Driver**:
An implementation of a HAL interface for a specific part, loaded from storage as an ELF or compiled into the kernel. Named for the part it drives, not the board it appears on.

**Driver entry**:
A driver's declaration inside a board profile — its identifier, the HAL interface it registers, its ELF filename, and its part-specific configuration.

**Variant**:
Two parts that are register-compatible enough to share one driver, distinguished at runtime by probing. A variant is not a separate driver.

### Time

**Time source**:
Anything that can tell the kernel the time: a time server (NTP), the GPS receiver, the mobile network (NITZ), a clock chip, or the user with automatic time off. Ranked in that order, the user aside; the one that last set or confirmed the clock is what Settings names.
_Avoid_: clock source, time provider, sync source

**Clock chip**:
A battery-backed real-time clock on the board, such as a PCF8563, registered as the RTC HAL interface. Distinct from the SoC's own RTC domain, which keeps the time through sleep and restarts but not through a power-on.
_Avoid_: RTC (ambiguous between the two), hardware clock
