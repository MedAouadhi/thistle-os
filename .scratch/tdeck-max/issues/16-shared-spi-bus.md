# 16: LoRa sends fail with -706 while the SD card is written

**What to build:** A LoRa transmission goes out on its first attempt, whatever else is using the SPI bus at the time.

**Blocked by:** none. Made frequent by 14, which writes every message sent to the SD card as it is sent.

**Status:** in-review

- [x] The SX1262 driver holds the bus while the radio is selected (`ThistleHal::spiBeginTransaction`/`spiEndTransaction`)
- [x] The receive task and a sender take turns at the radio (`RadioLock`), and the task reads only a packet the radio says it received (`RADIOLIB_SX126X_IRQ_RX_DONE`)
- [x] The e-paper driver holds the bus while the panel is selected
- [x] Host tests for the two radio races, each seen to fail with its fix removed
- [x] Verified on hardware

## Comments

### What the log showed

A reply to a MeshCore contact failed twice before it went:

```
I (335200) messenger_ui: queued message id=8 on MeshCore
I (335210) meshcore: message 1 to awax, attempt 1, flood
E (335213) sx1262: TX failed: -706
...
E (341197) sx1262: TX failed: -706
I (347136) meshcore: message 1 to awax, attempt 3, flood
I (348084) meshcore: message 1 delivered
```

-706 is `RADIOLIB_ERR_SPI_CMD_INVALID`: the radio's status byte said it had been sent a command it could not make sense of.

### Why

The radio, the SD card and the e-paper share SPI host 2 (MOSI 33, MISO 47, SCLK 36). The SD card's driver (`sdspi_host.c`) holds the bus with `spi_device_acquire_bus()` from before it lowers its chip select until after it raises it. The radio's and the e-paper's chip selects are GPIOs their drivers drive themselves (`spics_io_num = -1`), and neither held the bus. RadioLib lowers the radio's CS and then calls `spiTransfer`, which waits for the bus; if the SD card has it, the radio is selected while the SD card's bytes go by, and takes them for a command. Sending a message to MeshCore also appends it to `/sdcard/messages/mesh/` (`record_sent`, from the Messenger's task), 10 ms before the kernel worker transmits it.

A command that makes no sense is refused with -706. One that happens to make sense is carried out. While selected, the radio also drives MISO against the SD card.

The receive task (`radio_irq`) and the sender (the mesh, on the kernel worker) could also talk to the radio at once. With no affinity, the two can run on different cores, and RadioLib has no lock. Two `spi_device_polling_transmit` calls on one device at once fail, and the driver ignored the failure, so RadioLib parsed an unread buffer as the status.

The e-paper keeps its CS low across a frame's 2 KB chunks; a transfer to another device between two chunks reached the panel as pixels.

ADR-0006 records the rule both drivers now follow, what else was considered, and that loadable drivers cannot follow it yet.

### On the hardware

Before: 2 of the 3 attempts at one message failed. After: two sends, each with the SD write in the same 10 ms before it, both went out on the first attempt and were acknowledged; a message received during the same session read normally. No `SPI transfer failed` in the log.
