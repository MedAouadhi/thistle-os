# 02: LoRa can be switched off

**What to build:** The MeshCore node can be stopped and started again while the device runs. Off, the radio sleeps. `rs_mesh_set_enabled` saves `thistle_os.mesh.enabled` and starts or stops the node; a node switched off at boot leaves the radio asleep.

**Blocked by:** none.

**Status:** in-review; stop and resume seen on hardware, a message received after them not yet

- [x] `meshcore_stop()`: cancels the node's kernel-worker job, stops receiving, sleeps the radio
- [x] `meshcore_start()` after a stop wakes the radio, retunes it and listens, without loading the identity or contacts a second time
- [x] `rs_mesh_set_enabled(bool)`, `rs_mesh_is_enabled()`
- [x] Off at boot: radio asleep
- [x] Verified on hardware: off, then on, then an advert sent
- [ ] Verified on hardware: a message received after a resume

## Comments

### Done

`meshcore_stop()` cancels the node's kworker job (which waits out a run in progress, a LoRa send at worst), drops queued commands, saves contacts that were waiting for `tick()`'s delayed save, then `stop_receive` and `sleep(true)`. A later `meshcore_start()` wakes the radio (`sleep(false)`, RadioLib's standby), retunes it, and the job's first run calls `resume_node()` in place of `start_node()`: `begin_node()` would add the Public channel a second time. The resume calls `on_ready` again, which is what sets `rs_mesh_is_running()`.

`rs_mesh_set_enabled(true)` on a node that will not start leaves it off and unsaved; in the simulator, which cannot run MeshCore, that is every time ("LoRa did not start").

`mesh_start()` with the mesh switched off now puts the radio to sleep: before, the SX1262 sat in standby from boot.

The light-sleep wake list still names LoRa DIO1. A sleeping radio raises nothing on it, so it costs nothing, but it is worth seeing that the board still sleeps with LoRa off.

### On the hardware

A throwaway boot hook (since removed) switched the mesh off 25 s after boot and on again 8 s later, on the T-Deck Max:

```
QPTEST before: enabled=true running=true
I (31129) meshcore: off air; radio asleep
mesh: switched off
QPTEST off: rc=0 in 16 ms, enabled=false running=false
mesh: switched on
I (39178) meshcore: back on air as ThistleOS-11F7
QPTEST on: rc=0 enabled=true
QPTEST after: running=true advert=0
I (42241) meshcore: advertising as ThistleOS-11F7
I (43004) meshcore: tx advert, flood, 117 bytes
```

So the job stopped at once, the radio woke from sleep, took its settings and transmitted. It left `"mesh": { "enabled": true }` in the board's system.json, which is what an absent setting means.
