# Drivers hold a shared SPI bus while their device is selected

The T-Deck Max's SPI host 2 carries the SD card, the e-paper and the LoRa radio. ESP-IDF runs one transfer at a time on a bus, but a chip select is part of a transfer only when the SPI peripheral drives it. The radio's and the e-paper's drivers drive theirs as GPIOs (`spics_io_num = -1`): RadioLib frames each command itself, and the e-paper sends a frame as 2 KB chunks under one CS. Neither held the bus between lowering CS and raising it, so a transfer to another device could run while either was selected, and reach it. The radio took the SD card's bytes for commands and refused LoRa sends with -706 (ticket 16); the e-paper would take them for pixels. A driver that drives its own CS now holds the bus with `spi_device_acquire_bus()` from before it lowers CS until after it raises it, as ESP-IDF's own SD card driver (`sdspi_host.c`) does.

## Considered Options

**Letting the SPI peripheral drive CS** (`spics_io_num` set to the pin) selects the device for each transfer and for nothing else, with no lock to take. It suits a driver whose every command is one transfer. It does not remove the lock for the others: keeping CS low across transfers takes `SPI_TRANS_CS_KEEP_ACTIVE`, which ESP-IDF refuses unless the device holds the bus. RadioLib drives CS itself through its HAL, and would have to be kept from doing so.

**A bus lock of the kernel's**, handed out with `hal_bus_get_spi()`, would let the kernel see who holds a bus. It duplicates the lock ESP-IDF keeps for each bus, and guards only against drivers that take it; ESP-IDF's SD card driver would not.

**A bus for each device** is not available: the board wires all three to one.

## Consequences

The SX1262 driver takes the bus in `ThistleHal::spiBeginTransaction()` and `spiEndTransaction()`, which RadioLib calls on either side of CS; the e-paper driver takes it in `epaper_send_cmd()` and `epaper_send_data()`. A full frame holds the bus for as long as it takes to send, about 40 ms at 2 MHz, and a LoRa command or an SD transfer waits that long at most.

Loadable drivers (`.drv.elf`) cannot do this yet. The syscall table exports `gpio_set_level()` and `spi_device_polling_transmit()`, but not `spi_device_acquire_bus()` or `spi_device_release_bus()`. Until it does, a loadable driver on a shared bus lets the peripheral drive its CS, one transfer per command. Exporting the two adds to the syscall table; whether to is not decided here.
