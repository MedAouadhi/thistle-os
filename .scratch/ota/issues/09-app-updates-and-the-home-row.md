# 09: App updates and the Home row

**What to build:** Installed apps get updates the way the firmware does, and
Home says when there is something to update. The daily check and the App
Store's Refresh mark each installed app that has a newer release in the
catalog (a higher release sequence, with a `min_os` the running firmware
meets); the Installed tab shows them with an Update button that installs
through the existing TAP path; and the TAP installer itself refuses a
package whose `min_os` the firmware does not meet. Home's "what is new" rows
gain one naming the ThistleOS version on offer and/or the number of app
updates, which opens Settings → System update, or the App Store when only
apps have updates. Spec: `../spec.md`, "Device UI" and "The update manager".

**Blocked by:** 05 (Release an app by tag and install it from your catalog),
07 (Update over the air from the catalog)

**Status:** ready-for-agent

- [ ] `update_policy`'s app offer is tested on the host: higher, equal and
      lower release sequences, and a `min_os` above, at and below the
      running version (a laptop build counting as its base).
- [ ] The C API gives the number of app updates.
- [ ] A simulator test updates an app from the Installed tab, and an app
      release that needs newer firmware is neither offered nor installable.
- [ ] Home's row reads kernel state once a second, rewrites its label only
      when the text changes, and logs `(launcher_ui) now: ...` when it does;
      the row's choice and text are host-tested with Home's other rows, and
      the Home test covers it in the simulator.
- [ ] On the T-Deck Max, a new app release shows on Home and in the
      Installed tab after a check, and updates on a tap.
