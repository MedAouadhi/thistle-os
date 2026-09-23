# 07: LoRa on the T-Deck Max at 868 MHz

**What to build:** The board transmits and receives over LoRa, and long-range radio apps work.

The radio is the same part at the same pins as the T-Deck Pro, so the driver and its pin configuration are reused unchanged. What is new is power and antenna: the radio's rail sits behind the IO expander and must be raised before the driver probes, and this board has a software-controlled RF switch selecting between internal and external antenna paths. That selection must be set to a defined state at boot, or transmission may be routed to an unconnected port.

**Blocked by:** 03 (T-Deck Max boots to a usable system).

**Status:** in-review

- [x] The LoRa power rail is raised via an expander pin at boot, before drivers load
- [x] The antenna selection is set to a defined state at boot
- [x] A driver entry declares the radio at 868 MHz
- [x] The board transmits and receives

## Comments

Two things the ticket did not anticipate turned up on the bench, both found by
instrumenting `sx1262_init` on hardware rather than by reading code.

**The module needs its oscillator and its own RF switch.** The S62F carries an
embedded 32 MHz TCXO (`hardware/S62F_Lora.pdf`) run off the transceiver's DIO3,
and uses DIO2 to drive its internal transmit/receive switch — both of which the
vendor's own sketches set (`examples/LoRa_sx1262/lora_send/lora_send.ino`:
`setTCXO(2.4)`, `setDio2AsRfSwitch()`). Neither is a board property, but only a
board profile knows which module is soldered on, so both became driver config
(`tcxo_mv`, `dio2_rf_switch`) that stays off unless a profile asks. This is
separate from the antenna switch the ticket names: that one is in front of the
module, on the expander.

**DIO1 carries TxDone as well as RxDone.** The receive task runs at
`configMAX_PRIORITIES - 1`, so on a TxDone it preempted the sender, read a
packet nobody had sent, and cleared the interrupt the sender was waiting on —
so every transmission returned `RADIOLIB_ERR_TX_TIMEOUT`. The task now passes by
unless `receiving` is set, which a send clears before it starts. This was a
latent bug on every board with this radio, not just the Max.

Verified on hardware: `transmit=0 in 46914us` against a 36096us calculated
time-on-air, with `startReceive=0`. Before the fix, the same probe returned
`-5` with `irq=0x00000000 dio1=0 errors=0x0000` two seconds after a
`startTransmit` that had itself succeeded.

Not isolated: the hardware run that passed carried both changes, so which one
cleared the timeout is unproven. Both are required by the module datasheet and
the vendor firmware, so both stay.

**A send used to leave the radio deaf.** Transmitting ends in standby, and
nothing put the caller back on air, so a messenger heard the first message and
nothing after its own first reply. `sx1262_send` now restores listening if the
caller had it on. This predates the ticket — the old code reached the same
silence by a different route — but "the board transmits and receives" is this
ticket's fourth line, so it is fixed here.

### Follow-up: `heltec-v3` declares a TCXO in a spelling nothing reads

`heltec-v3.json` carries `"tcxo": true` on its radio entry. Nothing has ever
read that key, so that board has never had `setTCXO` called and may be running
its radio the way the Max was. This ticket added `tcxo_mv`, which *is* read, so
there are now two spellings and only one of them works.

Left alone deliberately: the voltage is not written down anywhere on disk, and
the repo's hardware-authority rule is not to infer a part's numbers from a
plausible-looking source for a different board. It wants its own ticket, by
someone who can put a Heltec V3 on a bench.
