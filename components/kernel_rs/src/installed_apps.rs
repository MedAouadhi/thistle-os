// SPDX-License-Identifier: BSD-3-Clause
//! The apps on the SD card as the App Store's Installed tab lists and removes
//! them, which are the ones elf_loader's boot scan loads (`app_elf_paths`):
//! each `NAME.app.elf` in the apps folder, and each package the App Store
//! installed (tap_installer), `ID/` holding the release `ID/active.json` names.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use std::path::Path;

use crate::appstore_client::{card_root, copy_str_to_buf};
use crate::manifest::Manifest;

const ESP_OK: i32 = 0;
const ESP_ERR_INVALID_ARG: i32 = 0x102;
const ESP_ERR_NOT_FOUND: i32 = 0x105;
const ESP_FAIL: i32 = -1;

/// One installed app. Matches installed_app_t in appstore_client.h.
#[repr(C)]
#[derive(Clone)]
pub struct InstalledApp {
    /// What appstore_remove_installed() takes: the package's folder, or the
    /// ELF's name without `.app.elf`.
    pub key: [u8; 64],
    pub id: [u8; 64],
    pub name: [u8; 64],
    pub version: [u8; 16],
    pub size_bytes: u32,
    /// Installed by the App Store as a package, rather than copied on as an ELF.
    pub is_package: bool,
}

impl InstalledApp {
    fn new(key: &str, manifest: Option<Manifest>, size_bytes: u64, is_package: bool) -> Self {
        let mut app = InstalledApp {
            key: [0; 64],
            id: [0; 64],
            name: [0; 64],
            version: [0; 16],
            size_bytes: size_bytes.min(u32::MAX as u64) as u32,
            is_package,
        };
        let manifest = manifest.unwrap_or_default();
        let id = if manifest.id.is_empty() { key } else { manifest.id.as_str() };
        let name = if manifest.name.is_empty() { id } else { manifest.name.as_str() };
        copy_str_to_buf(key, &mut app.key);
        copy_str_to_buf(id, &mut app.id);
        copy_str_to_buf(name, &mut app.name);
        copy_str_to_buf(&manifest.version, &mut app.version);
        app
    }

    fn name_str(&self) -> String {
        let end = self.name.iter().position(|&b| b == 0).unwrap_or(self.name.len());
        String::from_utf8_lossy(&self.name[..end]).to_lowercase()
    }
}

/// A name in the apps folder that appstore_remove_installed() may act on:
/// one component, not hidden, short enough for InstalledApp::key.
fn valid_key(key: &str) -> bool {
    !key.is_empty()
        && key.len() < 64
        && !key.starts_with('.')
        && !key.contains(['/', '\\', '\0'])
}

/// The apps in `apps`, by name.
pub fn list_installed(apps: &Path) -> Vec<InstalledApp> {
    let mut found = Vec::new();
    let Ok(dir) = std::fs::read_dir(apps) else { return found };
    for entry in dir.flatten() {
        let path = entry.path();
        let file_name = entry.file_name();
        let file_name = file_name.to_string_lossy();
        if let Some(stem) = file_name.strip_suffix(".app.elf") {
            if !valid_key(stem) || !path.is_file() {
                continue;
            }
            let manifest = Manifest::from_file(Path::new(&Manifest::path_from_elf(&path.to_string_lossy()))).ok();
            let size = path.metadata().map(|m| m.len()).unwrap_or(0);
            found.push(InstalledApp::new(stem, manifest, size, false));
        } else if valid_key(&file_name) && path.is_dir() {
            let Some(generation) = crate::tap_installer::active_generation(&path) else { continue };
            let manifest = Manifest::from_file(&generation.join("package.json")).ok();
            let size = generation.join("app.app.elf").metadata().map(|m| m.len()).unwrap_or(0);
            found.push(InstalledApp::new(&file_name, manifest, size, true));
        }
    }
    found.sort_by_cached_key(InstalledApp::name_str);
    found
}

/// An app the App Store installed, as update_policy weighs an update to it.
pub(crate) type InstalledRelease = crate::update_policy::InstalledApp;

/// The packages the App Store installed in `apps`: each one's id (its
/// folder) and the release it runs. ELFs copied on have no release.
pub(crate) fn installed_releases(apps: &Path) -> Vec<InstalledRelease> {
    let Ok(dir) = std::fs::read_dir(apps) else { return Vec::new() };
    dir.flatten()
        .filter_map(|entry| {
            let id = entry.file_name().to_string_lossy().into_owned();
            if !valid_key(&id) || !entry.path().is_dir() {
                return None;
            }
            let release_sequence = crate::tap_installer::active_release_sequence(&entry.path())?;
            Some(InstalledRelease { id, release_sequence })
        })
        .collect()
}

/// Remove the app `key` names in `apps`: a package's whole folder, or an ELF
/// with its signature and manifest.
pub fn remove_installed(apps: &Path, key: &str) -> Result<(), i32> {
    if !valid_key(key) {
        return Err(ESP_ERR_INVALID_ARG);
    }
    let folder = apps.join(key);
    if folder.is_dir() {
        return std::fs::remove_dir_all(&folder).map_err(|_| ESP_FAIL);
    }
    let elf = apps.join(format!("{key}.app.elf"));
    if !elf.is_file() {
        return Err(ESP_ERR_NOT_FOUND);
    }
    std::fs::remove_file(&elf).map_err(|_| ESP_FAIL)?;
    let _ = std::fs::remove_file(apps.join(format!("{key}.app.elf.sig")));
    let _ = std::fs::remove_file(Manifest::path_from_elf(&elf.to_string_lossy()));
    Ok(())
}

/// Fill `out` with up to `max` of the SD card's apps, by name.
///
/// # Safety
/// `out` must point to `max` InstalledApp; `out_count` must be valid.
#[no_mangle]
pub unsafe extern "C" fn appstore_list_installed(
    out: *mut InstalledApp,
    max: c_int,
    out_count: *mut c_int,
) -> i32 {
    if out.is_null() || out_count.is_null() || max <= 0 {
        return ESP_ERR_INVALID_ARG;
    }
    let apps = list_installed(&card_root().join("apps"));
    let count = apps.len().min(max as usize);
    let out = std::slice::from_raw_parts_mut(out, count);
    out.clone_from_slice(&apps[..count]);
    *out_count = count as c_int;
    ESP_OK
}

/// Remove the SD card's app `key` names (InstalledApp::key). It stays in the
/// launcher until the next restart.
///
/// # Safety
/// `key` must be a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn appstore_remove_installed(key: *const c_char) -> i32 {
    if key.is_null() {
        return ESP_ERR_INVALID_ARG;
    }
    let Ok(key) = CStr::from_ptr(key).to_str() else { return ESP_ERR_INVALID_ARG };
    match remove_installed(&card_root().join("apps"), key) {
        Ok(()) => {
            crate::update_manager::apps_changed();
            ESP_OK
        }
        Err(error) => error,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU32, Ordering};

    static DIR_SEQUENCE: AtomicU32 = AtomicU32::new(0);

    fn apps_dir() -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "thistle-installed-{}-{}",
            std::process::id(),
            DIR_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn text(field: &[u8]) -> &str {
        let end = field.iter().position(|&b| b == 0).unwrap_or(field.len());
        std::str::from_utf8(&field[..end]).unwrap()
    }

    fn install_package(apps: &Path, id: &str, name: &str, sequence: u32) {
        let generation = apps.join(id).join("generations").join(sequence.to_string());
        std::fs::create_dir_all(&generation).unwrap();
        std::fs::write(generation.join("app.app.elf"), vec![0u8; 2048]).unwrap();
        std::fs::write(generation.join("app.app.elf.sig"), vec![0u8; 64]).unwrap();
        std::fs::write(
            generation.join("package.json"),
            format!("{{\"schema\":\"thistle.app.package/v1\",\"type\":\"app\",\"id\":\"{id}\",\"name\":\"{name}\",\"version\":\"1.{sequence}.0\",\"release_sequence\":{sequence},\"entry\":\"app.app.elf\"}}"),
        )
        .unwrap();
        std::fs::write(
            apps.join(id).join("active.json"),
            format!("{{\"release_sequence\": {sequence}, \"version\": \"1.{sequence}.0\"}}"),
        )
        .unwrap();
    }

    #[test]
    fn layout_matches_the_c_struct() {
        assert_eq!(std::mem::size_of::<InstalledApp>(), 216);
    }

    #[test]
    fn lists_elfs_once_and_active_packages_by_name() {
        let apps = apps_dir();
        // An ELF with its signature and manifest is one app, not three.
        std::fs::write(apps.join("hello.app.elf"), vec![0u8; 1000]).unwrap();
        std::fs::write(apps.join("hello.app.elf.sig"), vec![0u8; 64]).unwrap();
        std::fs::write(
            apps.join("hello.manifest.json"),
            r#"{"type":"app","id":"com.example.hello","name":"Hello World","version":"1.0.0"}"#,
        )
        .unwrap();
        // An ELF with no manifest is named by its file.
        std::fs::write(apps.join("bare.app.elf"), b"elf").unwrap();
        install_package(&apps, "com.thistle.notes", "Notes", 2);
        // Unpacked but never activated, and a download still staged.
        std::fs::create_dir_all(apps.join("com.thistle.half/generations/1")).unwrap();
        std::fs::write(apps.join(".x.app.elf.thistle-tmp-3"), b"").unwrap();

        let listed = list_installed(&apps);
        let rows: Vec<(&str, &str, &str, &str, u32, bool)> = listed
            .iter()
            .map(|a| (text(&a.key), text(&a.id), text(&a.name), text(&a.version), a.size_bytes, a.is_package))
            .collect();
        assert_eq!(
            rows,
            vec![
                ("bare", "bare", "bare", "", 3, false),
                ("hello", "com.example.hello", "Hello World", "1.0.0", 1000, false),
                ("com.thistle.notes", "com.thistle.notes", "Notes", "1.2.0", 2048, true),
            ]
        );
        std::fs::remove_dir_all(&apps).unwrap();
    }

    #[test]
    fn the_app_stores_installs_are_known_by_id_and_release_sequence() {
        let apps = apps_dir();
        install_package(&apps, "com.thistle.notes", "Notes", 2);
        install_package(&apps, "com.thistle.weather", "Weather", 1_000_000);
        // An ELF copied on has no release to update from.
        std::fs::write(apps.join("hello.app.elf"), b"elf").unwrap();
        // Unpacked but never activated.
        std::fs::create_dir_all(apps.join("com.thistle.half/generations/1")).unwrap();

        let mut releases = installed_releases(&apps);
        releases.sort_by(|a, b| a.id.cmp(&b.id));
        assert_eq!(
            releases,
            vec![
                InstalledRelease { id: "com.thistle.notes".into(), release_sequence: 2 },
                InstalledRelease { id: "com.thistle.weather".into(), release_sequence: 1_000_000 },
            ]
        );
        std::fs::remove_dir_all(&apps).unwrap();
    }

    #[test]
    fn removes_a_package_folder_or_an_elf_with_its_files() {
        let apps = apps_dir();
        install_package(&apps, "com.thistle.notes", "Notes", 1);
        for file in ["hello.app.elf", "hello.app.elf.sig", "hello.manifest.json", "other.app.elf"] {
            std::fs::write(apps.join(file), b"x").unwrap();
        }

        assert_eq!(remove_installed(&apps, "com.thistle.notes"), Ok(()));
        assert!(!apps.join("com.thistle.notes").exists());
        assert_eq!(remove_installed(&apps, "hello"), Ok(()));
        for file in ["hello.app.elf", "hello.app.elf.sig", "hello.manifest.json"] {
            assert!(!apps.join(file).exists(), "{file} left behind");
        }
        assert!(apps.join("other.app.elf").exists());
        assert_eq!(remove_installed(&apps, "hello"), Err(ESP_ERR_NOT_FOUND));
        std::fs::remove_dir_all(&apps).unwrap();
    }

    #[test]
    fn removal_stays_inside_the_apps_folder() {
        let apps = apps_dir();
        std::fs::create_dir_all(apps.join("inner")).unwrap();
        for key in ["", ".", "..", "../victim", "inner/..", "a\\b", ".hidden", &"x".repeat(64)] {
            assert_eq!(remove_installed(&apps, key), Err(ESP_ERR_INVALID_ARG), "{key:?}");
        }
        assert!(apps.join("inner").is_dir());
        std::fs::remove_dir_all(&apps).unwrap();
    }
}
