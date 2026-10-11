# LoRa messaging speaks MeshCore, and the kernel runs the node from boot

The Messenger's LoRa conversation used a broadcast format of its own — a random sender id, a length and the text — that no other device speaks. MeshCore had been vendored as a submodule with a shim to the HAL radio, but nothing ever started it. We made MeshCore the protocol: the kernel runs a MeshCore chat node from boot (`components/meshcore`, driven by `mesh_manager.rs`), and the Messenger's LoRa transport is that node — the Public channel every MeshCore node shares, and direct messages to contacts it has heard advertise.

## Considered Options

**Meshtastic** has the larger installed base, and is what an off-the-shelf LoRa handheld usually runs. Its firmware is GPL-3.0, which this project's no-GPL rule excludes, so speaking it would mean a clean-room implementation of its packet format and encryption — a far larger job than finishing a MeshCore integration that was mostly in place. MeshCore is MIT. A device running Meshtastic cannot talk to ThistleOS; it has to be flashed with MeshCore firmware.

**Keeping the homegrown format** was rejected: it reaches only other ThistleOS devices, has no routing through repeaters, and no end-to-end encryption.

**Running MeshCore inside an app**, as the older `thistle_mesh_init`/`loop` interface assumed, was rejected: the node would hear nothing while the app was closed, and MeshCore needs a node that listens all the time to learn contacts and routes. The kernel runs it instead, queues what it hears, and apps read from that queue.

## Consequences

A MeshCore node only hears peers on its frequency, bandwidth and spreading factor. The node takes MeshCore's own preset for the band the board's radio is matched for — EU/UK (Narrow) at 868 MHz, USA/Canada at 915 MHz — and `system.json`'s `mesh` section overrides any of it, but never moves the node outside the board's band.

The node keeps its identity and contacts in SPIFFS, because a new key on every boot would make it a stranger to every peer. Its clock only moves forward, from the latest time it is known to have used: nothing sets the system clock yet, and a peer ignores an advert no newer than the last one it had from the same node.
