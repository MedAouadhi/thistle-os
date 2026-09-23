# 09: IMU on the T-Deck Max

**What to build:** Orientation data is available, and orientation-aware apps work.

The IMU is the same part the T-Deck Pro carries and sits on a 1.8 V rail that is now behind the IO expander, so the rail must be raised before the driver probes.

One thing needs resolving first: there are two drivers for this part in the tree, one C and one Rust, and the Rust one is a stub that returns not-supported. Which of them actually links is not established. Settle that before wiring the driver entry, or the board will appear to have an IMU that never returns data.

**Blocked by:** 03 (T-Deck Max boots to a usable system).

**Status:** ready-for-agent

- [ ] The IMU's 1.8 V rail is raised via an expander pin at boot, before drivers load
- [ ] It is established which of the two IMU drivers in the tree links, and the stub is not the one serving this board
- [ ] A driver entry declares the IMU
- [ ] Orientation data is available through the IMU HAL interface
- [ ] A TODO is recorded that the board's haptic driver has no HAL interface to register against — adding one is a kernel ABI change deserving its own design, and would serve two boards since the Pro's later revision carries the same part
