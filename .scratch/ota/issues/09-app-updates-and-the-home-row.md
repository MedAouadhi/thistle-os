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

- [x] `update_policy`'s app offer is tested on the host: higher, equal and
      lower release sequences, and a `min_os` above, at and below the
      running version (a laptop build counting as its base).
- [x] The C API gives the number of app updates.
- [x] A simulator test updates an app from the Installed tab, and an app
      release that needs newer firmware is neither offered nor installable.
- [x] Home's row reads kernel state once a second, rewrites its label only
      when the text changes, and logs `(launcher_ui) now: ...` when it does;
      the row's choice and text are host-tested with Home's other rows, and
      the Home test covers it in the simulator.
- [ ] On the T-Deck Max, a new app release shows on Home and in the
      Installed tab after a check, and updates on a tap.

## Comments

**2026-10-26, implementer.** Done with no board attached; nothing pushed,
no tag created. Built on ticket 07's branch, with `ota` merged in.

- **`update_policy`**: `app_releases` reads the catalog's app entries for
  this chip (each parsed alone, as the firmware entries are), `app_offers`
  picks for each installed package its newest release with a higher release
  sequence whose `min_os` the running firmware meets, a laptop build
  counting as its tag and an untagged one as 0.0.0. 7 new host tests.
- **`update_manager`**: every catalog read (the daily check, Check now, the
  App Store's Refresh, and the kept catalog at boot) counts the app updates
  against `installed_apps::installed_releases` and logs `update check (...):
  1 app update: com.thistle.notes 1.0.1` or `no app updates`; an install or
  a removal recounts them. C API: `rs_update_app_count()`,
  `rs_update_app_offer(id, version, len)`.
- **`min_os`**: the TAP installer refuses a package whose own `package.json`
  `min_os` the firmware does not meet (`TapError::MinOs`, logged `App install
  refused: ... needs ThistleOS X`, `ESP_ERR_INVALID_VERSION`, toast "Notes
  needs a newer ThistleOS"), and `parse_catalog_into` /
  `parse_catalog_entries` leave such app releases out of the App Store's
  lists.
- **The Installed tab** marks an app with an update (`Update: 1.0.1`, the
  status line counts them, the log says `Update offered: ID OLD -> NEW`);
  its details gain Update at the left of Remove, which installs the
  catalog's entry through the same job and TAP path as Install.
- **Home**: `HOME_NOW_SYSTEM_UPDATE` ("System update", "ThistleOS 0.6.0
  and 2 apps") and `HOME_NOW_APP_UPDATES` ("App updates", "1 app has an
  update"), after the tailnet's row. Home reads the offer only while it is
  not installed and waiting for its restart. The row opens Settings at
  System update through the new `settings_app_open_next(page)` (Settings
  logs `Settings opens at System update`; Back goes to its list), or the App
  Store.
- **Simulator**: `THISTLE_SIM_PUBLISHER_KEY` is trusted for apps as well as
  firmware, in simulator builds only, so a test can install an app it
  signed.
- **Tests**: `cargo test` 2089 passed; `thistle_sim_tests` 152 passed (5 new
  Home cases); `test_home.py` 13 of 13 (4 new: the row naming ThistleOS and
  opening System update, apps alone opening the App Store, a release needing
  newer firmware or already installed not counted); `test_appstore.py` 16 of
  16 (3 new: an update from the Installed tab, a release needing newer
  firmware not offered, a package needing it refused although its catalog
  entry says otherwise); `test_system_update.py` 23, `test_launcher_visuals.py`
  10, `test_tap_appstore.py`, `test_tap_notes.py`, `test_kernel_slots.py`
  pass. `idf.py build` succeeds.

**Left for the user:** the T-Deck Max box. Flash a laptop build, install an
app from the catalog, release a newer one by tag (or point the card's
`config/appstore.json` at a catalog listing one), then Refresh or wait for
the daily check: Home should show "App updates", the Installed tab
`Update: X`, and Update should install it.
