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

#[cfg(test)]
mod tests {
    use super::*;

    const SHA: &str = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";

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

    #[test]
    fn a_board_the_kernel_has_no_profile_for_keeps_its_own() {
        assert_eq!(board_profile(Some("0.5.2"), "0.6.0", false), Profile::Keep);
        assert_eq!(board_profile(None, "0.6.0", false), Profile::Keep);
    }
}
