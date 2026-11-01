# 08: Update warning in WhatsApp builds

**What to build:** A WhatsApp build says so, and warns before an update
takes WhatsApp away (ADR-0013). Settings' About shows that it is a WhatsApp
build. Settings → System update, before Download and install or Install
from card goes ahead, says that the release has no WhatsApp and that the
user's own build will need flashing again by USB, where the linked device
is picked up from the card. A build without WhatsApp shows neither.

**Blocked by:** 01

**Status:** ready-for-agent

- [ ] A flag the update manager and Settings read says whether this is a WhatsApp build
- [ ] Simulator test: a WhatsApp build shows the warning before installing from the fake GitHub and from the card, and installs after it is accepted; a build without it shows none
- [ ] About shows the WhatsApp build

## Comments
