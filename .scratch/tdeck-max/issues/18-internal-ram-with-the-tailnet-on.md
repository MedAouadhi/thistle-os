# 18: Internal RAM with the mesh and the tailnet on

**What to build:** Nothing until internal RAM is actually short. Then, smaller MicroLink task stacks, sized from measurements under load. Until then this ticket records why a T-Deck Max logs `ThistleOS ready; internal RAM 83 KB free, largest block 31 KB` where earlier boots said 134 to 150 KB, and where the tailnet's internal RAM goes.

**Blocked by:** none. Follows ticket 11 (internal RAM) and `.scratch/tailscale/spec.md`.

**Status:** deferred

## What it was

Nothing leaks, and the To-do app is not the cause. The ready line reads 83 KB whenever the mesh and the tailnet are on and WiFi joins during the boot; with both off, the same firmware reads 140 KB at the same point.

A temporary build (`../measure/ram-report.patch`) reported at the ready line, then 20 s and 60 s later: every task, the internal heap block holding its stack and the bytes of it never touched, and every other used block of the internal heap (`heap_caps_walk`), by size. Firmware: 45be548 plus the To-do app and 32 app slots. The T-Deck Max on WiFi, with no SIM, under the LVGL e-paper manager. At the ready line:

| | Mesh and tailnet off | Both on (3 boots) |
| --- | --- | --- |
| Internal RAM free | 140 KB (144,179 B) | 82-83 KB (84,975 B) |
| Largest free block | 62 KB | 31 KB |
| Tasks, and their stacks | 14, 80,896 B | 18, 123,904 B |
| Other used blocks | 544, 114,144 B | 629, 128,524 B |

The 59,204 B between the two:

- **43,008 B: MicroLink's four task stacks**, fixed by `#define` in its `include/microlink_internal.h:44-56`:

  | Task | Stack | Never touched by the ready line |
  | --- | --- | --- |
  | `ml_derp_tx` | 14,336 | 10,800 |
  | `ml_coord` | 12,288 | 5,128 |
  | `ml_net_io` | 8,192 | 5,272 |
  | `ml_wg_mgr` | 8,192 | 5,456 |
  | Total | 43,008 | 26,656 |

  The same 60 s later.
- **1,408 B:** their four task control blocks (352 B each).
- **14,380 B:** heap blocks that the tailnet and the mesh hold between them; this run did not split them. Most of it is 22 more blocks of 257 to 512 B (19 of them 272 B), one 3 KB block that reads as random bytes (keys, or a hash table), and one more 1,728 B block.
- **408 B:** the heap's own headers for those blocks.

The kernel worker's stack costs the mesh nothing extra: MeshCore runs on it, and its crypto check leaves 5,392 B of it untouched (12,684 B with the mesh off).

## Why the ready line moves

- **The tailnet's tasks start when WiFi gets an address**, not when the tailnet connects (`microlink: All tasks started`, 12.9 s). The boot waits up to 10 s for the saved network (`wifi_manager_connect(..., 10000)` in `wifi_manager.rs`'s auto-connect), then 5 s more (`main.c`, before `ota_mark_valid()`), then logs the ready line. A boot whose WiFi joins in time, as every boot on 2026-10-20 did, counts the tailnet. On 2026-10-18 the tailnet started only at 31 s, so WiFi had not joined in time: the ready line said 136 KB, and the tailnet's own line, later, 104 KB.
- **The ready line counts `app_main`'s own stack**, a 17,408 B block, freed a moment later. One boot logged the ready line at 83 KB at 18.45 s, `app_main` returned at once, and the tailnet's `connected as ...; internal RAM 100 KB free` came at 19.0 s.
- **Settled, with both on: about 107 KB.** That is the 60 s report (91,919 B, `app_main` still waiting) plus `app_main`'s stack and control block. Between the 20 s and 60 s reports 6,824 B came back as the modem powered off, for want of a SIM; with a SIM the modem stays on, and about 100 KB is what is left.

## Readings over time

The ready line in earlier sessions' captures, by the date of the session that took them. The tailnet arrived on 2026-10-15 (089da1e).

| Date | Tailnet not started by the ready line | Started by then |
| --- | --- | --- |
| 10-12 | 152-166 KB (166 is ticket 11's build) | |
| 10-13 | 148-159 KB | |
| 10-14 | 138-150 KB | |
| 10-15 | 145 KB | |
| 10-16 | 141 KB | 92 KB |
| 10-17 | 135-143 KB | 91 KB |
| 10-18 | 134-136 KB | 91 KB |
| 10-20 | 140 KB (mesh off too) | 82-83 KB |

The "100 and something" figures were the tailnet's own `connected as` line, logged once `app_main` had returned: 100-110 KB from 10-16 to 10-18, 100 KB on 10-20.

About 9 KB went between 10-17 and 10-20 (91-92 to 82-83 KB with the tailnet up), most of it before the To-do change (143 to 134 KB without the tailnet by the evening of 10-18). It was not traced commit by commit. Free internal RAM before WiFi and the drivers start (`tk_render: ... free_before=`) was 290,091 B on 10-14 and 281,723 B on 10-20: 8.2 KB less, which points at static internal RAM or early allocations rather than anything at run time. `idf.py size` puts static internal RAM (DIRAM) at 145 KB: 75 KB of code, 42 KB of `.bss`, 28 KB of `.data`. Ticket 11 left it at 124 KB, if that was measured the same way.

The To-do app and the 12 more app slots take 453 B of static internal RAM (261 B of `libtodo.a`, 192 B of slots), and nothing at run time until To-do opens; its list is in PSRAM.

## The rest of internal RAM, with both off

For whoever trims next. At the ready line:

- **Stacks:** `main` 17,408 (freed after boot), `kworker` 16,384, `lvgl` 8,192, `wifi` 6,656, `net_coord` 6,144, `sys_evt` 4,608, `uart_task` 4,096 (the modem's, gone once it powers off), `radio_irq` 4,096, `esp_timer` 4,096, `tiT` 3,584, two idle tasks of 1,536 and two IPC tasks of 1,280. Every stack is in the internal heap.
- **A 32 KB block that is not in use:** ESP-IDF's DMA reserve pool (`SPIRAM_MALLOC_RESERVE_INTERNAL`). `esp_psram_extram_reserve_dma_pool()` takes it from the heap and adds it back as a heap of its own, with the capabilities `DMA|INTERNAL` and `8BIT|32BIT` but not `DEFAULT`. So the walk counts it as used, `heap_caps_get_free_size(INTERNAL|8BIT)` counts its free space as free, and `malloc()` cannot use it, though a task's stack can. The heap's total, 349,963 B, is the regions `heap_init` lists plus these 32 KB again, to within 0.5 KB.
- **Repeated blocks:** 10 of 1,664 B that start `ef be ad de`, which fits WiFi's 10 static receive buffers (`ESP_WIFI_STATIC_RX_BUFFER_NUM`, 1,600 B each); 9 of 1,728 B, 12 zeroed blocks of 960 B, 102 of 84 B and 207 of 12 B, none of them identified.

## Running it again

```sh
git apply .scratch/tdeck-max/measure/ram-report.patch
idf.py build
tools/check_serial_port.sh /dev/ttyACM0 && idf.py -p /dev/ttyACM0 flash
python3 tools/serial_capture.py ram.log --seconds 105
grep ramdiag ram.log
git apply -R .scratch/tdeck-max/measure/ram-report.patch   # then build and flash again
```

The patch keeps `app_main` 60 s longer, so its 17 KB stack is counted in all three reports. It takes its task list with `uxTaskGetSnapshotAll()`, which takes no lock, so it is for a diagnostic build only. The captures are not kept here: they hold the tailnet's peer names and addresses, and the WiFi network's name.

## Still to see

- **MicroLink's stacks.** 26 KB of the 42 KB was untouched by the ready line, and `ml_derp_tx` used 3.5 KB of its 14 KB. One quiet boot gives what the tasks need at least, not at most. Before cutting, take the high-water marks under a Hermes session, a direct path, `tailscale ping`, a sign-in with the QR code, mobile data, a switch between WiFi and mobile data, a sleep and wake, and a control-plane reconnect after 120 s of silence. The sizes are in the fork (`MedAouadhi/microlink`, branch `thistle`); a change means a commit there and moving the pin in `components/kernel/idf_component.yml`, as CLAUDE.md describes. It might give back 10 to 15 KB.
- **A ready line that compares between boots.** A reading taken once the boot has settled, for instance a kernel worker job 30 s after boot, or the ready line naming whether the mesh and the tailnet are on.
- **The 14 KB of heap**, split between the tailnet and the mesh: one boot with only one of them on.
- **Whether the 31 KB largest block is the DMA pool's free space.** The walk's largest free block with both on was 32,024 B, the pool's size less a little. If it is, the largest block `malloc()` can have is smaller.
- **CLAUDE.md's figures** ("about 150 KB"; the tailnet "about 31 KB") predate these measurements: the tailnet's stacks and control blocks alone are 43 KB.
