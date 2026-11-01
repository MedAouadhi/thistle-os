# WhatsApp is an AGPL part kept out of releases

ThistleOS can be a linked device of the user's WhatsApp account through `oxidezap/whatsapp-rust`. That library's crates say MIT, but its Signal implementation, `wacore/libsignal`, is a copy of Signal's libsignal, and every one of its 31 source files keeps Signal's `SPDX-License-Identifier: AGPL-3.0-only`; only its `Cargo.toml` says MIT. ThistleOS is BSD-3 and takes no GPL dependency.

**WhatsApp is an optional part under its own AGPL licence file, built only when a laptop's `.env` sets `THISTLE_WHATSAPP=1`.** Release builds run on hosted runners with no `.env`, so no release ever contains it, and released firmware stays BSD-3. A laptop build with it is the user's own; the AGPL asks nothing of a build that is not distributed. The part takes upstream's core crates only, with ThistleOS's own transport, store and runtime, not the ESP32 port's crate, which would bring esp-idf-svc, PSRAM stacks and global sdkconfig changes into the kernel.

A build with WhatsApp says so, and Settings → System update warns that a release has no WhatsApp before installing one. After it, the user flashes their own build again by USB; the linked device's store on the card is picked up again.

## Considered Options

**Ship it in releases** would make every release AGPL as a whole. The source is public, so complying would be easy, but it changes the project's licence for one app.

**Replace the Signal core** with a permissively licensed one: there is no mature one in Rust, and writing a double ratchet and sender keys ourselves is crypto we would rather not own.

## Consequences

Nobody but a developer gets WhatsApp. Making it a release feature later is a decision about the project's licence, not a code change. The mislabelled licence is upstream's to fix; if Signal's code is ever replaced there, this ADR can be revisited.
