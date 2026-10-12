# 11: Internal RAM

**What to build:** A T-Deck Max that boots with internal RAM to spare. Buffers that need none of internal RAM's properties live in PSRAM; stacks stay in internal RAM; loops that mostly wait share one worker instead of keeping a thread each; and the configuration stops spending internal RAM where PSRAM or flash would do.

**Blocked by:** none. Ticket 10 found the problem: a 4 KB task could not be created.

**Status:** in-review

## What it was

A temporary build logged the internal heap at each boot stage and then listed every task's stack. On the T-Deck Max, with WiFi and mobile data on, booted to the LVGL e-paper manager:

- **14 KB of internal RAM free** once booted, the largest block 5.6 KB. During boot it fell to 2.3 KB, while LVGL started.
- Static internal RAM was 247 KB of 342 KB. Of that, 118 KB was `.bss`: LVGL's memory pool alone was 64 KB, the Assistant's conversation 16 KB, the Vault 8 KB, and other UI state about 7 KB. Another 104 KB was code placed in internal RAM for speed.
- 16 tasks had 92 KB of stack, all internal. The main task kept 16 KB for a loop that only slept. MeshCore kept 20 KB, the GPS reader 4 KB, and the network coordinator 6 KB.
- At boot, WiFi took 47 KB, the drivers 44 KB (the e-paper's two frames 19 KB, audio's I2S DMA 6.6 KB), MeshCore 34 KB, and mobile data 15 KB.

## What changed

**Into PSRAM:**
- LVGL's pool, by a compile definition on the lvgl component (top-level `CMakeLists.txt`).
- The Assistant, Terminal, Messenger and WiFi Scanner state, and the LVGL widget table, by `EXT_RAM_BSS_ATTR`.
- The e-paper frames. They now go to the panel through a 2 KB DMA buffer.
- MeshCore's radio slots, hash tables, contacts and command queue.
- The GPS and Terminal UART receive buffers, which grew to 8 KB. Past 4 KB, the UART driver takes them from PSRAM.

The Vault's decrypted entries and key, and `msg_crypto`'s state, stay in internal RAM: PSRAM is a separate chip on an external bus.

**Configuration (`sdkconfig.defaults.esp32s3`):**
- WiFi and lwIP buffers try PSRAM first (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`). On its own that switched WiFi to 16 static TX and 16 static RX buffers, 35 KB more internal RAM. They are pinned at 8 TX and 10 RX, and a frame with no TX buffer waits in a PSRAM queue.
- TLS sessions, the NimBLE host's pools and the NVS cache go to PSRAM.
- WiFi's IRAM speed-ups are off. The BLE controller runs from flash: it is off unless Bluetooth is switched on, and it can drop a connection while flash is erased. Ringbuffer functions run from flash. Together these moved about 35 KB of code out of internal RAM.

**Fewer stacks:**
- The kernel worker (`kworker.rs`, `thistle/kworker.h`) runs jobs, one at a time, on one 16 KB internal stack. MeshCore's loop is a job every 10 ms; its deepest point, the crypto self-check, leaves 5.3 KB of the stack unused. The GPS reader is a job every 100 ms.
- `app_main` returns after boot instead of sleeping in `kernel_run()`, which is gone, so its 16 KB is freed.
- The network coordinator's attempts, power-downs and WiFi rejoins run on its own 6 KB internal stack, one at a time. Before, each attempt had a thread with an 8 KB PSRAM stack. A connect used 3.4 KB of the stack. Switching mobile data off mid-attempt goes through a new optional `cancel` entry in `hal_net_driver_t`. Measured: the attempt ended 0.4 s after the power-on step it was in, and the module was off 3 s later. The cost is that WiFi is rejoined only once an attempt ends, up to two minutes later. Traffic has the cellular link meanwhile.
- The Terminal reads its UART from an `lv_timer`, not a 4 KB task. That also ends a race: stopping the old task did not wait for it to finish.
- Threads the kernel starts are named in task lists (`thistle_pthread_name_next`).

**Found on the way:**
- `driver_loader.rs` and `elf_loader.rs` asked for `MALLOC_CAP_SPIRAM` as `1 << 9`, which is `MALLOC_CAP_PID7`. No heap has that capability, so every `.drv.elf` and `.app.elf` load failed for want of memory. Both now use `1 << 10`.
- `app_manager.rs` called `1 << 3` (`MALLOC_CAP_DMA`) `MALLOC_CAP_DEFAULT`. The app eviction threshold was in effect measuring internal RAM, which is the right thing to measure, so it now does so by name.

## Result

| | Before | After |
| --- | --- | --- |
| Internal RAM free once booted | 14 KB | about 180 KB |
| Largest free block | 5.6 KB | 88 KB |
| Lowest during boot | 2.3 KB | 146 KB |
| Static internal RAM | 247 KB | 124 KB |
| Tasks, and their stacks | 16, 92 KB | 14, 66 KB |

The clean build logs `ThistleOS ready; internal RAM 166 KB free, largest block 88 KB`, with `app_main`'s 16 KB still counted. WiFi moved 133-139 KB/s down and 113-119 KB/s up to speedtest.tele2.net. GPS, MeshCore (an advert on air), WiFi, and mobile data at boot all worked. Host tests pass: Rust (1653, six of them new for the worker), the modem, bus, e-paper and radio C tests, and the simulator's unit, boot and app-matrix tests (102 cases).

## Still to see

- **LTE throughput and mobile data off/on cycles, on this build.** Partway through testing, the modem started answering `+CPIN: SIM REMOVED`. With `AT+CFUN=1` the ICCID still could not be read; the module was on SIM slot 1 (`AT+SWITCHSIM?` 0) with hot-swap detection off. The previous build had connected on the same SIM, and nothing on the SoC side reaches the SIM interface, so the card looks unseated. Mobile data at boot connected with the inline coordinator before that.
- A received LoRa packet on the worker. None arrived during testing, so MeshCore's receive path has not run on it; the worker warns once if a job leaves less than 4 KB of its stack.
- MeshCore's 16 pooled packets (about 5 KB) are still in internal RAM: the pool allocates them inside the MeshCore submodule.
- `SPIRAM_MALLOC_ALWAYSINTERNAL` (4 KB) is unchanged. Lowering it would move every small allocation to PSRAM. That would be a large saving, but it would also apply to code nobody here has checked for DMA or interrupt use.
