// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS Kernel — kernel slots
//
// The flash holds two kernel slots, `kernel_a` and `kernel_b`, and a slot
// reserved for Recovery (partitions.csv, ADR-0010). A kernel is updated by
// writing the *other* kernel slot, named by its label: ESP-IDF's "next update
// partition" from `kernel_b` would be the reserved slot.
//
// What is decided here is pure and tested on the host: which slot is the
// other one, when a new kernel is kept, the version an image describes, and,
// for the simulator, what ESP-IDF's bootloader does with its otadata at a
// reset. ota.rs runs them.

use std::time::Duration;

pub const KERNEL_A: &str = "kernel_a";
pub const KERNEL_B: &str = "kernel_b";

/// How long a new kernel runs before it is kept. Until then a reset rolls it
/// back to the other kernel slot.
pub const KEEP_DELAY: Duration = Duration::from_secs(60);

/// The kernel slot an update from `running` is written into: always the other
/// kernel slot, never the reserved one. None when `running` is no kernel slot
/// (an old table's `factory`, or the reserved slot).
pub fn other_kernel_slot(running: &str) -> Option<&'static str> {
    match running {
        KERNEL_A => Some(KERNEL_B),
        KERNEL_B => Some(KERNEL_A),
        _ => None,
    }
}

fn index(slot: &str) -> Option<usize> {
    match slot {
        KERNEL_A => Some(0),
        KERNEL_B => Some(1),
        _ => None,
    }
}

const SLOTS: [&str; 2] = [KERNEL_A, KERNEL_B];

/// What to do about keeping the running kernel.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Keep {
    /// The kernel is not new: it was kept before, or flashed by USB.
    NothingToKeep,
    /// New, and not yet run for long enough: ask again after this long.
    Wait(Duration),
    /// New, and run for long enough: keep it now.
    Now,
}

/// Whether a kernel that is `pending` verification and has run for
/// `running_for` is kept now, given the keep `delay`.
pub fn keep(pending: bool, running_for: Duration, delay: Duration) -> Keep {
    if !pending {
        Keep::NothingToKeep
    } else if running_for >= delay {
        Keep::Now
    } else {
        Keep::Wait(delay - running_for)
    }
}

/// Where esp_app_desc_t sits in an app image: after the 24-byte image header
/// and the first segment's 8-byte header.
const APP_DESC_OFFSET: usize = 32;
const APP_DESC_MAGIC: u32 = 0xABCD_5432;
const IMAGE_MAGIC: u8 = 0xE9;
/// esp_app_desc_t.version: after magic, secure_version and two reserved words.
const VERSION_OFFSET: usize = APP_DESC_OFFSET + 16;
const VERSION_LEN: usize = 32;

/// How many bytes from an image's start image_version() needs.
pub const IMAGE_VERSION_BYTES: usize = VERSION_OFFSET + VERSION_LEN;

/// The version an ESP-IDF app image describes (its `esp_app_desc_t`), if the
/// bytes start like one.
pub fn image_version(image: &[u8]) -> Option<String> {
    if image.len() < IMAGE_VERSION_BYTES || image[0] != IMAGE_MAGIC {
        return None;
    }
    let magic = u32::from_le_bytes(image[APP_DESC_OFFSET..APP_DESC_OFFSET + 4].try_into().ok()?);
    if magic != APP_DESC_MAGIC {
        return None;
    }
    let field = &image[VERSION_OFFSET..VERSION_OFFSET + VERSION_LEN];
    let end = field.iter().position(|&b| b == 0).unwrap_or(VERSION_LEN);
    let version = std::str::from_utf8(&field[..end]).ok()?.trim();
    (!version.is_empty()).then(|| version.to_string())
}

/// The state ESP-IDF keeps in otadata for the selected kernel slot.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Image {
    /// Just written and selected; not yet booted.
    New,
    /// Booted once and not yet kept. A reset now rolls it back.
    Pending,
    /// Kept, or flashed by USB.
    Valid,
}

/// What a reset booted.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Boot {
    pub slot: &'static str,
    /// The kernel booted is new and has to be kept.
    pub pending: bool,
    /// The slot whose new kernel was not kept, if this reset rolled it back.
    pub rolled_back_from: Option<&'static str>,
}

/// The simulator's otadata and the versions written into each kernel slot,
/// kept as a small text file beside the slots' images.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Slots {
    pub boot: &'static str,
    pub image: Image,
    versions: [Option<String>; 2],
    /// Erased by an install that began writing it and has not finished.
    erased: [bool; 2],
}

/// What a kernel slot holds, as its image description would say.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Contents<'a> {
    /// Never written by an install: in the simulator, the build's kernel.
    Unwritten,
    /// An image of this version.
    Written(&'a str),
    /// Erased, or half written: no image to describe.
    Erased,
}

impl Default for Slots {
    /// As `idf.py flash` leaves a board: the first kernel slot, kept.
    fn default() -> Self {
        Slots { boot: KERNEL_A, image: Image::Valid, versions: [None, None], erased: [false; 2] }
    }
}

impl Slots {
    /// Read the file's text; anything unreadable reads as a fresh flash.
    pub fn parse(text: &str) -> Slots {
        let mut slots = Slots::default();
        let mut boot = None;
        let mut image = None;
        for line in text.lines() {
            let Some((key, value)) = line.split_once('=') else { continue };
            let value = value.trim();
            match key.trim() {
                "boot" => boot = index(value),
                "erased" => {
                    if let Some(i) = index(value) {
                        slots.erased[i] = true;
                    }
                }
                "image" => {
                    image = match value {
                        "new" => Some(Image::New),
                        "pending" => Some(Image::Pending),
                        "valid" => Some(Image::Valid),
                        _ => None,
                    }
                }
                slot => {
                    if let (Some(i), false) = (index(slot), value.is_empty()) {
                        slots.versions[i] = Some(value.to_string());
                    }
                }
            }
        }
        match (boot, image) {
            (Some(i), Some(image)) => {
                slots.boot = SLOTS[i];
                slots.image = image;
                slots
            }
            _ => Slots::default(),
        }
    }

    pub fn to_text(&self) -> String {
        let image = match self.image {
            Image::New => "new",
            Image::Pending => "pending",
            Image::Valid => "valid",
        };
        let mut text = format!("boot={}\nimage={}\n", self.boot, image);
        for (slot, version) in SLOTS.iter().zip(&self.versions) {
            if let Some(version) = version {
                text.push_str(&format!("{slot}={version}\n"));
            }
        }
        for (slot, erased) in SLOTS.iter().zip(&self.erased) {
            if *erased {
                text.push_str(&format!("erased={slot}\n"));
            }
        }
        text
    }

    /// What ESP-IDF's bootloader does at a reset: a new kernel boots pending,
    /// a pending one that was not kept is rolled back to the other slot.
    pub fn reset(&mut self) -> Boot {
        match self.image {
            Image::New => {
                self.image = Image::Pending;
                Boot { slot: self.boot, pending: true, rolled_back_from: None }
            }
            Image::Pending => {
                let from = self.boot;
                self.boot = other_kernel_slot(from).unwrap_or(KERNEL_A);
                self.image = Image::Valid;
                Boot { slot: self.boot, pending: false, rolled_back_from: Some(from) }
            }
            Image::Valid => Boot { slot: self.boot, pending: false, rolled_back_from: None },
        }
    }

    /// `slot` now holds an image of `version`, selected to boot next.
    pub fn install(&mut self, slot: &'static str, version: Option<String>) {
        let Some(i) = index(slot) else { return };
        self.versions[i] = version;
        self.erased[i] = false;
        self.boot = SLOTS[i];
        self.image = Image::New;
    }

    /// An install has begun writing `slot`: what it held is gone.
    pub fn erase(&mut self, slot: &str) {
        let Some(i) = index(slot) else { return };
        self.versions[i] = None;
        self.erased[i] = true;
    }

    /// Going back: `slot`, as it is, boots next, new, to be kept or rolled
    /// back as an install is.
    pub fn select(&mut self, slot: &str) {
        let Some(i) = index(slot) else { return };
        self.boot = SLOTS[i];
        self.image = Image::New;
    }

    /// What `slot` holds.
    pub fn contents(&self, slot: &str) -> Contents<'_> {
        match index(slot) {
            None => Contents::Erased,
            Some(i) if self.erased[i] => Contents::Erased,
            Some(i) => self.versions[i].as_deref().map_or(Contents::Unwritten, Contents::Written),
        }
    }

    /// The running kernel is kept: a reset boots it again.
    pub fn keep(&mut self) {
        if self.image == Image::Pending {
            self.image = Image::Valid;
        }
    }

    /// The version recorded for `slot` when it was written, if any.
    pub fn version(&self, slot: &str) -> Option<&str> {
        index(slot).and_then(|i| self.versions[i].as_deref())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // --- the other kernel slot ---------------------------------------------

    #[test]
    fn an_update_from_either_kernel_slot_writes_the_other() {
        assert_eq!(other_kernel_slot("kernel_a"), Some("kernel_b"));
        assert_eq!(other_kernel_slot("kernel_b"), Some("kernel_a"));
    }

    #[test]
    fn nothing_is_written_from_outside_a_kernel_slot() {
        // The reserved slot is never written, and an old table's factory
        // kernel has no other slot to write.
        assert_eq!(other_kernel_slot("recovery"), None);
        assert_eq!(other_kernel_slot("factory"), None);
        assert_eq!(other_kernel_slot("ota_0"), None);
        assert_eq!(other_kernel_slot(""), None);
    }

    // --- keeping a new kernel -----------------------------------------------

    #[test]
    fn a_kernel_that_is_not_new_has_nothing_to_keep() {
        assert_eq!(keep(false, Duration::ZERO, KEEP_DELAY), Keep::NothingToKeep);
        assert_eq!(keep(false, Duration::from_secs(600), KEEP_DELAY), Keep::NothingToKeep);
    }

    #[test]
    fn a_new_kernel_waits_out_the_rest_of_its_first_minute() {
        assert_eq!(
            keep(true, Duration::from_secs(15), KEEP_DELAY),
            Keep::Wait(Duration::from_secs(45))
        );
    }

    #[test]
    fn a_new_kernel_is_kept_once_it_has_run_for_the_delay() {
        assert_eq!(keep(true, Duration::from_secs(60), KEEP_DELAY), Keep::Now);
        assert_eq!(keep(true, Duration::from_secs(61), KEEP_DELAY), Keep::Now);
    }

    // --- the version an image describes ------------------------------------

    /// The start of an ESP-IDF app image: a 24-byte image header, an 8-byte
    /// segment header, then esp_app_desc_t (magic, secure_version, two
    /// reserved words, then version[32]).
    fn image_with_version(version: &str) -> Vec<u8> {
        let mut image = vec![0u8; 256];
        image[0] = 0xE9;
        image[32..36].copy_from_slice(&0xABCD_5432u32.to_le_bytes());
        image[48..48 + version.len()].copy_from_slice(version.as_bytes());
        image
    }

    #[test]
    fn an_image_says_its_version() {
        assert_eq!(image_version(&image_with_version("0.6.0")).as_deref(), Some("0.6.0"));
        assert_eq!(
            image_version(&image_with_version("0.6.0-3-gabc1234-dirty")).as_deref(),
            Some("0.6.0-3-gabc1234-dirty")
        );
    }

    #[test]
    fn bytes_that_are_no_app_image_have_no_version() {
        assert_eq!(image_version(b"not a firmware"), None);
        let mut no_desc = image_with_version("0.6.0");
        no_desc[32] = 0;
        assert_eq!(image_version(&no_desc), None);
        let mut no_header = image_with_version("0.6.0");
        no_header[0] = 0;
        assert_eq!(image_version(&no_header), None);
    }

    // --- the simulator's bootloader -----------------------------------------

    #[test]
    fn a_fresh_flash_boots_the_first_kernel_slot_with_nothing_to_keep() {
        let mut slots = Slots::parse("");
        assert_eq!(
            slots.reset(),
            Boot { slot: "kernel_a", pending: false, rolled_back_from: None }
        );
    }

    #[test]
    fn an_installed_kernel_boots_pending_after_the_restart() {
        let mut slots = Slots::default();
        slots.install("kernel_b", Some("0.6.0".into()));
        assert_eq!(
            slots.reset(),
            Boot { slot: "kernel_b", pending: true, rolled_back_from: None }
        );
        assert_eq!(slots.version("kernel_b"), Some("0.6.0"));
        assert_eq!(slots.version("kernel_a"), None);
    }

    #[test]
    fn a_reset_before_the_keep_rolls_back_to_the_other_slot() {
        let mut slots = Slots::default();
        slots.install("kernel_b", Some("0.6.0".into()));
        slots.reset();
        assert_eq!(
            slots.reset(),
            Boot { slot: "kernel_a", pending: false, rolled_back_from: Some("kernel_b") }
        );
        // And it stays there.
        assert_eq!(
            slots.reset(),
            Boot { slot: "kernel_a", pending: false, rolled_back_from: None }
        );
    }

    #[test]
    fn a_kept_kernel_boots_again_after_a_reset() {
        let mut slots = Slots::default();
        slots.install("kernel_b", Some("0.6.0".into()));
        slots.reset();
        slots.keep();
        assert_eq!(
            slots.reset(),
            Boot { slot: "kernel_b", pending: false, rolled_back_from: None }
        );
    }

    #[test]
    fn the_slots_survive_being_written_down_and_read_back() {
        let mut slots = Slots::default();
        slots.install("kernel_b", Some("0.6.0".into()));
        slots.reset();
        let read_back = Slots::parse(&slots.to_text());
        assert_eq!(read_back, slots);
        // A run that ended before the keep: the next one rolls back.
        let mut next_run = read_back;
        assert_eq!(next_run.reset().rolled_back_from, Some("kernel_b"));
    }

    #[test]
    fn an_erased_slot_describes_no_image_until_it_is_written() {
        let mut slots = Slots::default();
        slots.install("kernel_b", Some("0.6.0".into()));
        slots.reset();
        slots.keep();
        assert_eq!(slots.contents("kernel_a"), Contents::Unwritten);
        assert_eq!(slots.contents("kernel_b"), Contents::Written("0.6.0"));
        slots.erase("kernel_a");
        assert_eq!(slots.contents("kernel_a"), Contents::Erased);
        // The erase survives being written down; the boot choice is as it was.
        let mut read_back = Slots::parse(&slots.to_text());
        assert_eq!(read_back, slots);
        assert_eq!(read_back.reset().slot, "kernel_b");
        read_back.install("kernel_a", Some("0.7.0".into()));
        assert_eq!(read_back.contents("kernel_a"), Contents::Written("0.7.0"));
    }

    #[test]
    fn going_back_selects_the_other_slot_as_it_is_and_boots_it_pending() {
        let mut slots = Slots::default();
        slots.install("kernel_b", Some("0.6.0".into()));
        slots.reset();
        slots.keep();
        slots.select("kernel_a");
        assert_eq!(slots.reset(), Boot { slot: "kernel_a", pending: true, rolled_back_from: None });
        assert_eq!(slots.contents("kernel_b"), Contents::Written("0.6.0"));
        // A run that ends before the keep returns to the version it left.
        assert_eq!(slots.reset().slot, "kernel_b");
    }

    #[test]
    fn unreadable_slots_read_as_a_fresh_flash() {
        assert_eq!(Slots::parse("boot=recovery\nimage=bogus\n"), Slots::default());
    }
}
