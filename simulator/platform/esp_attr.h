/*
 * Simulator shim for ESP-IDF esp_attr.h
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

/* On the device this places the variable in the .ext_ram.bss section, so it
 * lands in PSRAM instead of internal DRAM; apps/appstore uses it to keep its
 * catalogue out of a DRAM segment it would otherwise overflow. Placement is
 * all it does — the variable is zero-initialised either way — so on a host
 * with one flat address space the attribute means nothing. */
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
