# 15: The Chat app on the device, as a modern chat app made for e-paper

**What to build:** the device's Chat app looked like a settings page: three bordered tab buttons over plain text rows, a conversation of "Lena: ..." lines, and Send, a voice button and Call in a row. Asked for by the user: "revamp the ui of the chat app on the tdeck max, make it modern and intuitive with all the quirks of modern chat apps but also taken into consideration it's on an e-ink display".

**Blocked by:** none (tickets 03, 04, 08, 09 and 13's screens)

**Status:** done (the hardware run, below, to come)

### What it is now

- **The lists:** a large title with Settings at its right, and the tabs along the bottom, as a phone's: a picture over each name, the tab shown with its picture on ink, and the unread messages and waiting requests counted at the picture's corner. A row is a monogram (solid for a contact, hollow for a stranger or a contact never reached, a dot while it is connected), the name in 18 px, when its latest came ("14:05", "Yesterday", "Mon", "2 Nov"), the latest on one line after its tick, and the unread count. Contacts starts with Look for chat peers and Add contact. An empty Chats says what to do, and offers Look for chat peers.
- **A conversation:** back, the contact's monogram, its name and what it is doing (typing, sending, online, its tailnet name), and Call; the name opens the contact's page. Mine are ink bubbles on the right, theirs paper bubbles with an ink edge on the left; the last of a run has a square corner on its writer's side and the time, beside its last line when it fits, with mine's ticks (a clock while waiting, one tick delivered, two read). A heading for each day, a line above what came unread (and the page opens there if it is more than a screen), what a reply quotes inside its bubble, reactions in a pill hanging from it, and voice messages as a round Play, a waveform and their length. A message longer than 200 characters or 12 lines is cut, and Read all shows the rest.
- **Doing things with a message:** a tap opens its sheet (six reactions, the one already ours on ink to take back, Reply, Play, Read all), as a long press and Enter do; a swipe to the right replies to it; a tap on a voice message plays it.
- **The composer:** a rounded field and one round button, the microphone while it is empty (held, it records) and Send, on ink, once something is typed; a reply shows above it, a recording in the field's place.
- **The pages:** a contact's has its monogram, name and presence, round Message and Call buttons, its fingerprint and the name it was seen as in a card, Rename and Remove; a request's has its fingerprint to check before accepting, its messages as bubbles, and Decline and Accept along the bottom, Accept opening the conversation; Settings has this device's name and fingerprint, read receipts as a switch, and how often to check while asleep.
- **Emoji** show as emoji: Noto Emoji (SIL OFL 1.1), about 110 of the most sent cut to 14 px, and the six reactions to 20 px; a heart of the app's own, as Noto's is hatched. The fonts are 31 KB of the kernel slot, now 92% full.

### For e-paper

- Black and white only, nothing grey; inverted (ink) for what is chosen or shown, outlines for the rest.
- No animation, and nothing that flashes when pressed: a pressed button gets a heavier ring, a pressed row nothing.
- A list is drawn again only when a row changed (they are compared with what is on screen), so a contact that starts typing redraws no list, only the conversation's second title line. The composer's button changes only when the field goes from empty to not, and a recording's time once a second.
- Leaving a conversation asks the panel for a full refresh, to clear the black bubbles' ghost from the fast ones.
- Each list stays one `lv_table`: a full conversation of 20 messages leaves 29 KB of LVGL's pool free in the simulator.
- Shorter screens get smaller titles (the T-Deck's LCD), and the Cardputer no tab titles, pictures alone in the tab bar with Settings there, and a conversation title of one line.

### What the hardware has to confirm (T-Deck Max)

- [ ] The black bubbles' ghost after a few fast refreshes, and whether the full refresh on leaving a conversation clears it without being a nuisance (about 1.1 s, a flash).
- [ ] A swipe to the right on a bubble replies, and a long press opens its sheet, on the CST328's touch.
- [ ] The 1-bit emoji and pictures are legible on the panel.
- [ ] Opening a full conversation is quick: each bubble's text is measured to lay it out.

### Tests

- `apps/tailchat/tests/run_tests.sh`: the core's new text (monograms, when, day headings, runs, presence, waveform, ticks, excerpts).
- `simulator/tests/test_tailchat.py`: all 12 cases, the app's taps moved to the new screens; Accept now opens the conversation, and a voice message plays from a tap on its bubble.
- `simulator/tests/test_app_matrix.py`: 120 cases.
