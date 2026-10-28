// SPDX-License-Identifier: BSD-3-Clause
// The firmware's version, and reading and comparing versions.
//
// The firmware's version is its tag. The build takes `git describe --tags
// --match 'v[0-9]*' --dirty` (build.rs, or the firmware's PROJECT_VER passed
// in by kernel_rs/CMakeLists.txt) and the kernel reports it, leading `v`
// dropped, wherever it reports a version. A laptop build reads as, say,
// `0.5.2-108-gabc123-dirty` and counts as `0.5.2` when versions are compared.

use core::cmp::Ordering;

/// Monotonic signed-release counter. Never decrease or reuse this value for a
/// release that contains different firmware bytes. The release workflow
/// writes it into the artifact manifest; raise it by hand in a release that
/// fixes a hole.
pub const SECURITY_VERSION: u32 = 500;

/// A release version: `MAJOR.MINOR.PATCH`, perhaps with a prerelease such as
/// `rc.1`. A `git describe` suffix is not part of it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Version {
    pub major: u64,
    pub minor: u64,
    pub patch: u64,
    /// `rc.1` for `0.6.0-rc.1`; empty for a release.
    pub pre: String,
}

impl Version {
    /// Reads a tag (`v0.6.0`, `0.6.0-rc.1`) or a `git describe` string
    /// (`0.5.2-108-gabc123-dirty`), which counts as the tag it names. Refuses
    /// anything else, rather than reading it as 0.0.0.
    pub fn parse(text: &str) -> Option<Version> {
        let text = text.strip_prefix('v').unwrap_or(text);
        let text = text.strip_suffix("-dirty").unwrap_or(text);
        let text = strip_describe_suffix(text);
        let (core, pre) = match text.split_once('-') {
            Some((core, pre)) => (core, Some(pre)),
            None => (text, None),
        };
        let mut numbers = core.split('.');
        let major = number(numbers.next()?)?;
        let minor = number(numbers.next()?)?;
        let patch = number(numbers.next()?)?;
        if numbers.next().is_some() {
            return None;
        }
        let pre = match pre {
            None => String::new(),
            Some(pre) if pre.split('.').all(identifier) => pre.to_string(),
            Some(_) => return None,
        };
        Some(Version { major, minor, patch, pre })
    }
}

/// Drops describe's `-<commits>-g<hash>`, if the text ends in one.
fn strip_describe_suffix(text: &str) -> &str {
    let mut parts = text.rsplitn(3, '-');
    let (Some(hash), Some(count), Some(rest)) = (parts.next(), parts.next(), parts.next()) else {
        return text;
    };
    let is_hash = hash
        .strip_prefix('g')
        .is_some_and(|h| !h.is_empty() && h.bytes().all(|b| b.is_ascii_hexdigit()));
    let is_count = !count.is_empty() && count.bytes().all(|b| b.is_ascii_digit());
    if is_hash && is_count {
        rest
    } else {
        text
    }
}

/// A version number: digits, no leading zero.
fn number(text: &str) -> Option<u64> {
    if text.is_empty() || !text.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    if text.len() > 1 && text.starts_with('0') {
        return None;
    }
    text.parse().ok()
}

/// A prerelease identifier: letters and digits, a number with no leading
/// zero. Hyphens, which semver allows, would be taken for describe's.
fn identifier(text: &str) -> bool {
    if text.is_empty() || !text.bytes().all(|b| b.is_ascii_alphanumeric()) {
        return false;
    }
    !(text.bytes().all(|b| b.is_ascii_digit()) && text.len() > 1 && text.starts_with('0'))
}

fn compare_identifiers(a: &str, b: &str) -> Ordering {
    let numeric = |s: &str| s.bytes().all(|c| c.is_ascii_digit());
    match (numeric(a), numeric(b)) {
        // No leading zeros, so the longer number is the larger.
        (true, true) => a.len().cmp(&b.len()).then_with(|| a.cmp(b)),
        (true, false) => Ordering::Less,
        (false, true) => Ordering::Greater,
        (false, false) => a.cmp(b),
    }
}

impl Ord for Version {
    /// Semver precedence: a prerelease is below its release.
    fn cmp(&self, other: &Self) -> Ordering {
        let numbers = (self.major, self.minor, self.patch).cmp(&(other.major, other.minor, other.patch));
        if numbers != Ordering::Equal {
            return numbers;
        }
        match (self.pre.is_empty(), other.pre.is_empty()) {
            (true, true) => Ordering::Equal,
            (true, false) => Ordering::Greater,
            (false, true) => Ordering::Less,
            (false, false) => {
                let mut a = self.pre.split('.');
                let mut b = other.pre.split('.');
                loop {
                    match (a.next(), b.next()) {
                        (None, None) => return Ordering::Equal,
                        (None, Some(_)) => return Ordering::Less,
                        (Some(_), None) => return Ordering::Greater,
                        (Some(x), Some(y)) => match compare_identifiers(x, y) {
                            Ordering::Equal => continue,
                            order => return order,
                        },
                    }
                }
            }
        }
    }
}

impl PartialOrd for Version {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

/// Whether `running` meets the requirement `requirement` (an app's `min_os`):
/// an empty requirement always does, a malformed one never does.
pub fn meets(running: &Version, requirement: &str) -> bool {
    if requirement.is_empty() {
        return true;
    }
    Version::parse(requirement).is_some_and(|required| *running >= required)
}

/// The firmware's version as the build described it: `0.6.0` for a release,
/// `0.5.2-108-gabc123-dirty` for a laptop build. Shown, never compared.
pub fn running() -> &'static str {
    env!("THISTLE_FW_VERSION")
}

/// The same, NUL-terminated, for C.
pub fn running_c() -> *const core::ffi::c_char {
    concat!(env!("THISTLE_FW_VERSION"), "\0").as_ptr() as *const core::ffi::c_char
}

/// The release the running firmware counts as. A build whose describe string
/// is not a version (no tag in reach) counts as 0.0.0.
pub fn running_version() -> Version {
    Version::parse(running()).unwrap_or(Version {
        major: 0,
        minor: 0,
        patch: 0,
        pre: String::new(),
    })
}

/// Whether the running firmware meets an app's `min_os`.
pub fn satisfies(requirement: &str) -> bool {
    meets(&running_version(), requirement)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(text: &str) -> Version {
        Version::parse(text).unwrap_or_else(|| panic!("{text} should parse"))
    }

    #[test]
    fn a_plain_tag_reads_with_or_without_its_v() {
        let expected = Version { major: 0, minor: 6, patch: 0, pre: String::new() };
        assert_eq!(v("v0.6.0"), expected);
        assert_eq!(v("0.6.0"), expected);
        assert_eq!(v("12.34.567").major, 12);
        assert_eq!(v("12.34.567").patch, 567);
    }

    #[test]
    fn a_release_candidate_keeps_its_suffix() {
        assert_eq!(v("v0.6.0-rc.1").pre, "rc.1");
        assert_eq!(v("0.6.0-rc.1").minor, 6);
    }

    #[test]
    fn a_laptop_build_counts_as_the_tag_it_was_built_from() {
        assert_eq!(v("0.5.2-108-gabc123-dirty"), v("0.5.2"));
        assert_eq!(v("v0.5.2-108-gabc123"), v("0.5.2"));
        assert_eq!(v("0.5.2-dirty"), v("0.5.2"));
        assert_eq!(v("0.6.0-rc.1-3-gabc"), v("0.6.0-rc.1"));
        assert_eq!(v("0.6.0-rc.1-3-gabc-dirty"), v("0.6.0-rc.1"));
        assert_eq!(v("0.6.0-rc.1-dirty"), v("0.6.0-rc.1"));
    }

    #[test]
    fn malformed_versions_are_refused() {
        for bad in [
            "", "v", "0.6", "0.6.0.1", "a.b.c", "0.6.x", "-1.0.0", "0..0",
            "0.6.0-", "0.6.0-rc..1", "0.6.0-rc.1+build", " 0.6.0", "0.6.0 ",
            "abc1234", "0.6.0-3-gzzz", "vv0.6.0", "0.06.0", "1.0.0-01",
        ] {
            assert_eq!(Version::parse(bad), None, "{bad:?} should be refused");
        }
    }

    #[test]
    fn versions_order_by_number_not_by_text() {
        assert!(v("0.10.0") > v("0.9.9"));
        assert!(v("1.0.0") > v("0.99.99"));
        assert!(v("0.6.1") > v("0.6.0"));
        assert_eq!(v("0.6.0").cmp(&v("v0.6.0")), Ordering::Equal);
    }

    #[test]
    fn a_release_candidate_is_below_its_release() {
        assert!(v("0.6.0-rc.1") < v("0.6.0"));
        assert!(v("0.6.0-rc.1") > v("0.5.9"));
        assert!(v("0.6.0-rc.2") > v("0.6.0-rc.1"));
        assert!(v("0.6.0-rc.10") > v("0.6.0-rc.2"));
        assert!(v("0.6.0-rc.1") > v("0.6.0-beta.5"));
        assert!(v("0.6.0-rc") < v("0.6.0-rc.1"));
        // Numeric identifiers sort below alphanumeric ones (semver 11.4.3).
        assert!(v("0.6.0-1") < v("0.6.0-alpha"));
    }

    #[test]
    fn a_laptop_build_is_not_newer_than_its_tag() {
        assert_eq!(v("0.5.2-108-gabc123-dirty").cmp(&v("0.5.2")), Ordering::Equal);
        assert!(v("0.5.2-108-gabc123") < v("0.6.0-rc.1"));
    }

    #[test]
    fn min_os_is_met_by_that_version_or_newer() {
        let running = v("0.6.0");
        assert!(meets(&running, "0.6.0"));
        assert!(meets(&running, "0.3.0"));
        assert!(meets(&running, "v0.5.9"));
        assert!(!meets(&running, "0.6.1"));
        assert!(!meets(&running, "1.0.0"));
    }

    #[test]
    fn a_release_candidate_does_not_meet_its_own_release() {
        assert!(!meets(&v("0.6.0-rc.1"), "0.6.0"));
        assert!(meets(&v("0.6.0-rc.1"), "0.5.2"));
        assert!(meets(&v("0.6.0-rc.1-3-gabc"), "0.6.0-rc.1"));
    }

    #[test]
    fn a_laptop_build_meets_what_its_tag_meets() {
        let laptop = v("0.5.2-108-gabc123-dirty");
        assert!(meets(&laptop, "0.5.2"));
        assert!(!meets(&laptop, "0.5.3"));
    }

    #[test]
    fn no_requirement_is_met_and_a_malformed_one_is_not() {
        assert!(meets(&v("0.0.1"), ""));
        assert!(!meets(&v("9.9.9"), "latest"));
        assert!(!meets(&v("9.9.9"), "1.0"));
    }

    #[test]
    fn the_build_reports_a_firmware_version() {
        // build.rs takes it from tools/firmware_version.sh: a describe of a
        // `v<digit>` tag, or 0.0.0-unknown. Either reads as a version.
        assert!(!running().starts_with('v'), "{}", running());
        assert!(Version::parse(running()).is_some(), "{}", running());
        let c = unsafe { core::ffi::CStr::from_ptr(running_c()) };
        assert_eq!(c.to_str().unwrap(), running());
    }

    #[test]
    fn security_version_is_nonzero() {
        assert!(SECURITY_VERSION > 0);
    }
}
