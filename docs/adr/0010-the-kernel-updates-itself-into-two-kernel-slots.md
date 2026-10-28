# The kernel updates itself into the other of two kernel slots

Decided 2026-10-22; not yet built.

A device could not update its kernel over the air. The App Store's "Update OS" wrote ESP-IDF's next update partition, which from `factory` is `ota_0`, Recovery's 2.5 MB slot; the kernel is 0x284870 bytes and no longer fits. The kernel checked only a detached signature over the `.bin`, and only if the catalog named one: no version, no security floor, no board. Recovery has all three (a signed artifact manifest, a security floor in NVS, a chip and board check), but it has never run on a T-Deck Max. The normal build does not flash it, its HTTPS attaches no CA bundle, and `docs/recovery-first-installation.md`, still proposed, already lists "no conventional kernel A/B pair" as a gap.

The partition table becomes two 4.5 MB kernel slots (`ota_0`, `ota_1`) and a 2.5 MB slot reserved for Recovery, with NVS, otadata, SPIFFS and the core dump where they were. The running kernel fetches a release's artifact manifest and its signature, refuses one that is not signed by the publisher's key, is for another chip or board, or carries a security version below the device's security floor, then streams the image into the other kernel slot, checking its size and SHA-256, and only then selects that slot. It names the other kernel slot itself, never ESP-IDF's next update partition, which would be the Recovery slot. The checks are Recovery's, moved into code both can use; the floor stays in Recovery's NVS key (`thistle_sec`/`fw_sec_ver`) so the two agree.

A new kernel is kept once it has run for a minute. Until then ESP-IDF's rollback, already enabled, returns a reset device to the other slot, so a kernel that crashes while WiFi, the tailnet or GPS start up undoes itself; the power manager starts only after that minute. The other slot also holds the previous version, which Settings offers to go back to, with no download, while its security version is at or above the floor.

## Considered Options

**Recovery writes the single kernel slot**, as documented: the kernel stages the update on the card and restarts into Recovery, which verifies it and writes `ota_1`. Recovery would first have to be brought up on the Max; every update would need a card; and a kernel that failed would fall back to whatever `factory` held, never the previous release.

**Recovery as `factory`** would let the bootloader fall back to it when both kernels fail, but `idf.py flash` writes the boot default, `factory`, so every USB flash would need a command of its own. With no `factory`, it writes `ota_0` and resets otadata, as before.

**No Recovery slot.** Its 2.5 MB cannot go to SPIFFS without moving SPIFFS, which wipes the board profile and `system.json`, and a release that boots but cannot update itself would then need USB.

## Consequences

Every device needs one USB flash to take the new table; NVS and SPIFFS survive it.

A board kept the profile it first wrote, so a release that changed a profile never reached a board that had one (tickets 03, 06, 09, 10). Now a kernel rewrites the stored board profile from its embedded copy whenever the version that wrote the stored one is not its own, going back included, and keeps the old one beside it.

A release's `.bin`, artifact manifest and signature copied to `/sdcard/update/` install with no network, through the same checks and into the same other slot.

Recovery stays where it is, unflashed, until it is brought up on the Max as work of its own; its catalog URLs and keys are changed then.

Secure boot and flash encryption stay off. Signatures guard what arrives over the air; whoever holds the device and a USB cable can still flash anything, as before.
