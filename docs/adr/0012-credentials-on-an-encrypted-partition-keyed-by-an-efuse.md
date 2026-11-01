# Credentials on an encrypted partition, keyed by an eFuse

ThistleOS keeps the credentials that its apps, system features and kernel services present to outside services in one store, per service, where it used to keep them in card files and in builds made with a `.env`. This ADR records where that store is kept and how it is protected. `.scratch/credentials/spec.md` has the rest.

**The store is an NVS partition of its own, encrypted with ESP-IDF's HMAC-based NVS encryption.** The keys that encrypt it are derived through the S3's HMAC peripheral from a 256-bit key in an eFuse block of purpose HMAC_UP. No software can read that key back, so a dump of the flash shows no credential. It needs no flash encryption and no secure boot. The partition fills part of the free gap between `phy_init` and `kernel_a`, so no other partition moves, and a USB flash still keeps NVS, SPIFFS and the core dump. The existing `nvs` partition stays as it is.

**The eFuse key is burned the first time a credential is saved**, not at boot. A device that never stores a credential keeps the key block free.

**A device without the partition has no store.** An over-the-air update cannot change the partition table, so a device updated only that way says that the store needs one USB flash. Its consumers see every credential as missing. Nothing falls back to plain text.

## Considered Options

**A namespace in the existing `nvs`** needed no partition change and no eFuse. Anyone with the flash could read it, as they can already read the WiFi passwords and the tailnet identity. Credentials for paid services are worth more to take, and this is the store for them.

**A file on the card** is what was there before. Anyone holding the card had every key, and the card is the part of the device that gets lent, lost and copied.

**Encrypting with the Vault's key** is the strongest at rest. Dictation and AssistNow would then have nothing to work with until the user typed a passphrase after each boot.

**Encrypting the whole `nvs` partition** would protect WiFi and the tailnet identity too. Its format changes when it is encrypted, so moving to it would wipe them, and it would need the same eFuse burn. It can follow on its own.

## Consequences

The burn cannot be undone. If the eFuse key block is lost or changed, nothing on the credential partition can be read again; the partition must be erased and every credential entered again. A `-full.bin` written at 0x0 erases the partition as it erases NVS, which is expected when setting up a new device.

The simulator has no eFuse. Its store is a file in plain text behind the same API, so tests can set and read credentials, and only a device can show the encryption and the burn.

No credential is compiled into any build any more, including a laptop's. Developers set credentials with `tools/set_credentials.py`, which posts them to File Transfer, as a user would paste them.
