# Messages arrive by background checks, not a live connection

A WhatsApp connection on the device costs about 60 KB of internal RAM in task stacks, plus TLS, against about 150 KB free at boot and about 60 KB with the tailnet on. Light sleep drops it. Yet a messenger that tells nobody of a message until its app is opened is of little use.

**The kernel runs background checks.** A kernel module registers a check with an interval; with a network up and the clock set, the scheduler runs it on a FreeRTOS task of its own, made for the check and gone after, holding the device awake meanwhile, and wakes a light-sleeping device on a timer to do so. Deep sleep runs none: waking from it is a restart. WhatsApp's check connects, takes the messages the server kept for the linked device until it says the offline queue is done, logs and alerts them, and disconnects. The interval is the user's, in WhatsApp's settings, 15 minutes unless changed. While the WhatsApp app is open it holds a live connection and the check is paused.

Only kernel modules register checks; packaged apps cannot, as their code would have to stay loaded and be called off the UI task.

## Considered Options

**Always connected**, as SMS is, would alert at once, but would hold the internal RAM all the time and reconnect after every sleep.

**Only while the app is open**, as Hermes is, costs nothing in the background and alerts nothing.

**A check for WhatsApp alone** would be simpler now; the next module that wants one would start again.

## Consequences

A message arrives within one interval, not at once, and each check costs a TLS and Noise handshake and a wake. A check that finds too little internal RAM is skipped and logged, not forced. When WhatsApp unlinks the device or bans the number, the check stops rather than retrying, until the user links again or the ban ends.
