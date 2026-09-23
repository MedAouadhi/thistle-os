// SPDX-License-Identifier: BSD-3-Clause
//
// The BHI260AP driver lives in components/kernel_rs/src/drv_imu_bhi260ap.rs
// (Rust). kernel_rs is built with WHOLE_ARCHIVE, so its strong
// `drv_imu_bhi260ap_get` symbol always won the link over the C definition that
// used to be here — and that one could not have worked anyway: it read a
// register map belonging to some other part and never uploaded the firmware
// the hub needs before it has any sensors. This file is intentionally empty.
//
// The header `drv_imu_bhi260ap.h` stays: it declares the Rust entry points for
// the compiled-in T-Deck Pro board and the JSON dispatcher.
