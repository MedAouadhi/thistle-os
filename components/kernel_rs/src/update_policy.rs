// SPDX-License-Identifier: BSD-3-Clause
//! What a system update may do, decided without touching the flash, the
//! network or NVS. `update_manager` runs these; see `.scratch/ota/spec.md`,
//! "The update manager", and ADR-0010.
//!
//! **Accept.** A firmware is installed only if its artifact manifest (the
//! publisher's signed description of it, `thistle_artifact`) holds up:
//!
//! - the manifest's signature verifies against the kernel's key;
//! - the manifest is in its canonical form;
//! - its type is firmware;
//! - its arch is the running chip's;
//! - the stored board id is among its compatible boards;
//! - its security version is at least the device's security floor.
//!
//! An online offer also needs a version newer than the running one; a
//! release copied to the card may be any version at or above the floor.
//! Then the bytes written must be the manifest's: its size and its SHA-256.
//!
//! **The floor** rises to a kernel's security version when that kernel is
//! kept, never when it is written, so one rolled back never raises it.
//!
//! **Over the air.** The catalog's newest firmware newer than the running
//! version (a laptop build counting as its tag), for this chip and board, is
//! offered. The check is due a day after the last, or at once if there was
//! none or the clock went back; with the clock unset, once a boot; never
//! within an hour of the last try. A download starts on WiFi; on mobile data
//! only once the user allows it.

use crate::version::Version;
use thistle_artifact::{ArtifactManifest, ArtifactType};

/// What the device is, as a manifest is checked against it.
#[derive(Clone, Debug)]
pub(crate) struct Device<'a> {
    /// `manifest::current_arch()`: `esp32s3`, or `host` in the simulator.
    pub chip: &'a str,
    /// The stored board profile's `board_id`.
    pub board: &'a str,
    /// The security floor: no firmware below it installs.
    pub floor: u32,
}

/// Where a firmware comes from, which decides whether it must be newer.
#[derive(Clone, Debug)]
pub(crate) enum Source<'a> {
    /// A release copied to `/sdcard/update/`: any version at or above the
    /// floor, so the user can step back by hand.
    Card,
    /// A release the catalog offers: only one newer than the running version.
    Online { running: &'a str },
}

/// Why a firmware was refused. `reason()` says it to the user.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum Refusal {
    /// The manifest's signature does not verify against the kernel's key.
    Signature,
    /// The manifest is not a canonical V1 artifact manifest.
    Malformed(String),
    /// The manifest describes a board profile, a driver or a window manager.
    NotFirmware(&'static str),
    /// Built for another chip.
    Chip { built_for: String, this: String },
    /// The stored board is not among its compatible boards.
    Board(String),
    /// Its security version is below the device's floor.
    BelowFloor { security_version: u32, floor: u32 },
    /// Offered online, but not newer than what runs.
    NotNewer { version: String, running: String },
    /// The file is not the manifest's size.
    Size { file: u64, manifest: u64 },
    /// The bytes written do not hash to the manifest's SHA-256.
    Hash,
}

impl Refusal {
    /// The reason, as Settings and the log show it.
    pub(crate) fn reason(&self) -> String {
        match self {
            Refusal::Signature => "not signed by this device's publisher".into(),
            Refusal::Malformed(why) => format!("its manifest is malformed: {why}"),
            Refusal::NotFirmware(kind) => format!("it is a {kind}, not a firmware"),
            Refusal::Chip { built_for, this } => {
                format!("it is for {built_for}, not this {this}")
            }
            Refusal::Board(board) => format!("it is not for this board, {board}"),
            Refusal::BelowFloor { security_version, floor } => format!(
                "its security version {security_version} is below this device's floor {floor}"
            ),
            Refusal::NotNewer { version, running } => {
                format!("{version} is not newer than {running}")
            }
            Refusal::Size { file, manifest } => {
                format!("the file is {file} bytes; its manifest says {manifest}")
            }
            Refusal::Hash => "the file's SHA-256 is not its manifest's".into(),
        }
    }
}

/// Whether to install the firmware a manifest describes, before a byte of it
/// is written. `manifest` is the text whose signature `signature_ok` judged.
pub(crate) fn accept(
    signature_ok: bool,
    manifest: &str,
    device: &Device,
    source: &Source,
) -> Result<ArtifactManifest, Refusal> {
    if !signature_ok {
        return Err(Refusal::Signature);
    }
    let manifest = ArtifactManifest::parse(manifest).map_err(Refusal::Malformed)?;
    if manifest.artifact_type != ArtifactType::Firmware {
        let kind = match manifest.artifact_type {
            ArtifactType::Board => "board profile",
            ArtifactType::Driver => "driver",
            ArtifactType::WindowManager => "window manager",
            ArtifactType::Firmware => "firmware",
        };
        return Err(Refusal::NotFirmware(kind));
    }
    if manifest.arch != device.chip {
        return Err(Refusal::Chip { built_for: manifest.arch.clone(), this: device.chip.into() });
    }
    if !manifest.compatible_boards.iter().any(|b| b == device.board) {
        return Err(Refusal::Board(device.board.into()));
    }
    if manifest.security_version < device.floor {
        return Err(Refusal::BelowFloor {
            security_version: manifest.security_version,
            floor: device.floor,
        });
    }
    if let Source::Online { running } = source {
        let newer = match (Version::parse(&manifest.version), Version::parse(running)) {
            (Some(offered), Some(current)) => offered > current,
            // A running version that does not read (an untagged build) is
            // older than any release.
            (Some(_), None) => true,
            (None, _) => false,
        };
        if !newer {
            return Err(Refusal::NotNewer {
                version: manifest.version.clone(),
                running: (*running).into(),
            });
        }
    }
    Ok(manifest)
}

/// Whether a file of `size` bytes can be the one `manifest` describes.
pub(crate) fn check_size(manifest: &ArtifactManifest, size: u64) -> Result<(), Refusal> {
    if size == manifest.size as u64 {
        Ok(())
    } else {
        Err(Refusal::Size { file: size, manifest: manifest.size as u64 })
    }
}

/// Whether the bytes written, whose SHA-256 is `digest`, are the manifest's.
pub(crate) fn check_hash(manifest: &ArtifactManifest, digest: &[u8; 32]) -> Result<(), Refusal> {
    let hex: String = digest.iter().map(|b| format!("{b:02x}")).collect();
    if hex == manifest.sha256 {
        Ok(())
    } else {
        Err(Refusal::Hash)
    }
}

/// The floor once a kernel of `security_version` is kept: it never falls.
pub(crate) fn floor_after_keep(floor: u32, security_version: u32) -> u32 {
    floor.max(security_version)
}

/// Of the releases on the card, by their manifests' versions, which to
/// install: the newest that reads, else the first, so its refusal is shown.
pub(crate) fn pick_card_release(versions: &[Option<&str>]) -> Option<usize> {
    if versions.is_empty() {
        return None;
    }
    let newest = versions
        .iter()
        .enumerate()
        .filter_map(|(i, v)| v.and_then(Version::parse).map(|v| (i, v)))
        .max_by(|a, b| a.1.cmp(&b.1).then(b.0.cmp(&a.0)));
    Some(newest.map_or(0, |(i, _)| i))
}

// ---------------------------------------------------------------------------
// Over the air (ticket 07): the offer, when to check, and mobile data
// ---------------------------------------------------------------------------

/// A firmware release as the catalog lists it: one flat entry, as
/// tools/release_firmware.py writes it. Nothing here is trusted: the
/// download is checked against the signed artifact manifest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct FirmwareRelease {
    pub version: String,
    pub arch: String,
    pub compatible_boards: Vec<String>,
    /// The image.
    pub url: String,
    pub manifest_url: String,
    pub manifest_sig_url: String,
    pub sha256: String,
    pub size: u64,
    /// The release's notes, CHANGELOG.md's section for its tag.
    pub notes: String,
}

/// The firmware releases a catalog lists, each with what an install needs;
/// an entry missing any of it is left out.
///
/// Only the firmware entries are parsed into JSON values, one at a time: a
/// whole catalog's tree, of small allocations, would land in internal RAM
/// (allocations under 4 KB do), where a 64 KB catalog would cost far more
/// than the device has to spare.
pub(crate) fn firmware_releases(catalog_json: &str) -> Vec<FirmwareRelease> {
    use serde_json::Value;
    let text = |entry: &Value, key: &str| -> Option<String> {
        entry.get(key).and_then(Value::as_str).filter(|s| !s.is_empty()).map(str::to_string)
    };
    entry_objects(catalog_json)
        .into_iter()
        .filter(|object| object.contains("\"firmware\""))
        .filter_map(|object| serde_json::from_str::<Value>(object).ok())
        .filter(|entry| entry.get("type").and_then(Value::as_str) == Some("firmware"))
        .filter_map(|entry| {
            let entry = &entry;
            Some(FirmwareRelease {
                version: text(entry, "version")?,
                arch: text(entry, "arch")?,
                compatible_boards: entry
                    .get("compatible_boards")?
                    .as_array()?
                    .iter()
                    .filter_map(|b| b.as_str().map(str::to_string))
                    .collect(),
                url: text(entry, "url")?,
                manifest_url: text(entry, "manifest_url")?,
                manifest_sig_url: text(entry, "manifest_sig_url")?,
                sha256: text(entry, "sha256").unwrap_or_default(),
                size: entry.get("size_bytes").and_then(Value::as_u64).filter(|&n| n > 0)?,
                notes: text(entry, "changelog").unwrap_or_default(),
            })
        })
        .collect()
}

/// The text of each object in the catalog's `entries` array, found without
/// parsing the catalog: braces and brackets inside strings are skipped.
fn entry_objects(json: &str) -> Vec<&str> {
    let Some(key) = json.find("\"entries\"") else { return Vec::new() };
    let Some(open) = json[key..].find('[').map(|i| key + i) else { return Vec::new() };
    let bytes = json.as_bytes();
    let mut objects = Vec::new();
    let (mut depth, mut start) = (0usize, 0usize);
    let (mut in_string, mut escaped) = (false, false);
    for (i, &b) in bytes.iter().enumerate().skip(open + 1) {
        if in_string {
            match b {
                _ if escaped => escaped = false,
                b'\\' => escaped = true,
                b'"' => in_string = false,
                _ => {}
            }
            continue;
        }
        match b {
            b'"' => in_string = true,
            b'{' | b'[' => {
                if depth == 0 && b == b'{' {
                    start = i;
                }
                depth += 1;
            }
            b'}' | b']' => {
                if depth == 0 {
                    break; // the end of the entries array
                }
                depth -= 1;
                if depth == 0 && b == b'}' {
                    objects.push(&json[start..=i]);
                }
            }
            _ => {}
        }
    }
    objects
}

/// Which of the catalog's firmware releases to offer: the newest, by
/// version, that is newer than the running one (a laptop build counting as
/// the tag it was built from) and built for this chip and this board.
pub(crate) fn pick_offer(
    releases: &[FirmwareRelease],
    running: &str,
    chip: &str,
    board: &str,
) -> Option<usize> {
    let running = Version::parse(running);
    releases
        .iter()
        .enumerate()
        .filter(|(_, r)| r.arch == chip && r.compatible_boards.iter().any(|b| b == board))
        .filter_map(|(i, r)| Version::parse(&r.version).map(|v| (i, v)))
        .filter(|(_, v)| running.as_ref().map_or(true, |running| v > running))
        .max_by(|a, b| a.1.cmp(&b.1))
        .map(|(i, _)| i)
}

/// How long after a check the next is due.
pub(crate) const CHECK_EVERY_S: i64 = 24 * 3600;
/// How long after a check that failed, or any check this boot, the next may
/// be tried.
pub(crate) const RETRY_MS: u64 = 3600 * 1000;

/// What decides whether the daily check is due.
#[derive(Clone, Debug)]
pub(crate) struct CheckInputs {
    /// A network is up.
    pub network: bool,
    /// A check or an install is under way, or one waits for a restart.
    pub busy: bool,
    /// The time, if the clock is set (`time_policy::plausible`).
    pub now_s: Option<i64>,
    /// When the last check that read the catalog was made, if one was.
    pub last_check_s: Option<i64>,
    /// How long ago this boot's last check was tried, if one was.
    pub attempted_ms_ago: Option<u64>,
}

/// The daily check is due: with a network, nothing under way, and no check
/// tried this boot in the last hour; then, with the clock set, a day after
/// the last check (or at once, if there was none or the clock has gone back
/// since); with the clock unset, once a boot.
pub(crate) fn check_due(i: &CheckInputs) -> bool {
    if !i.network || i.busy {
        return false;
    }
    if i.attempted_ms_ago.is_some_and(|ago| ago < RETRY_MS) {
        return false;
    }
    match (i.now_s, i.last_check_s) {
        (None, _) => i.attempted_ms_ago.is_none(),
        (Some(_), None) => true,
        (Some(now), Some(last)) => now < last || now - last >= CHECK_EVERY_S,
    }
}

/// What the network is to a download.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Network {
    None,
    /// WiFi, Ethernet, or the simulator's host network.
    Free,
    /// Mobile data, which costs the user.
    Mobile,
}

/// Whether a download may start.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Gate {
    Start,
    WaitForNetwork,
    WaitForConsent,
}

/// A download starts on WiFi at once; on mobile data only once the user has
/// allowed it; with no network it waits for one.
pub(crate) fn may_download(network: Network, mobile_data_allowed: bool) -> Gate {
    match network {
        Network::None => Gate::WaitForNetwork,
        Network::Free => Gate::Start,
        Network::Mobile if mobile_data_allowed => Gate::Start,
        Network::Mobile => Gate::WaitForConsent,
    }
}

/// What NVS records of a kernel slot: the version and security version of
/// the kernel written into it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct SlotRecord<'a> {
    pub version: &'a str,
    pub security_version: u32,
}

/// The version going back to the other kernel slot would reach, if it is
/// offered: only while the slot's `record` names the version its `image`
/// describes (none once an install has begun erasing it) and the record's
/// security version is at or above the `floor`.
pub(crate) fn go_back(record: Option<SlotRecord>, image: Option<&str>, floor: u32) -> Option<String> {
    let record = record?;
    let image = image?;
    if record.version.is_empty() || record.version != image || record.security_version < floor {
        return None;
    }
    Some(record.version.to_string())
}

/// What to do with the stored board profile at boot.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Profile {
    /// Use it as it is.
    Keep,
    /// Rewrite it from the kernel's embedded copy, keeping it as the backup.
    Rewrite,
}

/// Whether the stored board profile, written by firmware `writer` (None if
/// it names none), is rewritten by the `running` firmware, which embeds a
/// profile for its board id or not. Any other version rewrites it, going
/// back included, so a profile travels with the firmware that carries it.
pub(crate) fn board_profile(writer: Option<&str>, running: &str, embedded: bool) -> Profile {
    if embedded && writer != Some(running) {
        Profile::Rewrite
    } else {
        Profile::Keep
    }
}

// ---------------------------------------------------------------------------
// App updates (ticket 09)
// ---------------------------------------------------------------------------

/// An app release as the catalog lists it: what deciding an offer needs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct AppRelease {
    pub id: String,
    pub version: String,
    /// major x 10^6 + minor x 10^3 + patch, as the release workflow writes it.
    pub release_sequence: u32,
    /// The oldest firmware it runs on; empty for any.
    pub min_os: String,
}

/// An app the App Store installed: its id and the release it runs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct InstalledApp {
    pub id: String,
    pub release_sequence: u32,
}

/// The app releases a catalog lists for this chip: entries of type `app`
/// (or of no type, as the App Store reads them) with an id, a version and a
/// release sequence, built for `chip` or for any. Each entry is parsed on
/// its own, as `firmware_releases` does.
pub(crate) fn app_releases(catalog_json: &str, chip: &str) -> Vec<AppRelease> {
    use serde_json::Value;
    let text = |entry: &Value, key: &str| -> String {
        entry.get(key).and_then(Value::as_str).unwrap_or("").to_string()
    };
    entry_objects(catalog_json)
        .into_iter()
        .filter_map(|object| serde_json::from_str::<Value>(object).ok())
        .filter(|entry| matches!(entry.get("type").and_then(Value::as_str), None | Some("app")))
        .filter(|entry| matches!(entry.get("arch").and_then(Value::as_str), None | Some("")) || text(entry, "arch") == chip)
        .filter_map(|entry| {
            let release_sequence = entry
                .get("release_sequence")
                .and_then(Value::as_u64)
                .filter(|&n| n > 0 && n <= u32::MAX as u64)? as u32;
            let release = AppRelease {
                id: text(&entry, "id"),
                version: text(&entry, "version"),
                release_sequence,
                min_os: text(&entry, "min_os_version"),
            };
            (!release.id.is_empty() && !release.version.is_empty()).then_some(release)
        })
        .collect()
}

/// Which app releases to offer: for each installed app, in the order given,
/// its newest release with a higher release sequence whose `min_os` the
/// running firmware meets (a laptop build counting as its tag).
pub(crate) fn app_offers<'a>(
    releases: &'a [AppRelease],
    installed: &[InstalledApp],
    running: &str,
) -> Vec<&'a AppRelease> {
    let running = Version::parse(running)
        .unwrap_or(Version { major: 0, minor: 0, patch: 0, pre: String::new() });
    installed
        .iter()
        .filter_map(|app| {
            releases
                .iter()
                .filter(|r| r.id == app.id && r.release_sequence > app.release_sequence)
                .filter(|r| crate::version::meets(&running, &r.min_os))
                .max_by_key(|r| r.release_sequence)
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    const SHA:&str = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";

    /// A manifest as tools/sign.py writes one for a firmware release.
    fn manifest(edit: impl FnOnce(&mut ArtifactManifest)) -> String {
        let mut m = ArtifactManifest {
            artifact_type: ArtifactType::Firmware,
            id: "thistle-os".into(),
            version: "0.6.0".into(),
            security_version: 500,
            arch: "esp32s3".into(),
            compatible_boards: vec!["tdeck".into(), "tdeck-max".into()],
            destination: "kernel".into(),
            sha256: SHA.into(),
            size: 4,
            url: "https://github.com/x/thistle-os/releases/download/v0.6.0/a.bin".into(),
        };
        edit(&mut m);
        m.destination = thistle_artifact::expected_destination(m.artifact_type, &m.id);
        String::from_utf8(m.canonical_bytes()).unwrap()
    }

    fn max() -> Device<'static> {
        Device { chip: "esp32s3", board: "tdeck-max", floor: 500 }
    }

    fn card(text: &str) -> Result<ArtifactManifest, Refusal> {
        accept(true, text, &max(), &Source::Card)
    }

    // --- accept --------------------------------------------------------------

    #[test]
    fn a_signed_firmware_for_this_chip_and_board_at_the_floor_is_accepted() {
        let accepted = card(&manifest(|_| {})).unwrap();
        assert_eq!(accepted.version, "0.6.0");
        assert_eq!(accepted.size, 4);
    }

    #[test]
    fn a_bad_signature_is_refused_whatever_the_manifest_says() {
        assert_eq!(
            accept(false, &manifest(|_| {}), &max(), &Source::Card),
            Err(Refusal::Signature)
        );
        assert_eq!(Refusal::Signature.reason(), "not signed by this device's publisher");
    }

    #[test]
    fn a_manifest_out_of_canonical_form_is_refused() {
        let text = manifest(|_| {}).replace("\n", "\r\n");
        assert!(matches!(card(&text), Err(Refusal::Malformed(_))));
        assert!(matches!(card("hello"), Err(Refusal::Malformed(_))));
    }

    #[test]
    fn a_manifest_of_another_type_is_refused() {
        let board = manifest(|m| {
            m.artifact_type = ArtifactType::Board;
            m.id = "tdeck-max".into();
        });
        assert_eq!(card(&board), Err(Refusal::NotFirmware("board profile")));
        assert_eq!(
            Refusal::NotFirmware("driver").reason(),
            "it is a driver, not a firmware"
        );
    }

    #[test]
    fn a_firmware_for_another_chip_is_refused() {
        let c3 = manifest(|m| m.arch = "esp32c3".into());
        assert_eq!(
            card(&c3),
            Err(Refusal::Chip { built_for: "esp32c3".into(), this: "esp32s3".into() })
        );
        assert_eq!(card(&c3).unwrap_err().reason(), "it is for esp32c3, not this esp32s3");
    }

    #[test]
    fn a_firmware_for_other_boards_is_refused() {
        let other = manifest(|m| m.compatible_boards = vec!["tdeck".into(), "tdeck-pro".into()]);
        assert_eq!(card(&other), Err(Refusal::Board("tdeck-max".into())));
        assert_eq!(
            card(&other).unwrap_err().reason(),
            "it is not for this board, tdeck-max"
        );
    }

    #[test]
    fn below_the_floor_is_refused_at_and_above_it_accepted() {
        let below = manifest(|m| m.security_version = 499);
        assert_eq!(
            card(&below),
            Err(Refusal::BelowFloor { security_version: 499, floor: 500 })
        );
        assert_eq!(
            card(&below).unwrap_err().reason(),
            "its security version 499 is below this device's floor 500"
        );
        assert!(card(&manifest(|m| m.security_version = 500)).is_ok());
        assert!(card(&manifest(|m| m.security_version = 501)).is_ok());
    }

    #[test]
    fn a_device_with_no_floor_yet_takes_any_security_version() {
        let none = Device { floor: 0, ..max() };
        assert!(accept(true, &manifest(|m| m.security_version = 1), &none, &Source::Card).is_ok());
    }

    #[test]
    fn a_card_install_may_be_any_version_at_or_above_the_floor() {
        for version in ["0.1.0", "0.6.0-rc.1", "9.0.0"] {
            assert!(card(&manifest(|m| m.version = version.into())).is_ok(), "{version}");
        }
    }

    #[test]
    fn an_online_offer_must_be_newer_than_what_runs() {
        let online = |version: &str, running: &str| {
            accept(true, &manifest(|m| m.version = version.into()), &max(), &Source::Online {
                running,
            })
        };
        assert!(online("0.6.0", "0.5.2").is_ok());
        assert!(online("0.6.0", "0.6.0-rc.1").is_ok());
        assert!(online("0.6.0", "0.5.2-108-gabc1234-dirty").is_ok());
        assert_eq!(
            online("0.6.0", "0.6.0"),
            Err(Refusal::NotNewer { version: "0.6.0".into(), running: "0.6.0".into() })
        );
        assert!(online("0.6.0", "0.6.0-3-gabc1234").is_err());
        assert!(online("0.5.0", "0.6.0").is_err());
        assert!(online("0.6.0", "0.0.0-unknown").is_ok());
    }

    #[test]
    fn checks_apply_in_order_signature_first() {
        // Everything wrong: the signature is what is reported.
        let all_wrong = manifest(|m| {
            m.arch = "esp32c3".into();
            m.security_version = 1;
        });
        assert_eq!(accept(false, &all_wrong, &max(), &Source::Card), Err(Refusal::Signature));
        assert!(matches!(card(&all_wrong), Err(Refusal::Chip { .. })));
    }

    // --- the bytes -----------------------------------------------------------

    #[test]
    fn a_file_of_another_size_is_refused() {
        let m = card(&manifest(|_| {})).unwrap();
        assert_eq!(check_size(&m, 4), Ok(()));
        assert_eq!(check_size(&m, 5), Err(Refusal::Size { file: 5, manifest: 4 }));
        assert_eq!(
            check_size(&m, 5).unwrap_err().reason(),
            "the file is 5 bytes; its manifest says 4"
        );
    }

    #[test]
    fn bytes_that_hash_otherwise_are_refused() {
        let m = card(&manifest(|_| {})).unwrap();
        // SHA-256 of "test".
        let test: [u8; 32] = [
            0x9f, 0x86, 0xd0, 0x81, 0x88, 0x4c, 0x7d, 0x65, 0x9a, 0x2f, 0xea, 0xa0, 0xc5, 0x5a,
            0xd0, 0x15, 0xa3, 0xbf, 0x4f, 0x1b, 0x2b, 0x0b, 0x82, 0x2c, 0xd1, 0x5d, 0x6c, 0x15,
            0xb0, 0xf0, 0x0a, 0x08,
        ];
        assert_eq!(check_hash(&m, &test), Ok(()));
        let mut other = test;
        other[31] ^= 1;
        assert_eq!(check_hash(&m, &other), Err(Refusal::Hash));
    }

    // --- the floor -----------------------------------------------------------

    #[test]
    fn keeping_a_kernel_raises_the_floor_and_never_lowers_it() {
        assert_eq!(floor_after_keep(0, 500), 500);
        assert_eq!(floor_after_keep(500, 600), 600);
        assert_eq!(floor_after_keep(600, 500), 600);
    }

    // --- the card ------------------------------------------------------------

    #[test]
    fn of_several_releases_on_the_card_the_newest_is_installed() {
        assert_eq!(pick_card_release(&[]), None);
        assert_eq!(pick_card_release(&[Some("0.6.0")]), Some(0));
        assert_eq!(
            pick_card_release(&[Some("0.6.0"), Some("0.10.0"), Some("0.7.0-rc.1")]),
            Some(1)
        );
        assert_eq!(pick_card_release(&[Some("0.6.0-rc.1"), Some("0.6.0")]), Some(1));
    }

    #[test]
    fn a_card_release_whose_manifest_does_not_read_is_still_picked_to_be_refused() {
        assert_eq!(pick_card_release(&[None]), Some(0));
        assert_eq!(pick_card_release(&[None, Some("0.6.0")]), Some(1));
    }

    // --- the online offer (ticket 07) ----------------------------------------

    fn release(version: &str) -> FirmwareRelease {
        FirmwareRelease {
            version: version.into(),
            arch: "esp32s3".into(),
            compatible_boards: vec!["tdeck".into(), "tdeck-max".into()],
            url: format!("https://github.com/x/thistle-os/releases/download/v{version}/a.bin"),
            manifest_url: "https://example.com/a.bin.manifest".into(),
            manifest_sig_url: "https://example.com/a.bin.manifest.sig".into(),
            sha256: SHA.into(),
            size: 4,
            notes: format!("What is new in {version}"),
        }
    }

    fn offer(releases: &[FirmwareRelease], running: &str) -> Option<String> {
        pick_offer(releases, running, "esp32s3", "tdeck-max").map(|i| releases[i].version.clone())
    }

    #[test]
    fn a_release_newer_than_what_runs_is_offered() {
        assert_eq!(offer(&[release("0.6.0")], "0.5.2"), Some("0.6.0".into()));
    }

    #[test]
    fn the_same_or_an_older_release_is_not_offered() {
        assert_eq!(offer(&[release("0.6.0")], "0.6.0"), None);
        assert_eq!(offer(&[release("0.5.0")], "0.6.0"), None);
    }

    #[test]
    fn a_laptop_build_counts_as_the_tag_it_was_built_from() {
        assert_eq!(offer(&[release("0.6.0")], "0.6.0-3-gabc1234-dirty"), None);
        assert_eq!(offer(&[release("0.6.1")], "0.6.0-3-gabc1234-dirty"), Some("0.6.1".into()));
        assert_eq!(offer(&[release("0.6.0")], "0.6.0-rc.1"), Some("0.6.0".into()));
        // An untagged build is older than any release.
        assert_eq!(offer(&[release("0.6.0")], "0.0.0-unknown"), Some("0.6.0".into()));
    }

    #[test]
    fn of_several_releases_the_newest_by_version_is_offered() {
        let releases = [release("0.7.0"), release("0.10.0"), release("0.9.0")];
        assert_eq!(offer(&releases, "0.6.0"), Some("0.10.0".into()));
    }

    #[test]
    fn a_release_for_another_chip_or_board_is_not_offered() {
        let c3 = FirmwareRelease { arch: "esp32c3".into(), ..release("0.9.0") };
        let pro = FirmwareRelease { compatible_boards: vec!["tdeck-pro".into()], ..release("0.8.0") };
        assert_eq!(offer(&[c3.clone(), pro.clone()], "0.6.0"), None);
        assert_eq!(offer(&[c3, pro, release("0.7.0")], "0.6.0"), Some("0.7.0".into()));
    }

    /// A catalog as build_catalog.py writes one: an app, then a firmware
    /// entry as release_firmware.py writes it.
    const CATALOG: &str = r#"{
  "version": 1,
  "generated": "2026-10-26T10:00:00Z",
  "entries": [
    {"type": "app", "id": "notes", "version": "1.0.3", "url": "https://x/notes.tap"},
    {
      "type": "firmware",
      "id": "thistle-os",
      "version": "0.6.0",
      "arch": "esp32s3",
      "compatible_boards": ["tdeck", "tdeck-max"],
      "url": "https://github.com/x/thistle-os/releases/download/v0.6.0/a.bin",
      "manifest_url": "https://github.com/x/thistle-os/releases/download/v0.6.0/a.bin.manifest",
      "manifest_sig_url": "https://github.com/x/thistle-os/releases/download/v0.6.0/a.bin.manifest.sig",
      "sha256": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
      "size_bytes": 3866624,
      "changelog": "Over the air updates.\n- Check now",
      "updated": "2026-10-26"
    }
  ]
}"#;

    #[test]
    fn the_catalogs_firmware_entries_are_read() {
        let releases = firmware_releases(CATALOG);
        assert_eq!(releases.len(), 1);
        let r = &releases[0];
        assert_eq!(r.version, "0.6.0");
        assert_eq!(r.arch, "esp32s3");
        assert_eq!(r.compatible_boards, vec!["tdeck".to_string(), "tdeck-max".to_string()]);
        assert!(r.url.ends_with("/a.bin"));
        assert!(r.manifest_url.ends_with("/a.bin.manifest"));
        assert!(r.manifest_sig_url.ends_with("/a.bin.manifest.sig"));
        assert_eq!(r.size, 3866624);
        assert_eq!(r.notes, "Over the air updates.\n- Check now");
    }

    #[test]
    fn a_firmware_entry_missing_what_an_install_needs_is_left_out() {
        let no_manifest = CATALOG.replace("\"manifest_url\"", "\"other\"");
        assert!(firmware_releases(&no_manifest).is_empty());
        let no_size = CATALOG.replace("\"size_bytes\": 3866624", "\"size_bytes\": 0");
        assert!(firmware_releases(&no_size).is_empty());
    }

    #[test]
    fn braces_and_quotes_inside_strings_do_not_split_an_entry() {
        let catalog = CATALOG.replace(
            "Over the air updates.",
            "Fixed {braces} and \\\"quotes\\\" and ]brackets[",
        );
        let releases = firmware_releases(&catalog);
        assert_eq!(releases.len(), 1);
        assert_eq!(releases[0].notes, "Fixed {braces} and \"quotes\" and ]brackets[\n- Check now");
        assert_eq!(releases[0].size, 3866624);
    }

    #[test]
    fn a_catalog_that_does_not_read_lists_no_firmware() {
        assert!(firmware_releases("").is_empty());
        assert!(firmware_releases("{\"entries\": 3}").is_empty());
        assert!(firmware_releases("<html>").is_empty());
    }

    // --- when to check -------------------------------------------------------

    const NOW: i64 = 1_793_000_000; // 2026-10-26
    const HOUR: i64 = 3600;

    fn due() -> CheckInputs {
        CheckInputs {
            network: true,
            busy: false,
            now_s: Some(NOW),
            last_check_s: Some(NOW - 25 * HOUR),
            attempted_ms_ago: None,
        }
    }

    #[test]
    fn a_check_is_due_a_day_after_the_last() {
        assert!(check_due(&due()));
        assert!(!check_due(&CheckInputs { last_check_s: Some(NOW - 23 * HOUR), ..due() }));
        assert!(check_due(&CheckInputs { last_check_s: Some(NOW - 24 * HOUR), ..due() }));
    }

    #[test]
    fn a_device_never_checked_checks_at_once() {
        assert!(check_due(&CheckInputs { last_check_s: None, ..due() }));
    }

    #[test]
    fn a_clock_set_back_since_the_last_check_is_no_reason_to_wait() {
        assert!(check_due(&CheckInputs { last_check_s: Some(NOW + 5 * HOUR), ..due() }));
    }

    #[test]
    fn nothing_is_checked_without_a_network_or_while_busy() {
        assert!(!check_due(&CheckInputs { network: false, ..due() }));
        assert!(!check_due(&CheckInputs { busy: true, ..due() }));
    }

    #[test]
    fn with_the_clock_unset_it_checks_once_a_boot() {
        let unset = CheckInputs { now_s: None, last_check_s: Some(NOW - HOUR), ..due() };
        assert!(check_due(&unset));
        assert!(!check_due(&CheckInputs { attempted_ms_ago: Some(1), ..unset.clone() }));
        assert!(!check_due(&CheckInputs { attempted_ms_ago: Some(48 * 3_600_000), ..unset }));
    }

    #[test]
    fn a_check_that_failed_is_tried_again_an_hour_later() {
        let failed = CheckInputs { attempted_ms_ago: Some(59 * 60_000), ..due() };
        assert!(!check_due(&failed));
        assert!(check_due(&CheckInputs { attempted_ms_ago: Some(RETRY_MS), ..failed }));
    }

    // --- mobile data ---------------------------------------------------------

    #[test]
    fn a_download_starts_at_once_on_wifi() {
        assert_eq!(may_download(Network::Free, false), Gate::Start);
        assert_eq!(may_download(Network::Free, true), Gate::Start);
    }

    #[test]
    fn on_mobile_data_a_download_waits_until_the_user_allows_it() {
        assert_eq!(may_download(Network::Mobile, false), Gate::WaitForConsent);
        assert_eq!(may_download(Network::Mobile, true), Gate::Start);
    }

    #[test]
    fn with_no_network_a_download_waits_for_one() {
        assert_eq!(may_download(Network::None, true), Gate::WaitForNetwork);
    }

    #[test]
    fn a_release_whose_version_does_not_read_is_not_offered() {
        assert_eq!(offer(&[release("latest")], "0.6.0"), None);
        assert_eq!(offer(&[release("0.7.0"), release("1.0")], "0.6.0"), Some("0.7.0".into()));
    }

    // --- the board profile ---------------------------------------------------

    #[test]
    fn a_profile_written_by_the_running_version_is_kept() {
        assert_eq!(board_profile(Some("0.6.0"), "0.6.0", true), Profile::Keep);
        assert_eq!(
            board_profile(Some("0.5.2-108-gabc1234-dirty"), "0.5.2-108-gabc1234-dirty", true),
            Profile::Keep
        );
    }

    #[test]
    fn a_profile_written_by_another_version_is_rewritten() {
        assert_eq!(board_profile(Some("0.5.2"), "0.6.0", true), Profile::Rewrite);
        // Going back is a version change too.
        assert_eq!(board_profile(Some("0.6.0"), "0.5.2", true), Profile::Rewrite);
        // A laptop build on the same tag is another build.
        assert_eq!(board_profile(Some("0.6.0"), "0.6.0-1-gabc1234", true), Profile::Rewrite);
    }

    #[test]
    fn a_profile_that_names_no_writer_is_rewritten() {
        // Written before profiles recorded their writer, or by hand.
        assert_eq!(board_profile(None, "0.6.0", true), Profile::Rewrite);
        assert_eq!(board_profile(Some(""), "0.6.0", true), Profile::Rewrite);
    }

    // --- going back ----------------------------------------------------------

    fn record(version: &str, security_version: u32) -> Option<SlotRecord<'_>> {
        Some(SlotRecord { version, security_version })
    }

    #[test]
    fn going_back_is_offered_to_a_slot_whose_record_is_its_image() {
        assert_eq!(go_back(record("0.5.2", 500), Some("0.5.2"), 500), Some("0.5.2".into()));
        assert_eq!(go_back(record("0.5.2", 600), Some("0.5.2"), 500), Some("0.5.2".into()));
        assert_eq!(go_back(record("0.5.2", 500), Some("0.5.2"), 0), Some("0.5.2".into()));
    }

    #[test]
    fn going_back_is_not_offered_below_the_floor() {
        assert_eq!(go_back(record("0.5.2", 499), Some("0.5.2"), 500), None);
    }

    #[test]
    fn going_back_is_not_offered_to_a_slot_whose_record_is_not_its_image() {
        // An install began erasing it, or wrote another version over it.
        assert_eq!(go_back(record("0.5.2", 500), Some("0.6.0"), 0), None);
        assert_eq!(go_back(record("0.5.2", 500), None, 0), None);
        // A slot with no record, or one cleared when its erase began.
        assert_eq!(go_back(None, Some("0.5.2"), 0), None);
        assert_eq!(go_back(record("", 0), Some(""), 0), None);
    }

    // --- app updates (ticket 09) ---------------------------------------------

    fn app(id: &str, version: &str, sequence: u32, min_os: &str) -> AppRelease {
        AppRelease {
            id: id.into(),
            version: version.into(),
            release_sequence: sequence,
            min_os: min_os.into(),
        }
    }

    fn installed(id: &str, sequence: u32) -> InstalledApp {
        InstalledApp { id: id.into(), release_sequence: sequence }
    }

    /// What is offered, as (id, version).
    fn app_offer(releases: &[AppRelease], apps: &[InstalledApp], running: &str) -> Vec<(String, String)> {
        app_offers(releases, apps, running)
            .into_iter()
            .map(|r| (r.id.clone(), r.version.clone()))
            .collect()
    }

    const NOTES: &str = "com.thistle.notes";

    #[test]
    fn an_app_release_with_a_higher_release_sequence_is_offered() {
        let releases = [app(NOTES, "1.0.3", 1_000_003, "0.3.0")];
        assert_eq!(
            app_offer(&releases, &[installed(NOTES, 1_000_000)], "0.6.0"),
            vec![(NOTES.to_string(), "1.0.3".to_string())]
        );
    }

    #[test]
    fn the_same_or_a_lower_release_sequence_is_not_offered() {
        let releases = [app(NOTES, "1.0.3", 1_000_003, "0.3.0")];
        assert!(app_offer(&releases, &[installed(NOTES, 1_000_003)], "0.6.0").is_empty());
        assert!(app_offer(&releases, &[installed(NOTES, 1_000_004)], "0.6.0").is_empty());
    }

    #[test]
    fn an_app_release_whose_min_os_is_above_the_running_version_is_not_offered() {
        let at = [app(NOTES, "1.0.3", 1_000_003, "0.6.0")];
        let above = [app(NOTES, "1.0.3", 1_000_003, "0.6.1")];
        let below = [app(NOTES, "1.0.3", 1_000_003, "0.5.0")];
        let notes = [installed(NOTES, 1_000_000)];
        assert_eq!(app_offer(&at, &notes, "0.6.0").len(), 1);
        assert!(app_offer(&above, &notes, "0.6.0").is_empty());
        assert_eq!(app_offer(&below, &notes, "0.6.0").len(), 1);
    }

    #[test]
    fn a_laptop_build_meets_the_min_os_its_tag_meets() {
        let notes = [installed(NOTES, 1)];
        let needs = |min_os: &str| [app(NOTES, "1.0.3", 2, min_os)];
        assert_eq!(app_offer(&needs("0.6.0"), &notes, "0.6.0-3-gabc1234-dirty").len(), 1);
        assert!(app_offer(&needs("0.6.1"), &notes, "0.6.0-3-gabc1234-dirty").is_empty());
        // A release candidate is below its release.
        assert!(app_offer(&needs("0.6.0"), &notes, "0.6.0-rc.1").is_empty());
        // An untagged build counts as 0.0.0.
        assert!(app_offer(&needs("0.1.0"), &notes, "0.0.0-unknown").is_empty());
        assert_eq!(app_offer(&needs(""), &notes, "0.0.0-unknown").len(), 1);
    }

    #[test]
    fn a_malformed_min_os_is_never_met() {
        let releases = [app(NOTES, "1.0.3", 2, "soon")];
        assert!(app_offer(&releases, &[installed(NOTES, 1)], "0.6.0").is_empty());
    }

    #[test]
    fn only_installed_apps_are_offered_each_its_newest_release_it_can_run() {
        let releases = [
            app(NOTES, "1.0.1", 1_000_001, "0.3.0"),
            app(NOTES, "1.1.0", 1_001_000, "9.0.0"),
            app(NOTES, "1.0.2", 1_000_002, "0.3.0"),
            app("com.thistle.weather", "2.0.0", 2_000_000, "0.3.0"),
            app("com.thistle.flashlight", "2.0.1", 2_000_001, "0.3.0"),
        ];
        let apps = [installed(NOTES, 1_000_000), installed("com.thistle.flashlight", 2_000_000)];
        assert_eq!(
            app_offer(&releases, &apps, "0.6.0"),
            vec![
                (NOTES.to_string(), "1.0.2".to_string()),
                ("com.thistle.flashlight".to_string(), "2.0.1".to_string()),
            ]
        );
    }

    const APP_CATALOG: &str = r#"{
  "version": 1,
  "entries": [
    {"id": "com.thistle.notes", "type": "app", "name": "Notes", "version": "1.0.3", "arch": "host",
     "release_sequence": 1000003, "min_os_version": "0.6.0", "package_url": "https://x/n.tap",
     "changelog": "Fixed {braces}"},
    {"id": "com.thistle.weather", "name": "Weather", "version": "1.0.0", "release_sequence": 1000000,
     "package_url": "https://x/w.tap"},
    {"id": "com.thistle.elsewhere", "type": "app", "version": "1.0.0", "arch": "esp32c6",
     "release_sequence": 1000000, "package_url": "https://x/e.tap"},
    {"id": "com.thistle.legacy", "type": "app", "version": "1.0.0", "url": "https://x/l.elf"},
    {"id": "kbd", "type": "driver", "version": "1.0.0", "release_sequence": 3},
    {"type": "firmware", "id": "thistle-os", "version": "0.6.0", "arch": "host"}
  ]
}"#;

    #[test]
    fn the_catalogs_app_releases_for_this_chip_are_read() {
        let releases = app_releases(APP_CATALOG, "host");
        assert_eq!(
            releases,
            vec![
                app(NOTES, "1.0.3", 1_000_003, "0.6.0"),
                // An entry with no type is an app, with no arch any chip's.
                app("com.thistle.weather", "1.0.0", 1_000_000, ""),
            ]
        );
        assert!(app_releases("<html>", "host").is_empty());
    }

    #[test]
    fn a_board_the_kernel_has_no_profile_for_keeps_its_own() {
        assert_eq!(board_profile(Some("0.5.2"), "0.6.0", false), Profile::Keep);
        assert_eq!(board_profile(None, "0.6.0", false), Profile::Keep);
    }
}
