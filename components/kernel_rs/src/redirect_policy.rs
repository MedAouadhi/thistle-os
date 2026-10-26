// SPDX-License-Identifier: BSD-3-Clause
//! Whether a download follows a redirect, decided without touching the
//! network.
//!
//! Every download follows redirects: the catalog, an artifact manifest and
//! its signature, a firmware, a package and its signature. A GitHub Release
//! asset's URL answers with a 302 to another host, so none of them could be
//! fetched otherwise.
//!
//! - At most [`MAX_REDIRECTS`] redirects; the next is refused.
//! - Never from HTTPS to HTTP. A download that starts on plain HTTP, as the
//!   simulator's fakes do, may redirect within HTTP, or up to HTTPS.
//! - Only to `http://` or `https://`.
//!
//! The host a download ends on is not trusted for anything: what is
//! installed is trusted for its signature and its hash alone.

/// The most redirects one download follows.
pub const MAX_REDIRECTS: u32 = 5;

/// Why a redirect is not followed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    /// [`MAX_REDIRECTS`] were followed already.
    TooMany,
    /// From `https://` to `http://`.
    Downgrade,
    /// To something other than `http://` or `https://`.
    Scheme,
}

impl Refusal {
    /// Why, for the log: a NUL-terminated string.
    pub fn reason(self) -> &'static [u8] {
        match self {
            Refusal::TooMany => b"more than 5 redirects\0",
            Refusal::Downgrade => b"a redirect from HTTPS to HTTP\0",
            Refusal::Scheme => b"a redirect to neither HTTP nor HTTPS\0",
        }
    }
}

/// Whether an answer's status is a redirect a download follows: 301, 302,
/// 303, 307 or 308, the ones ESP-IDF's client follows.
pub fn is_redirect(status: i32) -> bool {
    matches!(status, 301 | 302 | 303 | 307 | 308)
}

/// Whether the URL is HTTPS (true) or HTTP (false); None for anything else.
fn is_https(url: &str) -> Option<bool> {
    let scheme = url.split_once("://")?.0;
    if scheme.eq_ignore_ascii_case("https") {
        Some(true)
    } else if scheme.eq_ignore_ascii_case("http") {
        Some(false)
    } else {
        None
    }
}

/// Whether a download at `from` follows a redirect to `to`, having followed
/// `followed` already.
pub fn follow(from: &str, to: &str, followed: u32) -> Result<(), Refusal> {
    if followed >= MAX_REDIRECTS {
        return Err(Refusal::TooMany);
    }
    let to_https = is_https(to).ok_or(Refusal::Scheme)?;
    // A start that is neither counts as HTTPS: the stricter.
    if is_https(from).unwrap_or(true) && !to_https {
        return Err(Refusal::Downgrade);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const GITHUB: &str =
        "https://github.com/wan0net/thistle-apps/releases/download/notes-1.2.0/notes.tap";
    const ASSETS: &str =
        "https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sig=x";

    #[test]
    fn a_release_asset_redirect_to_another_host_is_followed() {
        assert_eq!(follow(GITHUB, ASSETS, 0), Ok(()));
    }

    #[test]
    fn the_fifth_redirect_is_followed_and_the_sixth_refused() {
        assert_eq!(follow(ASSETS, ASSETS, 4), Ok(()));
        assert_eq!(follow(ASSETS, ASSETS, 5), Err(Refusal::TooMany));
    }

    #[test]
    fn https_never_redirects_to_http() {
        assert_eq!(
            follow(GITHUB, "http://release-assets.githubusercontent.com/a", 0),
            Err(Refusal::Downgrade)
        );
        assert_eq!(follow("HTTPS://a.example/x", "Http://a.example/y", 0), Err(Refusal::Downgrade));
    }

    #[test]
    fn plain_http_may_stay_on_http_or_move_up_to_https() {
        assert_eq!(follow("http://127.0.0.1:8000/a.tap", "http://127.0.0.1:8001/a.tap", 0), Ok(()));
        assert_eq!(follow("http://127.0.0.1:8000/a.tap", ASSETS, 0), Ok(()));
    }

    #[test]
    fn a_redirect_off_the_web_is_refused() {
        assert_eq!(follow(GITHUB, "file:///sdcard/apps/x.tap", 0), Err(Refusal::Scheme));
        assert_eq!(follow("http://a.example/x", "ftp://a.example/x", 0), Err(Refusal::Scheme));
        assert_eq!(follow(GITHUB, "", 0), Err(Refusal::Scheme));
    }

    #[test]
    fn too_many_is_said_before_anything_else() {
        assert_eq!(follow(GITHUB, "http://a.example/x", 5), Err(Refusal::TooMany));
    }

    #[test]
    fn only_the_followed_statuses_are_redirects() {
        for status in [301, 302, 303, 307, 308] {
            assert!(is_redirect(status), "{status}");
        }
        for status in [0, 200, 206, 300, 304, 305, 306, 400, 404, 500] {
            assert!(!is_redirect(status), "{status}");
        }
    }
}
