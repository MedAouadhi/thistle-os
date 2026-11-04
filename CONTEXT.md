# ThistleOS

A portable ESP32-family operating system with an immutable kernel and loadable drivers, apps and window managers. This glossary fixes the vocabulary for hardware configuration and the driver layer, where several terms have drifted.

## Language

### Boards and configuration

**Board profile**:
The JSON document describing one board's hardware — its buses, pin assignments, and the drivers to load. Exists as a file in the board catalogue and as a copy embedded in the kernel.
_Avoid_: board config, board definition, board.json, board file

**Board id**:
The catalogue identifier for a board, always lowercase and hyphenated. The board profile's filename stem must equal it.
_Avoid_: board name, board slug

**Board name**:
The human-readable label for a board, distinct from its board id. Boards frequently self-report a name that differs from both.

**Embedded fallback profile**:
A board profile duplicated as a string literal inside the kernel, written to storage when hardware detection identifies a board with no profile present. The only configuration a freshly flashed board ever sees.
_Avoid_: fallback JSON, builtin board

**Writer version**:
The firmware version that wrote a stored board profile, kept in it as `written_by`. A kernel of another version rewrites the profile from its embedded fallback profile, so the profile follows the firmware.
_Avoid_: profile version (the profile's own `board.version` is something else)

**Fingerprint**:
The set of I²C addresses whose presence on a given pin pair identifies a board. Detection matches fingerprints in order, first match winning.
_Avoid_: signature, probe result

### Buses and pins

**Bus index**:
The position of a bus in the board profile's declared bus array, and the value drivers use to request a bus. Distinct from the hardware host or port number, which appears inside the bus declaration and is not what drivers reference.
_Avoid_: bus number, host, port

**Bus handle**:
The opaque token a driver receives when it requests a bus by index, and through which it performs transfers.

**Expander pin**:
A control line on an IO expander rather than on the SoC. Addressed by expander index and pin number, never by a GPIO number, and reachable only after the bus carrying its expander is up.
_Avoid_: virtual GPIO, extended GPIO, expander GPIO

### Drivers

**HAL interface**:
One of the vtable contracts a driver can implement — display, input, radio, GPS, audio, power, IMU, storage, network, crypto, RTC. A board populates some subset; absent hardware leaves an interface unregistered rather than stubbed.
_Avoid_: HAL driver, HAL type, HAL vtable

**Driver**:
An implementation of a HAL interface for a specific part, loaded from storage as an ELF or compiled into the kernel. Named for the part it drives, not the board it appears on.

**Driver entry**:
A driver's declaration inside a board profile — its identifier, the HAL interface it registers, its ELF filename, and its part-specific configuration.

**Variant**:
Two parts that are register-compatible enough to share one driver, distinguished at runtime by probing. A variant is not a separate driver.

### Time

**Time source**:
Anything that can tell the kernel the time: a time server (NTP), the GPS receiver, the mobile network (NITZ), a clock chip, or the user with automatic time off. Ranked in that order, the user aside; the one that last set or confirmed the clock is what Settings names.
_Avoid_: clock source, time provider, sync source

**Clock chip**:
A battery-backed real-time clock on the board, such as a PCF8563, registered as the RTC HAL interface. Distinct from the SoC's own RTC domain, which keeps the time through sleep and restarts but not through a power-on.
_Avoid_: RTC (ambiguous between the two), hardware clock

### Releases and updates

**Publisher**:
The party whose signing key a device trusts to install firmware and packages. A fork that releases on its own is its own publisher; a device trusts the publisher its kernel was built for, and no other.
_Avoid_: vendor, upstream (as a trust term), trusted source

**Release**:
One published version of the firmware, or of one app, made from one tagged commit and never changed once published. The firmware and each app release independently of one another.
_Avoid_: build, drop

**Catalog**:
The index of everything a device can install (firmware, apps, drivers, window managers) and where to fetch each. Discovery only: a device trusts what it installs because the publisher signed it, never because the catalog listed it.
_Avoid_: manifest, feed, store index

**Artifact manifest**:
The publisher's signed statement of what one firmware artifact is: its version, its chip, the boards it suits, its size and its hash. Distinct from a package manifest, which an app, driver or window manager carries to declare its id, version and permissions.
_Avoid_: firmware manifest, manifest (unqualified)

**Kernel slot**:
One of the two flash partitions that each hold a kernel. One is running; an update is written to the other, which boots next and is kept only if it comes up.
_Avoid_: OTA partition, A/B partition, ota_0, ota_1

**Security version**:
A number each firmware release carries in its artifact manifest, raised only by a release that fixes a security hole. Distinct from the release's version, which rises with every release.
_Avoid_: anti-rollback version, secure version

**Security floor**:
The highest security version a device has installed. A firmware below it is never installed or booted again, so a fixed hole cannot be reopened by going back.
_Avoid_: rollback counter, minimum version

### Credentials

**Service**:
An outside provider a device authenticates to, such as PodcastIndex, Cohere, Anthropic or Thingstream. Credentials are kept per service, not per app.
_Avoid_: provider, account, API

**Credential**:
What a device presents to one service to prove it may use it: an API key, a token, or a key and secret pair. Never a setting with a sensible default, such as an address or a model name.
_Avoid_: secret, token, API key (as the general term), config

**Credential requirement**:
A consumer's declaration that it uses a service's credential. An app, a system feature or a kernel service may read only the credentials it has declared, and one entry serves every consumer that declares its service.
_Avoid_: credential permission, secret request

### Background work

**Background check**:
A short, periodic piece of work a kernel module registers to look for news while nothing in front asks for it, such as fetching the WhatsApp messages that came in since the last one. It runs at an interval, only with a network up, may wake the device from light sleep, and finishes on its own; it is paused while the module's app holds a live connection.
_Avoid_: poll, sync, background job, push

### Messaging

**Linked device**:
A device that a phone's WhatsApp account has linked as one of its companions, by a QR code or a pairing code, and that then sends and receives as that account. It is identified by keys it made at linking, not by a credential, and stays linked until the phone unlinks it or it is gone too long.
_Avoid_: WhatsApp account, session, login

**Tailnet chat**:
Messaging, voice and video between installs that speak its protocol over the user's tailnet: the T-Deck's own app, and the companion app on Linux and phones. Peers connect to each other directly; there is no server. A message to a peer that is away waits with its sender until it is delivered.
_Avoid_: tailnet messenger, Tailscale chat, P2P chat

**Chat peer**:
An install that answers Tailnet chat's hello: the T-Deck's app or a companion app. A tailnet peer is any node on the tailnet, Tailscale's meaning, whether or not it speaks Tailnet chat.
_Avoid_: peer (unqualified, in Tailnet chat), node, client

**Identity key**:
The key a chat peer proves at every connection. It, not the tailnet name or address, is who a contact is. A companion app makes its own on first start; the T-Deck uses its MeshCore identity, so a device is one key on LoRa and on the tailnet.
_Avoid_: node key, device key, account

**Contact** (Tailnet chat):
A chat peer whose identity key the user has accepted. The first acceptance trusts the tailnet alone; from then on a chat peer under that contact's name or address with another key is refused and flagged, until the user accepts the new key in its place.
_Avoid_: friend, buddy, chat peer (for an accepted one)

**Contact request**:
What a chat peer that is not yet a contact sends, with or without messages. It waits, with its tailnet name and key fingerprint, until the user accepts it, which makes the sender a contact and delivers its messages, or declines it, which drops them.
_Avoid_: invite, friend request

**Call** (Tailnet chat):
A live conversation between two chat peers: always voice, and video while either side has its camera on. Between two video-capable chat peers, either side may switch its camera on or off at any time, from the first ring on. There is one kind of call; one with a camera on is not a different call.
_Avoid_: video call, voice call (as two kinds of call)

**Video-capable** (chat peer):
A chat peer that says it can take part in video, whether or not it has a camera: one without a camera still sees the other side's. A chat peer is video-capable by what it announces, never by the machine it runs on; the T-Deck is not, nor is an older companion app.
_Avoid_: supported device, video device
