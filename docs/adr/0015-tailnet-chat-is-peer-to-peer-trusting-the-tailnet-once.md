# Tailnet chat is peer to peer, trusts the tailnet once, and the device speaks with its MeshCore key

Tailnet chat carries messages, voice messages and calls between the T-Deck and a companion app on Linux and Android, and between companion apps, over the user's tailnet. The tailnet already encrypts every packet and admits only the user's nodes, but it tells the device nothing about who sent one: MicroLink parses no users or tags and has no whois, and an Android app beside the Tailscale app has neither.

**Chat peers connect directly.** Each listens on a fixed port on its tailnet address and opens one WebSocket to a peer it has something for. There is no server. A message to a peer that is away waits in its sender's outbox, retried when the peer is seen again and on a backoff, until the receiver's delivered receipt clears it. A T-Deck's tailnet is down while it sleeps, so a background check wakes it, every 15 minutes unless the user changes it, to rejoin and dial its contacts.

**The tailnet is trusted once.** A chat peer proves an Ed25519 identity key at every connection. The first acceptance of a contact request trusts that the request came over the tailnet; the key is pinned then. From then on a peer under a contact's name or address with another key is refused and flagged, until the user accepts the new key in the old one's place. Messages are not encrypted again inside WireGuard.

**The T-Deck's identity key is its MeshCore identity**, the Ed25519 key in `/spiffs/config/mesh_identity`. A device is one key on LoRa and on the tailnet.

## Considered Options

**A hub on homelab**, as Hermes has, would hold messages for an absent peer and make groups easy, but is a server to run and a point every message passes.

**The tailnet alone**, trusting a peer's address and the name it announces, needs no keys, but lets any node on the tailnet pass for any contact, and a phone could not tell.

**End-to-end encryption on top**, a Noise or Signal-style session per contact, guards against a compromised tailnet, at the cost of a second handshake and ratchet on the device.

**A key of Tailnet chat's own on the device**, on the encrypted credentials partition (ADR-0012), would keep it out of plain SPIFFS and apart from MeshCore.

## Consequences

Nothing arrives while both sides are away at once, and a T-Deck receives within one check interval. Its key is in plain SPIFFS: whoever reads the flash can pass for the device to its contacts, on LoRa and the tailnet alike. A new MeshCore identity, or a reflash that loses SPIFFS, flags the device to every contact, who must accept it again; so does a reinstalled companion app. Group chats have no server to keep their membership and are a design of their own.
